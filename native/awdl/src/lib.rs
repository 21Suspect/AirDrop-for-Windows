// SPDX-License-Identifier: GPL-3.0-or-later
//! Windows adapters for the pinned Owly AWDL engine.
//! The AX211 radio must be running before connecting these ports to Owly.
pub use owly_awdl;
pub mod packet;
pub mod ports;
#[cfg(windows)]
pub mod wintun;

#[cfg(windows)]
pub mod ax211;

#[cfg(windows)]
pub mod bridge;
