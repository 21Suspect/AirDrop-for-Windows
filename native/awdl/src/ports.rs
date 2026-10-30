// SPDX-License-Identifier: GPL-3.0-or-later
use owly_awdl::driver::{Air, Received};
use owly_awdl::{Frame, Ipv6Packet};
use std::io;
use tokio::sync::mpsc;

/// Bounded, cancellation-safe connection between Owly and the radio worker.
pub struct RadioPort {
    received: mpsc::Receiver<io::Result<Received>>,
    transmit: mpsc::Sender<Frame>,
}
pub struct RadioWorker {
    pub received: mpsc::Sender<io::Result<Received>>,
    pub transmit: mpsc::Receiver<Frame>,
}
pub fn radio_port(capacity: usize) -> (RadioPort, RadioWorker) {
    let (received_tx, received) = mpsc::channel(capacity);
    let (transmit, transmit_rx) = mpsc::channel(capacity);
    (
        RadioPort { received, transmit },
        RadioWorker {
            received: received_tx,
            transmit: transmit_rx,
        },
    )
}

impl RadioWorker {
    /// Queue a decoded MPDU from the configured channel. False means filtered
    /// wireless traffic. On WouldBlock the original payload remains with the
    /// caller and can be retried; no partial frame was queued.
    pub fn try_receive_ax211_mpdu(&self, payload: &[u8], channel: u8) -> io::Result<bool> {
        let Some(frame) = crate::packet::decode_ax211_mpdu(payload)? else {
            return Ok(false);
        };
        if frame.channel != channel {
            return Ok(false);
        }
        self.received
            .try_send(Ok(frame.received))
            .map_err(|why| match why {
                mpsc::error::TrySendError::Full(_) => {
                    io::Error::new(io::ErrorKind::WouldBlock, "AX211 receive queue is full")
                }
                mpsc::error::TrySendError::Closed(_) => {
                    io::Error::new(io::ErrorKind::BrokenPipe, "AWDL receive port stopped")
                }
            })?;
        Ok(true)
    }
}

impl Air for RadioPort {
    async fn recv(&mut self) -> io::Result<Received> {
        tokio::select! {
            biased;
            _ = self.transmit.closed() => Err(io::Error::new(
                io::ErrorKind::BrokenPipe, "AX211 transmit worker stopped")),
            next = self.received.recv() => next.unwrap_or_else(|| Err(io::Error::new(
                io::ErrorKind::BrokenPipe, "AX211 receive worker stopped"))),
        }
    }
    fn send(&mut self, frame: Frame) -> Result<(), Frame> {
        match self.transmit.try_send(frame) {
            Ok(()) => Ok(()),
            Err(mpsc::error::TrySendError::Full(frame)) => Err(frame),
            Err(mpsc::error::TrySendError::Closed(_)) => {
                // The receive channel reports the terminal worker error.
                tracing::error!("AX211 transmit worker stopped");
                Ok(())
            }
        }
    }
}

/// Only bounded, complete IPv6 packets go from the Windows stack onto AWDL.
pub fn ipv6_packet(data: &[u8]) -> Option<Ipv6Packet> {
    if data.len() < 40 || data.len() > 1500 || data[0] >> 4 != 6 {
        return None;
    }
    let payload = usize::from(u16::from_be_bytes([data[4], data[5]]));
    if 40 + payload != data.len() {
        return None;
    }
    Ipv6Packet::from_ip(data.to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;
    use owly_awdl::{MacAddr, rate::TxRate};
    use std::time::Duration;
    #[tokio::test]
    async fn canceled_receive_preserves_the_next_frame_and_shutdown_surfaces() {
        let (mut port, worker) = radio_port(1);
        assert!(
            tokio::time::timeout(Duration::from_millis(1), port.recv())
                .await
                .is_err()
        );
        worker
            .received
            .send(Ok(Received {
                bytes: vec![1, 2],
                rssi: Some(-45),
            }))
            .await
            .unwrap();
        assert_eq!(port.recv().await.unwrap().bytes, [1, 2]);
        drop(worker);
        assert_eq!(
            port.recv().await.unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
    }
    #[tokio::test]
    async fn queue_pressure_returns_the_original_frame_for_retry() {
        let (mut port, mut worker) = radio_port(1);
        let frame = Frame {
            bytes: vec![0; 24],
            to: MacAddr([2; 6]),
            rate: TxRate::BASIC,
            no_ack: true,
        };
        assert!(port.send(frame.clone()).is_ok());
        assert_eq!(port.send(frame.clone()), Err(frame.clone()));
        assert_eq!(worker.transmit.recv().await, Some(frame.clone()));
        assert!(port.send(frame).is_ok());
    }
    #[tokio::test]
    async fn closed_transmit_side_fails_even_while_receive_sender_survives() {
        let (mut port, worker) = radio_port(1);
        let RadioWorker { received, transmit } = worker;
        drop(transmit);
        let failure = tokio::time::timeout(Duration::from_millis(100), port.recv())
            .await
            .unwrap()
            .unwrap_err();
        assert_eq!(failure.kind(), io::ErrorKind::BrokenPipe);
        drop(received);
    }
    #[test]
    fn packet_lengths_and_link_local_addresses_are_checked() {
        let a = MacAddr([2, 0, 0, 0, 0, 1]);
        let b = MacAddr([2, 0, 0, 0, 0, 2]);
        let good = Ipv6Packet::echo_request(a, b, 7, 1, b"hello");
        assert_eq!(ipv6_packet(&good.data), Some(good.clone()));
        for end in 0..good.data.len() {
            assert!(ipv6_packet(&good.data[..end]).is_none());
        }
        let mut trailing = good.data.clone();
        trailing.push(0);
        assert!(ipv6_packet(&trailing).is_none());
        let mut nonlocal = good.data;
        nonlocal[8] = 0x20;
        assert!(ipv6_packet(&nonlocal).is_none());
    }
}
