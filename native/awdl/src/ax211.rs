// SPDX-License-Identifier: GPL-3.0-or-later
//! The administrator-only AX211 session. Driver installation is a separate,
//! explicit operation; opening this transport never replaces a Windows driver.
use crate::ports::{RadioPort, RadioWorker, radio_port};
use owly_awdl::{Frame, MacAddr, driver::Received, rate::TxRate};
use sha2::{Digest, Sha256};
use std::{
    ffi::c_void,
    fs::File,
    io::{self, Read},
    mem::size_of,
    os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
    path::Path,
    ptr::null,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::Instant,
};
use windows_sys::Win32::{
    Foundation::{INVALID_HANDLE_VALUE, WAIT_FAILED, WAIT_OBJECT_0},
    System::Threading::{
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, CreateWaitableTimerExW, SetWaitableTimerEx,
        TIMER_ALL_ACCESS, WaitForSingleObject,
    },
};

pub const LOCAL_MAC: MacAddr = MacAddr([2, 0xad, 0x21, 0, 0, 1]);
const TX_MAX: usize = 2304;
const RX_MAX: usize = 4024;
const RX_COUNT: usize = 8;
const FW_HASH: &str = "87590ea9535a8939fddb296bc7622f3a900f28f47b45cb4e435a2a1d5f496ccd";
const PNVM_HASH: &str = "b37f038b3467ed568d107a9c1dfd916fa7e8cbd58b2f9bec0c2c53297ccfcab7";

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Status {
    pub size: u32,
    pub version: u32,
    pub active: u32,
    pub kernel_status: u32,
    pub stop_reason: u32,
    pub buffers_released: u32,
    pub lease_ms: u32,
    pub uptime_ms: u32,
    pub exchanges: u32,
    pub tx_pending: u32,
    pub tx_submitted: u32,
    pub tx_completed: u32,
    pub tx_failed: u32,
    pub last_tx_status: u32,
    pub next_tx_index: u32,
    pub rx_valid: u32,
    pub rx_awdl: u32,
    pub rx_dropped: u32,
    pub ibss_configured: u32,
    pub beacons_completed: u32,
    pub last_beacon_status: u32,
}
#[repr(C)]
struct Input {
    size: u32,
    version: u32,
    tx_bytes: u32,
    rate_half_mbps: u32,
    no_ack: u32,
    frame: [u8; TX_MAX],
}
impl Input {
    fn empty() -> Self {
        Self {
            size: size_of::<Self>() as u32,
            version: 2,
            tx_bytes: 0,
            rate_half_mbps: 0,
            no_ack: 0,
            frame: [0; TX_MAX],
        }
    }
    fn set_frame(&mut self, frame: Option<&Frame>) -> io::Result<()> {
        self.tx_bytes = 0;
        if let Some(frame) = frame {
            if frame.bytes.len() > TX_MAX || frame.bytes.len() < 24 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "AWDL frame length",
                ));
            }
            let TxRate::Legacy(rate) = frame.rate else {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    "AX211 currently sends legacy OFDM rates",
                ));
            };
            if ![12, 18, 24, 36, 48, 72, 96, 108].contains(&rate) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "Invalid OFDM rate",
                ));
            }
            self.frame[..frame.bytes.len()].copy_from_slice(&frame.bytes);
            self.tx_bytes = frame.bytes.len() as u32;
            self.rate_half_mbps = u32::from(rate);
            self.no_ack = u32::from(frame.no_ack);
        }
        Ok(())
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
struct RxFrame {
    bytes: u32,
    rssi_dbm: i32,
    frame: [u8; RX_MAX],
}
#[repr(C)]
struct Output {
    status: Status,
    tx_accepted: u32,
    rx_count: u32,
    received: [RxFrame; RX_COUNT],
}
impl Output {
    fn new() -> Box<Self> {
        Box::new(Self {
            status: Status::default(),
            tx_accepted: 0,
            rx_count: 0,
            received: [RxFrame {
                bytes: 0,
                rssi_dbm: 0,
                frame: [0; RX_MAX],
            }; RX_COUNT],
        })
    }
}
#[repr(C)]
#[derive(Debug)]
struct Fault {
    size: u32,
    version: u32,
    observed: u32,
    read_mask: u32,
    lmac_address: u32,
    umac_address: u32,
    lmac: [u32; 38],
    umac: [u32; 15],
}
impl Default for Fault {
    fn default() -> Self {
        Self {
            size: 0,
            version: 0,
            observed: 0,
            read_mask: 0,
            lmac_address: 0,
            umac_address: 0,
            lmac: [0; 38],
            umac: [0; 15],
        }
    }
}
const _: () = assert!(size_of::<Fault>() == 236);

// Both sides assert fixed ABI sizes; no platform-sized fields cross the IOCTL.
const _: () = assert!(size_of::<Status>() == 84);
const _: () = assert!(size_of::<Input>() == 2324);
const _: () = assert!(size_of::<Output>() == 32348);

unsafe extern "C" {
    fn od_ax211_open(handle: *mut *mut c_void) -> u32;
    fn od_ax211_start(
        handle: *mut c_void,
        fw: *const u8,
        fw_bytes: u32,
        nv: *const u8,
        nv_bytes: u32,
        ibss: u32,
        state: *mut Status,
        startup: *mut [u32; 6],
    ) -> u32;
    fn od_ax211_exchange(handle: *mut c_void, input: *const Input, output: *mut Output) -> u32;
    fn od_ax211_stop(handle: *mut c_void, state: *mut Status) -> u32;
    fn od_ax211_fault(handle: *mut c_void, fault: *mut Fault) -> u32;
}
fn check(code: u32) -> io::Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(io::Error::from_raw_os_error(code as i32))
    }
}
fn verified_file(path: &Path, bytes: usize, hash: &str) -> io::Result<Vec<u8>> {
    let mut data = Vec::with_capacity(bytes);
    File::open(path)?
        .take(bytes as u64 + 1)
        .read_to_end(&mut data)?;
    if data.len() != bytes || format!("{:x}", Sha256::digest(&data)) != hash {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "Firmware does not match the pinned, authenticated image: {}",
                path.display()
            ),
        ));
    }
    Ok(data)
}
struct Device(OwnedHandle);
impl Device {
    fn start(fw: &[u8], nv: &[u8], ibss: bool) -> io::Result<Self> {
        let mut handle = std::ptr::null_mut();
        // SAFETY: valid out pointer, C opens one non-inheritable, exclusive handle.
        check(unsafe { od_ax211_open(&mut handle) })?;
        if handle.is_null() || handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::other("AX211 returned an invalid handle"));
        }
        // SAFETY: ownership transfers from od_ax211_open exactly once.
        let device = Self(unsafe { OwnedHandle::from_raw_handle(handle) });
        let mut state = Status::default();
        let mut startup = [0; 6];
        // SAFETY: borrowed byte slices and ABI-compatible output live through this synchronous call.
        let code = unsafe {
            od_ax211_start(
                device.0.as_raw_handle(),
                fw.as_ptr(),
                fw.len() as u32,
                nv.as_ptr(),
                nv.len() as u32,
                u32::from(ibss),
                &mut state,
                &mut startup,
            )
        };
        if code != 0 {
            let mut fault = Fault::default();
            // SAFETY: fixed ABI-compatible output and an exclusively owned handle.
            let fault_code = unsafe { od_ax211_fault(device.0.as_raw_handle(), &mut fault) };
            eprintln!("AX211 firmware fault (query {fault_code}): {fault:x?}");
            return Err(io::Error::other(format!(
                "AX211 start failed: Win32 {code}, NTSTATUS {:#x}, stage {}, command {}, response {:#x}, last packet {:#x}, MSI {:#x}, FH {:#x}",
                state.kernel_status,
                startup[0],
                startup[1],
                startup[2],
                startup[3],
                startup[4],
                startup[5]
            )));
        }
        Ok(device)
    }
    fn exchange(&self, input: &Input, output: &mut Output) -> io::Result<()> {
        // SAFETY: distinct, correctly sized wire buffers live for the entire IOCTL.
        check(unsafe { od_ax211_exchange(self.0.as_raw_handle(), input, output) })
    }
    fn stop(&self) -> io::Result<Status> {
        let mut state = Status::default();
        // SAFETY: exclusive owned handle and a valid output structure.
        check(unsafe { od_ax211_stop(self.0.as_raw_handle(), &mut state) })?;
        if state.active != 0 || state.buffers_released == 0 || state.kernel_status != 0 {
            return Err(io::Error::other(format!("AX211 stop state: {state:?}")));
        }
        Ok(state)
    }
}
impl Drop for Device {
    fn drop(&mut self) {
        if let Err(error) = self.stop() {
            tracing::error!("AX211 shutdown: {error}");
        }
        // Closing the owned handle also invokes the driver's file-cleanup path.
    }
}
struct PollTimer(OwnedHandle);
impl PollTimer {
    fn new() -> io::Result<Self> {
        // SAFETY: unnamed, non-inheritable timer, no pointers retained by Windows.
        let handle = unsafe {
            CreateWaitableTimerExW(
                null(),
                null(),
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS,
            )
        };
        if handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: newly created handle transfers into RAII ownership.
        Ok(Self(unsafe { OwnedHandle::from_raw_handle(handle) }))
    }
    fn wait(&self) -> io::Result<()> {
        let due = -10_000i64; // One millisecond, relative, no global timer-resolution change.
        // SAFETY: owned timer, synchronous setup, no APC and no wake-from-sleep request.
        if unsafe { SetWaitableTimerEx(self.0.as_raw_handle(), &due, 0, None, null(), null(), 0) }
            == 0
        {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: valid timer handle. Bound the wait even if Windows stops signaling.
        match unsafe { WaitForSingleObject(self.0.as_raw_handle(), 1000) } {
            WAIT_OBJECT_0 => Ok(()),
            WAIT_FAILED => Err(io::Error::last_os_error()),
            _ => Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "AX211 polling timer",
            )),
        }
    }
}
#[derive(Clone, Debug, Default)]
pub struct WorkerStats {
    pub driver: Status,
    pub received_queue_drops: u64,
    pub max_poll_gap_us: u64,
}
pub struct Running {
    stopped: Arc<AtomicBool>,
    stats: Arc<Mutex<WorkerStats>>,
    thread: Option<JoinHandle<io::Result<Status>>>,
}
impl Running {
    pub fn start(firmware: &Path, pnvm: &Path, ibss: bool) -> io::Result<(RadioPort, Self)> {
        let fw = verified_file(firmware, 1_641_260, FW_HASH)?;
        let nv = verified_file(pnvm, 55_208, PNVM_HASH)?;
        let (port, mut worker) = radio_port(8);
        let stopped = Arc::new(AtomicBool::new(false));
        let stats = Arc::new(Mutex::new(WorkerStats::default()));
        let stop_worker = stopped.clone();
        let shared = stats.clone();
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name("ax211-radio".into())
            .spawn(move || {
                let result = (|| {
                    let timer = PollTimer::new()?;
                    let device = Device::start(&fw, &nv, ibss)?;
                    let _ = ready_tx.send(Ok(()));
                    run(&device, &timer, &mut worker, &stop_worker, &shared)
                })();
                if let Err(error) = &result {
                    let message = error.to_string();
                    let _ = ready_tx.try_send(Err(io::Error::other(message.clone())));
                    let _ = worker.received.try_send(Err(io::Error::other(message)));
                }
                result
            })?;
        let mut running = Self {
            stopped,
            stats,
            thread: Some(thread),
        };
        match ready_rx.recv() {
            Ok(Ok(())) => Ok((port, running)),
            Ok(Err(error)) => {
                let _ = running.join();
                Err(error)
            }
            Err(_) => {
                let _ = running.join();
                Err(io::Error::other("AX211 worker ended at startup"))
            }
        }
    }
    pub fn stats(&self) -> WorkerStats {
        self.stats
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }
    fn join(&mut self) -> io::Result<Status> {
        self.stopped.store(true, Ordering::Release);
        match self.thread.take() {
            Some(thread) => thread
                .join()
                .map_err(|_| io::Error::other("AX211 worker panicked"))?,
            None => Ok(self.stats().driver),
        }
    }
    pub fn stop(mut self) -> io::Result<Status> {
        self.join()
    }
}
impl Drop for Running {
    fn drop(&mut self) {
        if let Err(error) = self.join() {
            tracing::error!("AX211 worker: {error}");
        }
    }
}
fn run(
    device: &Device,
    timer: &PollTimer,
    worker: &mut RadioWorker,
    stopped: &AtomicBool,
    shared: &Mutex<WorkerStats>,
) -> io::Result<Status> {
    let mut output = Output::new();
    let mut input = Input::empty();
    let mut pending: Option<Frame> = None;
    let mut stats = WorkerStats::default();
    let mut last_poll = Instant::now();
    let result = (|| {
        while !stopped.load(Ordering::Acquire) && !worker.received.is_closed() {
            if pending.is_none() {
                match worker.transmit.try_recv() {
                    Ok(frame) => pending = Some(frame),
                    Err(tokio::sync::mpsc::error::TryRecvError::Empty) => {}
                    Err(tokio::sync::mpsc::error::TryRecvError::Disconnected) => break,
                }
            }
            input.set_frame(pending.as_ref())?;
            let now = Instant::now();
            stats.max_poll_gap_us = stats.max_poll_gap_us.max(
                now.duration_since(last_poll)
                    .as_micros()
                    .min(u64::MAX as u128) as u64,
            );
            last_poll = now;
            device.exchange(&input, &mut output)?;
            stats.driver = output.status;
            if output.status.active == 0 || output.status.kernel_status != 0 {
                *shared
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner()) = stats.clone();
            }
            if output.status.active == 0 || output.status.kernel_status != 0 {
                return Err(io::Error::other(format!(
                    "AX211 session stopped: {:?}",
                    output.status
                )));
            }
            if output.tx_accepted != 0 {
                pending = None;
            }
            for frame in output.received.iter().take(output.rx_count as usize) {
                let bytes = frame.bytes as usize;
                if !(24..=RX_MAX).contains(&bytes) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "AX211 received frame length",
                    ));
                }
                let received = Received {
                    bytes: frame.frame[..bytes].to_vec(),
                    rssi: i8::try_from(frame.rssi_dbm).ok(),
                };
                if worker.received.try_send(Ok(received)).is_err() {
                    stats.received_queue_drops += 1;
                }
            }
            *shared
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner()) = stats.clone();
            timer.wait()?;
        }
        Ok(())
    })();
    let stopped = device.stop();
    if let Ok(state) = &stopped {
        stats.driver = *state;
        *shared
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = stats;
    }
    result?;
    stopped
}
