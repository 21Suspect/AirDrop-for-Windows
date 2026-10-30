// SPDX-License-Identifier: GPL-3.0-or-later
//! Own all three lifetimes together: Owly, the AX211 worker and Wintun.
use crate::{
    ax211::{self, LOCAL_MAC, Status, WorkerStats},
    wintun::WintunNetif,
};
use owly_awdl::{Channel, Config, PeerInfo, Presence, RadioConstraints, driver};
use std::{io, path::Path, sync::mpsc};
use tokio::sync::watch;

pub struct Bridge {
    engine: Option<driver::Running>,
    radio: Option<ax211::Running>,
    peers: watch::Receiver<Vec<PeerInfo>>,
    failed: watch::Receiver<Option<String>>,
    pub interface_index: u32,
}
impl Bridge {
    pub fn open(
        firmware: &Path,
        pnvm: &Path,
        wintun: &Path,
        name: &str,
        ibss: bool,
    ) -> io::Result<Self> {
        if name.is_empty() || name.len() > 63 || name.chars().any(char::is_control) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "The AWDL name must be 1..63 UTF-8 bytes without control characters",
            ));
        }
        let (firmware, pnvm, wintun) = (firmware.to_owned(), pnvm.to_owned(), wintun.to_owned());
        let adapter_name = format!("OpenDrop AWDL {}", std::process::id());
        let cfg = Config::new(
            LOCAL_MAC,
            name,
            RadioConstraints {
                channel: Channel(6),
                presence: Presence::Fixed,
            },
        );
        let (peers_tx, peers) = watch::channel(Vec::new());
        let (opened_tx, opened_rx) = mpsc::sync_channel(1);
        let engine = driver::spawn(
            cfg,
            move || {
                let netif = WintunNetif::create(&wintun, &adapter_name)?;
                netif.configure(LOCAL_MAC)?;
                let index = netif.interface_index();
                let (port, radio) = ax211::Running::start(&firmware, &pnvm, ibss)?;
                opened_tx
                    .send((radio, index))
                    .map_err(|_| io::Error::other("AWDL bridge controller stopped"))?;
                Ok((port, netif))
            },
            peers_tx,
        )?;
        let (radio, interface_index) = match opened_rx.recv() {
            Ok(opened) => opened,
            Err(_) => {
                engine.stop();
                return Err(io::Error::other("AWDL transport initialization ended"));
            }
        };
        let failed = engine.failed();
        Ok(Self {
            engine: Some(engine),
            radio: Some(radio),
            peers,
            failed,
            interface_index,
        })
    }
    pub fn failure(&self) -> Option<String> {
        self.failed.borrow().clone()
    }
    pub fn peer_count(&self) -> usize {
        self.peers.borrow().len()
    }
    pub fn stats(&self) -> WorkerStats {
        self.radio
            .as_ref()
            .map_or_else(WorkerStats::default, ax211::Running::stats)
    }
    fn shutdown(&mut self) -> io::Result<Status> {
        if let Some(engine) = self.engine.take() {
            engine.stop();
        }
        match self.radio.take() {
            Some(radio) => radio.stop(),
            None => Ok(Status::default()),
        }
    }
    pub fn stop(mut self) -> io::Result<Status> {
        self.shutdown()
    }
}
impl Drop for Bridge {
    fn drop(&mut self) {
        if let Err(error) = self.shutdown() {
            tracing::error!("AWDL bridge shutdown: {error}");
        }
    }
}
