// SPDX-License-Identifier: GPL-3.0-or-later
//! Generate a deterministic AWDL discovery fixture; this program performs no radio I/O.
use owly_awdl::{Action, Channel, Config, Input, MacAddr, Node, Presence, RadioConstraints, Time};
use std::{env, fs, io};

fn main() -> io::Result<()> {
    let path = env::args_os().nth(1).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "Usage: emit-test-frame output.bin",
        )
    })?;
    let address = MacAddr([0x02, 0xad, 0x21, 0, 0, 1]);
    let mut node = Node::new(
        Config::new(
            address,
            "OpenDrop radio test",
            RadioConstraints {
                channel: Channel(6),
                presence: Presence::Fixed,
            },
        ),
        Time(0),
    );
    for action in node.handle(Input::Timer(Time(0))) {
        if let Action::Transmit { frame, .. } = action
            && frame.to == MacAddr::BROADCAST
            && frame.bytes.first() == Some(&0xd0)
        {
            fs::write(path, &frame.bytes)?;
            println!(
                "Generated {}-byte AWDL discovery fixture; no radio I/O",
                frame.bytes.len()
            );
            return Ok(());
        }
    }
    Err(io::Error::other(
        "Owly emitted no broadcast discovery frame",
    ))
}
