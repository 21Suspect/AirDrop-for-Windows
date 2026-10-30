// SPDX-License-Identifier: GPL-3.0-or-later
//! Hardware-independent integration check: real Windows IPv6 UDP through Wintun.
//! It does not start a radio or advertise AirDrop.
#[cfg(windows)]
use opendrop_awdl_windows::{
    owly_awdl::{Ipv6Packet, MacAddr, driver::Netif},
    wintun::WintunNetif,
};
#[cfg(windows)]
use std::{
    io,
    net::{SocketAddrV6, UdpSocket},
    path::PathBuf,
    time::Duration,
};

#[cfg(windows)]
fn udp_reply(request: &Ipv6Packet, payload: &[u8]) -> io::Result<Ipv6Packet> {
    let data = &request.data;
    if data.len() < 48 || data[6] != 17 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Expected UDP without extension headers",
        ));
    }
    let mut reply = vec![0u8; 48 + payload.len()];
    reply[0] = 0x60;
    let udp_len = (8 + payload.len()) as u16;
    reply[4..6].copy_from_slice(&udp_len.to_be_bytes());
    reply[6] = 17;
    reply[7] = 64;
    reply[8..24].copy_from_slice(&data[24..40]);
    reply[24..40].copy_from_slice(&data[8..24]);
    reply[40..42].copy_from_slice(&data[42..44]);
    reply[42..44].copy_from_slice(&data[40..42]);
    reply[44..46].copy_from_slice(&udp_len.to_be_bytes());
    reply[48..].copy_from_slice(payload);
    let mut pseudo = reply[8..40].to_vec();
    pseudo.extend_from_slice(&u32::from(udp_len).to_be_bytes());
    pseudo.extend_from_slice(&[0, 0, 0, 17]);
    pseudo.extend_from_slice(&reply[40..]);
    if !pseudo.len().is_multiple_of(2) {
        pseudo.push(0);
    }
    let mut sum: u32 = pseudo
        .as_chunks::<2>()
        .0
        .iter()
        .map(|p| u32::from(u16::from_be_bytes([p[0], p[1]])))
        .sum();
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    let checksum = !(sum as u16);
    reply[46..48].copy_from_slice(&if checksum == 0 { 0xffff } else { checksum }.to_be_bytes());
    Ok(Ipv6Packet {
        src: request.dst,
        dst: request.src,
        data: reply,
    })
}

#[cfg(windows)]
async fn check(dll: PathBuf) -> io::Result<()> {
    let a = MacAddr([2, 0, 0, 0, 0, 0x11]);
    let b = MacAddr([2, 0, 0, 0, 0, 0x12]);
    let mut netif = WintunNetif::create(&dll, &format!("OpenDrop-test-{}", std::process::id()))?;
    netif.configure(a)?;
    let index = netif.interface_index();
    let socket = UdpSocket::bind(SocketAddrV6::new(a.link_local(), 0, 0, index))?;
    socket.set_read_timeout(Some(Duration::from_secs(5)))?;
    socket.send_to(
        b"opendrop-outbound",
        SocketAddrV6::new(b.link_local(), 32124, 0, index),
    )?;
    let request = tokio::time::timeout(Duration::from_secs(8), async {
        loop {
            let packet = netif.recv().await?;
            if packet.src == a
                && packet.dst == b
                && packet.data.len() >= 48
                && packet.data[6] == 17
                && &packet.data[48..] == b"opendrop-outbound"
            {
                break Ok::<_, io::Error>(packet);
            }
        }
    })
    .await
    .map_err(|_| {
        io::Error::new(
            io::ErrorKind::TimedOut,
            "No outbound IPv6 UDP reached Wintun",
        )
    })??;
    let response = udp_reply(&request, b"opendrop-inbound")?;
    netif.inject(&response)?;
    let mut buffer = [0u8; 128];
    let (size, from) = socket.recv_from(&mut buffer)?;
    if &buffer[..size] != b"opendrop-inbound" || from.ip() != b.link_local() || from.port() != 32124
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "UDP reply did not match",
        ));
    }
    drop(socket);
    drop(netif);
    println!(
        "{{\"ok\":true,\"windows_udp_to_wintun\":true,\"wintun_to_windows_udp\":true,\"adapter_closed\":true,\"airdrop_tested\":false}}"
    );
    Ok(())
}
#[cfg(windows)]
fn main() {
    let result = (|| {
        let dll = std::env::args_os()
            .nth(1)
            .map(PathBuf::from)
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "Usage: netif-check <official-wintun.dll>",
                )
            })?;
        tokio::runtime::Builder::new_current_thread()
            .enable_time()
            .build()?
            .block_on(check(dll))
    })();
    if let Err(error) = result {
        eprintln!("Windows network adapter test failed: {error}");
        std::process::exit(1);
    }
}
#[cfg(not(windows))]
fn main() {
    eprintln!("This test requires Windows.");
    std::process::exit(1);
}
