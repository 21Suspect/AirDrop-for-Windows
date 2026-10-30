// SPDX-License-Identifier: GPL-3.0-or-later
//! Managed radio process. Closing stdin stops Owly, Wintun and firmware.
//! The device must already have the development driver installed.
#[cfg(windows)]
fn run() -> std::io::Result<()> {
    use opendrop_awdl_windows::{ax211::LOCAL_MAC, bridge::Bridge};
    use std::{
        io::{self, Read, Write},
        path::PathBuf,
        sync::mpsc,
        thread,
        time::{Duration, Instant, SystemTime},
    };
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() != 4 && args.len() != 7 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Usage: awdl-host <firmware> <pnvm> <wintun.dll> <name> [--bounded <10..120 seconds> <ready.json> | --leased <heartbeat> <ready.json>]",
        ));
    }
    let name = args[3]
        .to_str()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "Invalid device name"))?;
    let lease = (args.len() == 7 && args[4] == "--leased").then(|| PathBuf::from(&args[5]));
    let ready_path = (args.len() == 7).then(|| PathBuf::from(&args[6]));
    let bounded = if args.len() == 7 && lease.is_none() {
        if args[4] != "--bounded" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Expected --bounded",
            ));
        }
        let seconds: u64 = args[5]
            .to_str()
            .and_then(|s| s.parse().ok())
            .filter(|s| (10..=120).contains(s))
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "Bound is 10..120 seconds")
            })?;
        Some((Duration::from_secs(seconds), PathBuf::from(&args[6])))
    } else {
        None
    };
    let bridge = Bridge::open(
        &PathBuf::from(&args[0]),
        &PathBuf::from(&args[1]),
        &PathBuf::from(&args[2]),
        name,
        true,
    )?;
    let ready = format!(
        "{{\"event\":\"ready\",\"version\":1,\"interface_index\":{},\
        \"address\":\"{}\",\"pid\":{},\"ibss_configured\":true}}",
        bridge.interface_index,
        LOCAL_MAC.link_local(),
        std::process::id()
    );
    if let Some(path) = &ready_path {
        // Bounded development mode publishes its ready state for a separately
        // launched, unelevated receiver. It never waits indefinitely for input.
        let temporary = path.with_extension("tmp");
        std::fs::write(&temporary, format!("{ready}\n"))?;
        std::fs::rename(temporary, path)?;
    }
    println!("{ready}");
    io::stdout().flush()?;
    let (stopped_tx, stopped_rx) = mpsc::sync_channel(1);
    if bounded.is_none() && lease.is_none() {
        thread::Builder::new()
            .name("controller-input".into())
            .spawn(move || {
                // A lost parent closes the pipe even when it cannot run cleanup.
                // This is a one-way stop channel: any byte or EOF requests stop.
                // Read a fixed buffer so a controller cannot grow this process's
                // memory by sending a line without a newline.
                let _ = io::stdin().lock().read(&mut [0u8; 1]);
                let _ = stopped_tx.send(());
            })?;
    }
    let deadline = bounded
        .as_ref()
        .map(|(duration, _)| Instant::now() + *duration);
    let mut failure = None;
    let mut next_stats = Instant::now();
    loop {
        if let Some(path) = &ready_path
            && Instant::now() >= next_stats
        {
            let stats = bridge.stats().driver;
            let text = format!(
                "{{\"peers\":{},\"tx_completed\":{},\"tx_failed\":{},\"rx_valid\":{},\"rx_awdl\":{},\"beacons_completed\":{}}}\n",
                bridge.peer_count(),
                stats.tx_completed,
                stats.tx_failed,
                stats.rx_valid,
                stats.rx_awdl,
                stats.beacons_completed
            );
            std::fs::write(path.with_extension("stats.json"), text)?;
            next_stats = Instant::now() + Duration::from_secs(2);
        }
        if let Some(path) = &lease {
            // User-mode owner refreshes this lease. A crash, app hang, or sleep
            // ends the session; no unbounded radio survives an absent controller.
            let fresh = std::fs::metadata(path)
                .and_then(|m| m.modified())
                .ok()
                .and_then(|mtime| SystemTime::now().duration_since(mtime).ok())
                .is_some_and(|age| age < Duration::from_secs(20));
            if !fresh || path.with_extension("stop").exists() {
                break;
            }
        }
        if let Some(why) = bridge.failure() {
            failure = Some(why);
            break;
        }
        if deadline.is_some_and(|end| Instant::now() >= end) {
            break;
        }
        if bounded.is_none() && lease.is_none() {
            match stopped_rx.recv_timeout(Duration::from_millis(200)) {
                Ok(()) | Err(mpsc::RecvTimeoutError::Disconnected) => break,
                Err(mpsc::RecvTimeoutError::Timeout) => {}
            }
        } else {
            thread::sleep(Duration::from_millis(200));
        }
    }
    let status = bridge.stop()?;
    println!(
        "{{\"event\":\"stopped\",\"buffers_released\":true,\
        \"tx_completed\":{},\"tx_failed\":{},\"rx_valid\":{},\"rx_awdl\":{},\
        \"beacons_completed\":{},\"last_beacon_status\":{}}}",
        status.tx_completed,
        status.tx_failed,
        status.rx_valid,
        status.rx_awdl,
        status.beacons_completed,
        status.last_beacon_status
    );
    if let Some(why) = failure {
        return Err(io::Error::other(why));
    }
    Ok(())
}
#[cfg(windows)]
fn main() {
    if let Err(error) = run() {
        eprintln!("AWDL radio host failed: {error}");
        println!("{{\"event\":\"error\",\"buffers_released\":false}}");
        std::process::exit(1);
    }
}
#[cfg(not(windows))]
fn main() {
    eprintln!("The AX211 radio host requires Windows.");
    std::process::exit(1);
}
