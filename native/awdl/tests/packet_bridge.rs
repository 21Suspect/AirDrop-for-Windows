// SPDX-License-Identifier: GPL-3.0-or-later
//! Software-only integration: real Owly engines + Windows queue adapters +
//! the shared C TX/RX codec. Descriptor fixtures replace the radio. This
//! proves neither Windows DMA nor RF transmission nor iPhone compatibility.
use opendrop_awdl_windows::{
    packet::decode_ax211_mpdu,
    ports::{RadioPort, RadioWorker, radio_port},
};
use owly_awdl::{
    Channel, Config, Frame, Ipv6Packet, MacAddr, PeerInfo, Presence, RadioConstraints, TxRate,
    driver::{self, Air, Netif, Running},
};
use std::{
    ffi::{c_int, c_void},
    io,
    time::Duration,
};
use tokio::sync::{mpsc, watch};

#[repr(C)]
struct TxRequest {
    queue: u32,
    index: u32,
    rate_half_mbps: u32,
    antenna: u32,
    no_ack: u32,
    command_version: u32,
    notification_version: u32,
}
#[repr(C)]
#[derive(Default)]
struct TxInfo {
    slot_bytes: u32,
    frame_bytes: u32,
    header_bytes: u32,
    byte_count: u32,
    rate: u32,
}
unsafe extern "C" {
    fn iwl_tx_frame_build(
        request: *const TxRequest,
        frame: *const u8,
        bytes: usize,
        dma: u64,
        slot: *mut c_void,
        capacity: usize,
        tfd: *mut c_void,
        tfd_capacity: usize,
        info: *mut TxInfo,
    ) -> c_int;
}

/// Fixed dummy address is ONLY serialized. No driver, device, or DMA is opened.
fn simulated_tx(frame: &Frame) -> Vec<u8> {
    let rate = match frame.rate {
        TxRate::Legacy(rate) => u32::from(rate),
        _ => panic!("this backend is configured for legacy OFDM"),
    };
    let request = TxRequest {
        queue: 1,
        index: 0,
        rate_half_mbps: rate,
        antenna: 1,
        no_ack: u32::from(frame.no_ack),
        command_version: 10,
        notification_version: 7,
    };
    let mut slot = [0u8; 4096];
    let mut tfd = [0u8; 256];
    let mut info = TxInfo::default();
    // Arrays match the C capacities and repr(C) structs match iwl_packet.h.
    let result = unsafe {
        iwl_tx_frame_build(
            &request,
            frame.bytes.as_ptr(),
            frame.bytes.len(),
            0x1234_0000,
            slot.as_mut_ptr().cast(),
            slot.len(),
            tfd.as_mut_ptr().cast(),
            tfd.len(),
            &mut info,
        )
    };
    assert_eq!(result, 0, "the codec rejected an Owly-generated frame");
    let header = info.header_bytes as usize;
    let mut wire = slot[32..32 + header].to_vec();
    let payload = 32 + header.next_multiple_of(4);
    wire.extend_from_slice(&slot[payload..info.slot_bytes as usize]);
    assert_eq!(wire.len(), info.frame_bytes as usize);
    assert_eq!(wire.len(), info.byte_count as usize);
    wire
}
fn rx_fixture(wire: &[u8]) -> Vec<u8> {
    let header = if wire[0] & 0xfc == 0x88 { 26 } else { 24 };
    let padded = header == 26;
    let mut descriptor = vec![0u8; 64];
    let length = wire.len() + usize::from(padded) * 2 + 4;
    descriptor[..2].copy_from_slice(&(length as u16).to_le_bytes());
    descriptor[2] = 0x20; // four-byte FCS suffix reported by the fixture
    descriptor[3] = if padded { 0x20 } else { 0 };
    descriptor[12] = 3; // fixture CRC + FIFO status
    descriptor[36..40].copy_from_slice(&0x4102u32.to_le_bytes());
    descriptor[40] = 48;
    descriptor[41] = 43;
    descriptor[42] = 6;
    descriptor.extend_from_slice(&wire[..header]);
    if padded {
        descriptor.extend_from_slice(&[0xaa, 0xbb]);
    }
    descriptor.extend_from_slice(&wire[header..]);
    descriptor.extend_from_slice(&[0xde, 0xad, 0xbe, 0xef]);
    descriptor
}
fn mac(index: u8) -> MacAddr {
    MacAddr([2, 0, 0, 0, 0, index])
}

struct MemNetif {
    outbound: mpsc::Receiver<Ipv6Packet>,
    delivered: mpsc::Sender<Ipv6Packet>,
}
impl Netif for MemNetif {
    async fn recv(&mut self) -> io::Result<Ipv6Packet> {
        self.outbound
            .recv()
            .await
            .ok_or_else(|| io::Error::other("host closed"))
    }
    fn deliver(&mut self, packet: Ipv6Packet) {
        self.delivered
            .try_send(packet)
            .expect("test host queue has room");
    }
}
struct Host {
    running: Running,
    peers: watch::Receiver<Vec<PeerInfo>>,
    send: mpsc::Sender<Ipv6Packet>,
    delivered: mpsc::Receiver<Ipv6Packet>,
}
fn host(index: u8, port: RadioPort) -> Host {
    let mut config = Config::new(
        mac(index),
        &format!("windows-test-{index}"),
        RadioConstraints {
            channel: Channel(6),
            presence: Presence::Fixed,
        },
    );
    config.neighbour_echo = false;
    let (peers_tx, peers) = watch::channel(Vec::new());
    let (send, outbound) = mpsc::channel(64);
    let (delivered_tx, delivered) = mpsc::channel(64);
    let running = driver::spawn(
        config,
        move || {
            Ok((
                port,
                MemNetif {
                    outbound,
                    delivered: delivered_tx,
                },
            ))
        },
        peers_tx,
    )
    .unwrap();
    Host {
        running,
        peers,
        send,
        delivered,
    }
}
async fn link(mut a: RadioWorker, mut b: RadioWorker) -> usize {
    let mut frames = 0;
    loop {
        let (frame, target) = tokio::select! {
            frame = a.transmit.recv() => (frame, &b),
            frame = b.transmit.recv() => (frame, &a),
        };
        let Some(frame) = frame else {
            return frames;
        };
        let payload = rx_fixture(&simulated_tx(&frame));
        match target.try_receive_ax211_mpdu(&payload, 6) {
            Ok(true) => frames += 1,
            Err(e) if e.kind() == io::ErrorKind::BrokenPipe => return frames,
            result => panic!("software radio failed: {result:?}"),
        }
    }
}

#[tokio::test]
async fn two_awdl_nodes_exchange_ipv6_through_shared_c_tx_rx_and_windows_ports() {
    let (a_port, a_worker) = radio_port(64);
    let (b_port, b_worker) = radio_port(64);
    let mut a = host(1, a_port);
    let mut b = host(2, b_port);
    let medium = tokio::spawn(link(a_worker, b_worker));
    tokio::time::timeout(Duration::from_secs(5), async {
        a.peers
            .wait_for(|p| p.iter().any(|p| p.mac == mac(2)))
            .await
            .unwrap();
        b.peers
            .wait_for(|p| p.iter().any(|p| p.mac == mac(1)))
            .await
            .unwrap();
    })
    .await
    .expect("the software AWDL nodes must discover each other");
    assert_eq!(a.peers.borrow()[0].rssi, Some(-43));
    for sequence in 0..10 {
        let payload = vec![sequence as u8; 1400];
        let ab = Ipv6Packet::echo_request(mac(1), mac(2), 0xCAFE, sequence, &payload);
        let ba = Ipv6Packet::echo_request(mac(2), mac(1), 0xBEAD, sequence, &payload);
        a.send.send(ab.clone()).await.unwrap();
        b.send.send(ba.clone()).await.unwrap();
        let got_ab = tokio::time::timeout(Duration::from_secs(3), b.delivered.recv())
            .await
            .unwrap()
            .unwrap();
        let got_ba = tokio::time::timeout(Duration::from_secs(3), a.delivered.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(got_ab, ab);
        assert_eq!(got_ba, ba);
    }
    a.running.stop();
    b.running.stop();
    assert!(
        tokio::time::timeout(Duration::from_secs(1), medium)
            .await
            .unwrap()
            .unwrap()
            >= 22
    );
}

fn sample_payload() -> Vec<u8> {
    let packet = Ipv6Packet::echo_request(mac(1), mac(2), 1, 2, b"queue-test");
    let frame = Frame {
        bytes: owly_awdl::frame::data_frame(mac(1), mac(2), 4, 5, &packet.data),
        to: mac(2),
        rate: TxRate::BASIC,
        no_ack: true,
    };
    rx_fixture(&simulated_tx(&frame))
}

#[tokio::test]
async fn decoded_receive_queue_handles_cancellation_pressure_channel_and_corruption() {
    let (mut port, worker) = radio_port(1);
    let payload = sample_payload();
    let expected = decode_ax211_mpdu(&payload).unwrap().unwrap().received;
    assert!(!worker.try_receive_ax211_mpdu(&payload, 44).unwrap());
    assert!(
        tokio::time::timeout(Duration::from_millis(2), port.recv())
            .await
            .is_err()
    );
    assert!(worker.try_receive_ax211_mpdu(&payload, 6).unwrap());
    assert_eq!(
        worker
            .try_receive_ax211_mpdu(&payload, 6)
            .unwrap_err()
            .kind(),
        io::ErrorKind::WouldBlock
    );
    assert_eq!(port.recv().await.unwrap(), expected);
    assert!(worker.try_receive_ax211_mpdu(&payload, 6).unwrap());
    assert_eq!(port.recv().await.unwrap(), expected);
    let mut damaged = payload.clone();
    damaged[12] = 1; // overflow, despite good CRC
    assert!(!worker.try_receive_ax211_mpdu(&damaged, 6).unwrap());
    assert_eq!(
        worker
            .try_receive_ax211_mpdu(&payload[..65], 6)
            .unwrap_err()
            .kind(),
        io::ErrorKind::InvalidData
    );
    drop(port);
    assert_eq!(
        worker
            .try_receive_ax211_mpdu(&payload, 6)
            .unwrap_err()
            .kind(),
        io::ErrorKind::BrokenPipe
    );
}

#[test]
fn missing_or_out_of_range_signal_is_not_clamped_into_a_valid_peer() {
    let mut payload = sample_payload();
    payload[40] = 0;
    payload[41] = 0;
    assert_eq!(
        decode_ax211_mpdu(&payload).unwrap().unwrap().received.rssi,
        None
    );
    payload[40] = 240;
    assert_eq!(
        decode_ax211_mpdu(&payload).unwrap().unwrap().received.rssi,
        None
    );
}
