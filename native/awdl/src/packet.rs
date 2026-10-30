// SPDX-License-Identifier: GPL-3.0-or-later
//! The same bounded AX211 MPDU decoder used by the native driver.
//! This formats already-received bytes; it does not access the Wi-Fi device.
use owly_awdl::driver::Received;
use std::{ffi::c_int, io};

#[repr(C)]
#[derive(Default)]
struct RxInfo {
    frame_bytes: u32,
    header_bytes: u32,
    channel: u32,
    rate_n_flags: u32,
    rssi_dbm: i32,
    phy_info: u32,
    device_time: u32,
    status: u32,
    deaggregated: u32,
}
unsafe extern "C" {
    fn iwl_rx_frame_decode(
        payload: *const u8,
        bytes: usize,
        frame: *mut u8,
        capacity: usize,
        info: *mut RxInfo,
    ) -> c_int;
}
#[derive(Debug)]
pub struct RxFrame {
    pub received: Received,
    pub channel: u8,
    pub device_time: u32,
    pub rate_n_flags: u32,
    pub deaggregated: bool,
}

/// Decode one firmware RX_MPDU payload. Unrelated or damaged wireless traffic
/// is Ok(None); a malformed descriptor is InvalidData. No FCS reaches Owly.
pub fn decode_ax211_mpdu(payload: &[u8]) -> io::Result<Option<RxFrame>> {
    let mut bytes = vec![0; 4024];
    let mut info = RxInfo::default();
    // All input/output buffers are separate and valid for their stated lengths.
    // The C implementation checks both lengths before reading/writing.
    let result = unsafe {
        iwl_rx_frame_decode(
            payload.as_ptr(),
            payload.len(),
            bytes.as_mut_ptr(),
            bytes.len(),
            &mut info,
        )
    };
    match result {
        0 => {
            let length = info.frame_bytes as usize;
            if length > bytes.len() {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Invalid RX decoder length",
                ));
            }
            bytes.truncate(length);
            Ok(Some(RxFrame {
                received: Received {
                    bytes,
                    rssi: info.rssi_dbm.try_into().ok(),
                },
                channel: info.channel as u8,
                device_time: info.device_time,
                rate_n_flags: info.rate_n_flags,
                deaggregated: info.deaggregated != 0,
            }))
        }
        2 | 3 => Ok(None),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Malformed AX211 RX descriptor",
        )),
    }
}
