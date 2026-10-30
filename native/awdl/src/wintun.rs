// SPDX-License-Identifier: GPL-3.0-or-later
//! Dynamically loads the official, unmodified Wintun 0.14.1 AMD64 binary.
//! The receive worker owns its read cursor; sending is safe from the AWDL thread.
use owly_awdl::{Ipv6Packet, MacAddr, driver::Netif};
use sha2::{Digest, Sha256};
use std::{ffi::c_void, io, os::windows::ffi::OsStrExt, path::Path, ptr, sync::Arc, thread};
use tokio::sync::mpsc;
use windows_sys::Win32::{
    Foundation::{
        CloseHandle, ERROR_BUFFER_OVERFLOW, ERROR_NO_MORE_ITEMS, FreeLibrary, HANDLE, HMODULE,
        WAIT_OBJECT_0,
    },
    NetworkManagement::{IpHelper::ConvertInterfaceLuidToIndex, Ndis::NET_LUID_LH},
    System::{
        LibraryLoader::{
            GetProcAddress, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, LOAD_LIBRARY_SEARCH_SYSTEM32,
            LoadLibraryExW,
        },
        Threading::{
            CreateEventW, INFINITE, SetEvent, WaitForMultipleObjects, WaitForSingleObject,
        },
    },
};

const DLL_SHA256: &str = "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce";
type CreateAdapter =
    unsafe extern "system" fn(*const u16, *const u16, *const c_void) -> *mut c_void;
type CloseAdapter = unsafe extern "system" fn(*mut c_void);
type GetLuid = unsafe extern "system" fn(*mut c_void, *mut NET_LUID_LH);
type StartSession = unsafe extern "system" fn(*mut c_void, u32) -> *mut c_void;
type EndSession = unsafe extern "system" fn(*mut c_void);
type GetReadEvent = unsafe extern "system" fn(*mut c_void) -> HANDLE;
type ReceivePacket = unsafe extern "system" fn(*mut c_void, *mut u32) -> *mut u8;
type ReleasePacket = unsafe extern "system" fn(*mut c_void, *const u8);
type AllocatePacket = unsafe extern "system" fn(*mut c_void, u32) -> *mut u8;
type SendPacket = unsafe extern "system" fn(*mut c_void, *const u8);

struct Api {
    module: HMODULE,
    create: CreateAdapter,
    close: CloseAdapter,
    luid: GetLuid,
    start: StartSession,
    end: EndSession,
    event: GetReadEvent,
    receive: ReceivePacket,
    release: ReleasePacket,
    allocate: AllocatePacket,
    send: SendPacket,
}
// Wintun documents these entry points as thread-safe. Module lifetime covers all calls.
unsafe impl Send for Api {}
unsafe impl Sync for Api {}
impl Drop for Api {
    fn drop(&mut self) {
        unsafe {
            FreeLibrary(self.module);
        }
    }
}
impl Api {
    fn load(path: &Path) -> io::Result<Self> {
        let path = path.canonicalize()?;
        let hash = format!("{:x}", Sha256::digest(std::fs::read(&path)?));
        if hash != DLL_SHA256 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Expected the official Wintun 0.14.1 AMD64 DLL",
            ));
        }
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let module = unsafe {
            LoadLibraryExW(
                wide.as_ptr(),
                ptr::null_mut(),
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32,
            )
        };
        if module.is_null() {
            return Err(io::Error::last_os_error());
        }
        // On a missing export release the module instead of leaving it loaded.
        macro_rules! export {
            ($name:literal, $ty:ty) => {{
                let address = unsafe { GetProcAddress(module, concat!($name, "\0").as_ptr()) };
                match address {
                    Some(address) => unsafe {
                        std::mem::transmute::<unsafe extern "system" fn() -> isize, $ty>(address)
                    },
                    None => {
                        let error = io::Error::last_os_error();
                        unsafe {
                            FreeLibrary(module);
                        }
                        return Err(error);
                    }
                }
            }};
        }
        Ok(Self {
            module,
            create: export!("WintunCreateAdapter", CreateAdapter),
            close: export!("WintunCloseAdapter", CloseAdapter),
            luid: export!("WintunGetAdapterLUID", GetLuid),
            start: export!("WintunStartSession", StartSession),
            end: export!("WintunEndSession", EndSession),
            event: export!("WintunGetReadWaitEvent", GetReadEvent),
            receive: export!("WintunReceivePacket", ReceivePacket),
            release: export!("WintunReleaseReceivePacket", ReleasePacket),
            allocate: export!("WintunAllocateSendPacket", AllocatePacket),
            send: export!("WintunSendPacket", SendPacket),
        })
    }
}

struct Session {
    api: Api,
    adapter: *mut c_void,
    session: *mut c_void,
    index: u32,
}
// The receive side is used by exactly one worker. Wintun allows concurrent sends.
unsafe impl Send for Session {}
unsafe impl Sync for Session {}
impl Drop for Session {
    fn drop(&mut self) {
        unsafe {
            (self.api.end)(self.session);
            (self.api.close)(self.adapter);
        }
    }
}
struct StopEvent(HANDLE);
unsafe impl Send for StopEvent {}
unsafe impl Sync for StopEvent {}
impl Drop for StopEvent {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

/// The Wintun adapter lives only while this value is alive; dropping joins its worker.
pub struct WintunNetif {
    session: Arc<Session>,
    stop: Arc<StopEvent>,
    worker: Option<thread::JoinHandle<()>>,
    received: mpsc::Receiver<Ipv6Packet>,
    failure: Arc<std::sync::Mutex<Option<io::Error>>>,
}
impl WintunNetif {
    pub fn create(dll: &Path, name: &str) -> io::Result<Self> {
        if name.is_empty() || name.contains('\0') || name.encode_utf16().count() > 100 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Invalid adapter name",
            ));
        }
        let api = Api::load(dll)?;
        let wide: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
        let kind: Vec<u16> = "OpenDrop".encode_utf16().chain(Some(0)).collect();
        let adapter = unsafe { (api.create)(wide.as_ptr(), kind.as_ptr(), ptr::null()) };
        if adapter.is_null() {
            return Err(io::Error::last_os_error());
        }
        let mut luid: NET_LUID_LH = unsafe { std::mem::zeroed() };
        let mut index = 0;
        unsafe {
            (api.luid)(adapter, &mut luid);
        }
        let error = unsafe { ConvertInterfaceLuidToIndex(&luid, &mut index) };
        if error != 0 {
            unsafe {
                (api.close)(adapter);
            }
            return Err(io::Error::from_raw_os_error(error as i32));
        }
        let session = unsafe { (api.start)(adapter, 4 * 1024 * 1024) };
        if session.is_null() {
            let error = io::Error::last_os_error();
            unsafe {
                (api.close)(adapter);
            }
            return Err(error);
        }
        let session = Arc::new(Session {
            api,
            adapter,
            session,
            index,
        });
        let event = unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) };
        if event.is_null() {
            return Err(io::Error::last_os_error());
        }
        let stop = Arc::new(StopEvent(event));
        let (send, received) = mpsc::channel(512);
        let failure = Arc::new(std::sync::Mutex::new(None));
        let (reader, stopped, error_slot) = (session.clone(), stop.clone(), failure.clone());
        let worker = thread::Builder::new()
            .name("awdl-wintun".into())
            .spawn(move || {
                if let Err(error) = receive_loop(&reader, &stopped, send) {
                    *error_slot.lock().unwrap_or_else(|p| p.into_inner()) = Some(error);
                }
            })?;
        Ok(Self {
            session,
            stop,
            worker: Some(worker),
            received,
            failure,
        })
    }
    pub fn interface_index(&self) -> u32 {
        self.session.index
    }

    /// Configures only the adapter this object just created. No default route is added.
    pub fn configure(&self, mac: MacAddr) -> io::Result<()> {
        let address = mac.link_local();
        let script = format!(
            "$ErrorActionPreference='Stop'; \
             Set-NetIPInterface -InterfaceIndex {index} -AddressFamily IPv6 -NlMtuBytes 1500 -DadTransmits 0 -RouterDiscovery Disabled -Forwarding Disabled; \
             Get-NetIPAddress -InterfaceIndex {index} -AddressFamily IPv6 -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false; \
             New-NetIPAddress -InterfaceIndex {index} -AddressFamily IPv6 -IPAddress '{address}' -PrefixLength 64 -PolicyStore ActiveStore | Out-Null",
            index = self.session.index
        );
        use std::os::windows::process::CommandExt;
        let system_root =
            std::env::var_os("SystemRoot").ok_or_else(|| io::Error::other("SystemRoot missing"))?;
        let powershell =
            Path::new(&system_root).join("System32/WindowsPowerShell/v1.0/powershell.exe");
        let output = std::process::Command::new(powershell)
            .args(["-NoProfile", "-NonInteractive", "-Command", &script])
            .creation_flags(0x08000000)
            .output()?;
        if !output.status.success() {
            return Err(io::Error::other(
                String::from_utf8_lossy(&output.stderr).into_owned(),
            ));
        }
        Ok(())
    }
    pub fn inject(&self, packet: &Ipv6Packet) -> io::Result<()> {
        if crate::ports::ipv6_packet(&packet.data).is_none() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Incomplete or oversized AWDL IPv6 packet",
            ));
        }
        let buffer =
            unsafe { (self.session.api.allocate)(self.session.session, packet.data.len() as u32) };
        if buffer.is_null() {
            return Err(io::Error::last_os_error());
        }
        unsafe {
            ptr::copy_nonoverlapping(packet.data.as_ptr(), buffer, packet.data.len());
            (self.session.api.send)(self.session.session, buffer);
        }
        Ok(())
    }
}
fn receive_loop(
    session: &Session,
    stop: &StopEvent,
    send: mpsc::Sender<Ipv6Packet>,
) -> io::Result<()> {
    let ready = unsafe { (session.api.event)(session.session) };
    if ready.is_null() {
        return Err(io::Error::last_os_error());
    }
    let handles = [stop.0, ready];
    loop {
        if unsafe { WaitForSingleObject(stop.0, 0) } == WAIT_OBJECT_0 {
            return Ok(());
        }
        let mut bytes = 0;
        let packet = unsafe { (session.api.receive)(session.session, &mut bytes) };
        if packet.is_null() {
            let error = io::Error::last_os_error();
            if error.raw_os_error() != Some(ERROR_NO_MORE_ITEMS as i32) {
                return Err(error);
            }
            match unsafe { WaitForMultipleObjects(2, handles.as_ptr(), 0, INFINITE) } {
                WAIT_OBJECT_0 => return Ok(()),
                n if n == WAIT_OBJECT_0 + 1 => continue,
                _ => return Err(io::Error::last_os_error()),
            }
        }
        let ip = if bytes <= 1500 {
            crate::ports::ipv6_packet(unsafe { std::slice::from_raw_parts(packet, bytes as usize) })
        } else {
            None
        };
        unsafe {
            (session.api.release)(session.session, packet);
        }
        if let Some(ip) = ip {
            match send.try_send(ip) {
                Ok(()) | Err(mpsc::error::TrySendError::Full(_)) => {}
                Err(mpsc::error::TrySendError::Closed(_)) => return Ok(()),
            }
        }
    }
}
impl Netif for WintunNetif {
    async fn recv(&mut self) -> io::Result<Ipv6Packet> {
        self.received.recv().await.ok_or_else(|| {
            self.failure
                .lock()
                .unwrap_or_else(|p| p.into_inner())
                .take()
                .unwrap_or_else(|| {
                    io::Error::new(io::ErrorKind::BrokenPipe, "Wintun receiver stopped")
                })
        })
    }
    fn deliver(&mut self, packet: Ipv6Packet) {
        if let Err(error) = self.inject(&packet)
            && error.raw_os_error() != Some(ERROR_BUFFER_OVERFLOW as i32)
        {
            tracing::warn!("Wintun delivery failed: {error}");
        }
    }
}
impl Drop for WintunNetif {
    fn drop(&mut self) {
        unsafe {
            SetEvent(self.stop.0);
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        // Session and adapter close only after the last reader has exited.
    }
}
