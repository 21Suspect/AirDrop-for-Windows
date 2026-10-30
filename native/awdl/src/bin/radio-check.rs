// SPDX-License-Identifier: GPL-3.0-or-later
//! Bounded integration check: real AX211 <-> Owly <-> Windows Wintun.
//! It does not advertise an AirDrop service or claim an iPhone transfer.
#[cfg(windows)]
fn check() -> std::io::Result<()> {
    use opendrop_awdl_windows::bridge::Bridge;
    use std::{
        io,
        path::PathBuf,
        thread,
        time::{Duration, Instant},
    };
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() != 5 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Usage: radio-check <firmware> <pnvm> <wintun.dll> <seconds: 10..60> <monitor|ibss>",
        ));
    }
    let seconds: u64 = args[3]
        .to_str()
        .and_then(|s| s.parse().ok())
        .filter(|s| (10..=60).contains(s))
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "Duration must be 10..60 seconds",
            )
        })?;
    let ibss = match args[4].to_str() {
        Some("ibss") => true,
        Some("monitor") => false,
        _ => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Mode must be monitor or ibss",
            ));
        }
    };
    let firmware = PathBuf::from(&args[0]);
    let pnvm = PathBuf::from(&args[1]);
    let wintun = PathBuf::from(&args[2]);
    let bridge = Bridge::open(&firmware, &pnvm, &wintun, "OpenDrop Windows test", ibss)?;
    let index = bridge.interface_index;
    let mut failure = None;
    let mut max_peers = 0usize;
    let deadline = Instant::now() + Duration::from_secs(seconds);
    while Instant::now() < deadline {
        max_peers = max_peers.max(bridge.peer_count());
        if let Some(why) = bridge.failure() {
            failure = Some(why);
            break;
        }
        thread::sleep(Duration::from_millis(200));
    }
    let stats = bridge.stats();
    let status = bridge.stop()?; // Joins Owly/radio, closes Wintun and verifies DMA shutdown.
    if let Some(why) = failure {
        return Err(io::Error::other(why));
    }
    if status.tx_completed < 20 || status.tx_failed != 0 || status.rx_valid == 0 {
        return Err(io::Error::other(format!(
            "Insufficient live radio traffic: {status:?}"
        )));
    }
    if ibss
        && (status.ibss_configured != 1
            || status.beacons_completed == 0
            || status.last_beacon_status != 1)
    {
        return Err(io::Error::other(format!(
            "IBSS beacon transmission not confirmed: {status:?}"
        )));
    }
    println!(
        "{{\"ok\":true,\"buffers_released\":true,\"owly_live\":true,\"wintun_closed\":true,\
        \"interface_index\":{index},\"seconds\":{seconds},\"tx_submitted\":{},\
        \"tx_completed\":{},\"tx_failed\":{},\"rx_valid\":{},\"rx_awdl\":{},\
        \"valid_peers\":{max_peers},\"exchanges\":{},\"max_poll_gap_us\":{},\
        \"receive_queue_drops\":{},\"ibss_configured\":{},\"beacons_completed\":{},\"last_beacon_status\":{},\"iphone_transfer_verified\":false}}",
        status.tx_submitted,
        status.tx_completed,
        status.tx_failed,
        status.rx_valid,
        status.rx_awdl,
        status.exchanges,
        stats.max_poll_gap_us,
        stats.received_queue_drops,
        status.ibss_configured,
        status.beacons_completed,
        status.last_beacon_status,
    );
    Ok(())
}
#[cfg(windows)]
fn main() {
    if let Err(error) = check() {
        eprintln!("Live AX211/Owly test failed: {error}");
        // Recovery must independently probe the driver if no verified stop state
        // reached this process. Closing its handle still invokes kernel cleanup.
        println!("{{\"ok\":false,\"buffers_released\":false,\"iphone_transfer_verified\":false}}");
        std::process::exit(1);
    }
}
#[cfg(not(windows))]
fn main() {
    eprintln!("This integration check requires Windows.");
    std::process::exit(1);
}
