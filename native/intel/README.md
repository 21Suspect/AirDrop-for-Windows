# AX211 Windows hardware development

The complete desktop preview now sends and receives real AirDrop transfers on the tested AX211. See [installation](../../INSTALL.md) and [current status](../../WINDOWS-STATUS.md). The notes below record earlier bring-up milestones, not the current feature list.

The development drivers can
read this AX211's identity registers and allocate Windows DMA common buffers.
Three fixed controller power cycles and the bounded firmware startup and
PNVM/NVM initialization tests passed on this AX211. Windows MSI-X delivers ALIVE
and command responses through RX DMA. The bounded radio reception test also passed;
a single-frame TX test also received a successful firmware completion.
Continuous packet I/O and a live Owly/Wintun session have also passed. The Owly adapter is in ../awdl.

Hardware verified on this PC: PCI 8086:7A70 / subsystem 00948086 / revision 11.
The UMDF tests and the post-restart KMDF test returned hardware revision 0x430,
RF identity 0x2010d000 and a 16 KiB MMIO resource. The KMDF test allocated a
1 MiB common buffer. It did not perform a hardware DMA transfer.

Each completed test restored Intel driver 23.150.0.4 (oem46.inf), removed its
development driver package and removed the temporary signing certificate.
The current app requires Test Mode. Boot settings must be managed explicitly; these historical tests do not describe the current boot configuration.

## Build

Install Microsoft C++ Build Tools and Windows SDK 26100. Extract the verified
Microsoft.Windows.WDK.x64 10.0.26100.6584 NuGet package outside the repository.
Its `c` directory is the WdkRoot below. These commands do not install a driver.
Use a writable build directory outside protected Documents folders.

~~~powershell
.\Build-IntelDriver.ps1 -WdkRoot C:\path\wdk\c -OutDir C:\path\umdf-build
.\Build-IntelDmaDriver.ps1 -WdkRoot C:\path\wdk\c -OutDir C:\path\kmdf-build
Copy-Item C:\path\umdf-build\ax211-probe.exe,C:\path\umdf-build\ax211-stage.exe C:\path\kmdf-build\
~~~

## Firmware preparation

`iwl_firmware.c` parses the published Intel TLV container with bounds on its
size, records, sections and paging layout. It returns offsets into the original
image. It does not modify firmware or execute it.

The image is not bundled. From the repository root, with its Python dependencies
installed, fetch the pinned SO/GF API-77 firmware and Intel license:

~~~powershell
python tools\fetch_intel_firmware.py C:\path\firmware
cmake -S native/intel -B C:\path\parser-build
cmake --build C:\path\parser-build --config Release
C:\path\parser-build\Release\firmware-inspect.exe C:\path\firmware\iwx-so-a0-gf-a0-77
$env:IWL_FW_LIBRARY = 'C:\path\parser-build\Release\iwl_fw_test.dll'
$env:IWL_FW_IMAGE = 'C:\path\firmware\iwx-so-a0-gf-a0-77'
python -m pytest tests/native -q
~~~

The downloader authenticates OpenBSD's signed manifest, verifies the package
checksum and pins the individual image, PNVM and license SHA256 values.
The firmware retains Intel's license; it is not GPL source. See PROVENANCE.md.
A changed signing key or a missing pinned package causes a failure.

The optional kernel staging request parses the image again, copies its sections
and image loader into Windows DMA common buffers, compares source and copy CRCs,
then releases the buffers. It never programs a device address or starts DMA.
The hardware test passed after the user approved retrying the administrator
prompt: 56 buffers, 1,620,940 bytes, equal source/copy CRC32 values (3851562140)
and rejection of a malformed image. Intel's driver was restored and the test
package and temporary signing certificate were removed. Firmware execution and
hardware DMA were not started. The 1 MiB allocation test above is separate.

## Hardware test

Run as Administrator only on this exact device, with Wi-Fi disconnected and
Ethernet up. Choose a new StatePath for every run. The test briefly replaces
Intel's driver, so Wi-Fi is unavailable during it. Nothing is installed at login.

~~~powershell
.\Test-IntelDriver.ps1 -DriverKind UMDF -BuildDirectory C:\path\umdf-build -StatePath C:\path\test-1\state.json
~~~

The test exports Intel's original driver, creates a short-lived local signing
certificate, starts a separate restoration watchdog, installs the test driver,
reads hardware, and restores the original driver in a finally block. The
watchdog also restores after 180 seconds if the test process is interrupted.
If a test cannot confirm DMA shutdown, buffers remain allocated and same-boot
restoration refuses to unload the driver until a health probe verifies DMA is
inactive. A verified later boot clears that retained-session condition; no
firmware starts automatically in PnP callbacks. No platform reset is issued.
The watchdog does not restart Windows; the original driver backup
and restoration script are retained. A failure to restore is reported as an error.

The KMDF variant uses `-DriverKind KMDF`. Add `-FirmwarePath` to request the
firmware-buffer check after the identity and 1 MiB allocation check:

~~~powershell
.\Test-IntelDriver.ps1 -DriverKind KMDF -BuildDirectory C:\path\kmdf-build -StatePath C:\path\test-2\state.json -FirmwarePath C:\path\firmware\iwx-so-a0-gf-a0-77
~~~

KMDF testing refuses to install unless Windows is already running in driver
test mode. Test mode permits locally signed development kernel drivers and
requires a restart to enter or leave. `Set-DriverTestMode.ps1` changes that one
boot option and saves a BCD backup; it never restarts Windows, changes Secure
Boot, or modifies BitLocker. It is not part of ordinary receiver startup.
Only use this development route after reading the current installation requirements and limitations.

Explicit restoration, if needed:

~~~powershell
.\Test-IntelDriver.ps1 -Mode Restore -StatePath C:\path\test-2\state.json
~~~

## Firmware and packet implementation

Three fixed power cycles wrote documented controller registers and verified
bus-master shutdown. They did not execute firmware. The new boot path starts
the image loader using Windows common-buffer addresses, polls for a valid ALIVE
notification for at most five seconds, and stops DMA before freeing buffers.
It passed on hardware after enabling MSI in the INF and registering a KMDF
MSI-X ISR: ALIVE 0xcafe, firmware 77, two interrupt callbacks, and verified
shutdown. The initial polling-only test timed out. No platform reset was used.

PNVM selection and firmware commands are connected to a bounded initialization
test (-InitializeFirmware with -FirmwarePath and -PnvmPath). It waits for ALIVE,
loads matching platform data, completes NVM initialization, reads capabilities
and channel restrictions, then stops DMA before freeing buffers. It passed:
13,716 PNVM bytes, three command round trips, INIT_COMPLETE, 110 channel entries,
seven receive notifications, and all 67 common buffers released.

iwl_packet.c implements bounded RX_MPDU decoding and TX command v10 formatting.
It removes receive padding/FCS and normalizes deaggregated frames. TX descriptors
use an explicit OFDM rate and support QoS No Ack, adding QoS control to ordinary
unicast DATA when necessary. These codecs do not configure or submit radio I/O.
PHY/regulatory configuration and continuous packet queues are implemented and
hardware-tested. The IBSS configuration and a real iPhone image transfer passed.
Individual over-the-air ACKs were not separately captured. Owly's portable AWDL tests pass on
Windows; the actual Wintun IPv6 UDP round trip has now passed.
The receive-only test passed with 125 valid channel-6 frames in five seconds;
a single-frame TX test then received firmware status SUCCESS. The later IBSS
test and full iPhone transfer are described below.

Only administrators and SYSTEM can open the device interface. It exposes fixed
identity fields, bounded development tests and a lease-limited AWDL session.
The session accepts only bounded AWDL frames and validated local addresses; it
does not expose arbitrary memory, physical addresses or register writes. The package matches one Intel subsystem;
it does not claim general Intel support.

## Receive-only development test

-MonitorReceive uses the same verified firmware/PNVM bundle and restoration
procedure. It queries the existing regulatory profile, requires channel 6 to be
valid, sets a 20 MHz listener MAC/PHY, enables Bluetooth coexistence and counts
frames for five seconds. It allocates no data transmit queue and leaves all
station aggregation disabled. Reports contain counts, not captured addresses or
payloads. A quiet channel is a failed reception test, not proof of working RF.

The fixed monitor command builders and MCC response decoder are covered by
native tests. The driver rejects unsupported firmware layouts. The continuous
AWDL worker uses channel 6. Its IBSS branch subsequently received a real iPhone
image through the native Windows receiver.

Measured monitor result: 240 RX notifications, 125 valid frames (92 beacons,
19 action frames), zero malformed frames, verified shutdown and Intel restoration.


-TransmitFrame additionally allocates one firmware management queue and submits
one fixed 299-byte AWDL discovery frame generated by the pinned Owly core. It
requires NVM and current MCC VALID/IBSS/ACTIVE flags and rejects radar channels.
The completed hardware test returned queue 1, TX status SUCCESS, 299 bytes at
6 Mbps, zero retries, then verified shutdown and released all 70 buffers.
No peer reception was observed. This fixture is not a running AWDL session.

The queue builder validates Windows DMA address alignment, overflow and overlap.
Completion parsing checks the owning queue and next descriptor index, including
the full 16-bit hardware pointer wrap independently of the 256 storage slots.

## Continuous radio validation (2026-10-30)

The continuous channel-6 session passed 1,024 firmware-confirmed transmissions
and 662 decoded receive frames. Explicit stop, closing the device handle and
10-second heartbeat expiry all quiesced DMA and released the buffers. Gen3's
16-bit hardware queue position is kept separate from its 256 storage slots.

The Windows Rust worker then ran the actual pinned Owly engine and Wintun for
30 seconds: 891 successful TX completions, 606 received frames, 19,701 exchanges,
no receive-queue drops, and a 6,385 microsecond maximum polling gap. The network
adapter was removed and Intel's oem46.inf driver restored with ProblemCode 0.
335 native tests and seven receiver-window tests pass; driver W4/WX and Clippy checks pass.

That earlier monitor-only run observed no peer. The corrected IBSS setup then
passed 904 transmissions, 654 RX frames and beacon confirmation. The full
receiver test subsequently received a real 3.05 MB iPhone image, with 4,424 TX
completions, zero failures and 2,568 AWDL receive frames. See ../../WINDOWS-STATUS.md.
The fixed development MAC is 02:ad:21:00:00:01; it is not a release identity scheme.

## IBSS and bounded receiver

-IbssRadio uses radio-check.exe, the verified Wintun DLL and the same pinned
firmware/PNVM. It adds a 10.24-second HT20 beacon template, IBSS MAC on the AWDL
BSSID with passive monitor filtering, one MAC/PHY binding, and separate
multicast data and broadcast management stations/queues. Beacon v13 follows
MAC creation; a second listener MAC is not created for the passive monitor.
Current NVM/MCC channel-6 transmit permissions are required before the beacon
can start. The firmware metadata must match the implemented command layouts.

The 30-second test requires RX/TX activity and a successful beacon notification.
It reports the firmware startup stage, command and status on failure.
Invoke-BoundedTool.ps1 preserves stdout/stderr separately, waits at most
90 seconds for the owned helper, and terminates that helper if it exceeds the
bound. Recovery independently probes DMA state when shutdown was not confirmed;
it never assumes that killing a process proves safe hardware shutdown.

Example, using a fresh state directory and already compiled matching binaries:

    ./Test-IntelDriver.ps1 -StatePath C:/path/ibss-test/state.json -BuildDirectory C:/path/kmdf-build -DriverKind KMDF -FirmwarePath C:/path/iwx-so-a0-gf-a0-77 -PnvmPath C:/path/iwx-so-a0-gf-a0.pnvm -IbssRadio -WintunPath C:/path/wintun.dll

The compiled radio-check.exe and probe/staging tools belong in BuildDirectory.
This test requires administrator access and the existing Test Mode boot.
It does not change boot policy or restart Windows. The matching awdl-host
process and Python receiver integration are described in ../awdl/README.md;
the bounded, privilege-separated path passed the iPhone transfer. -ReceiverTest
uses Invoke-ReceiverWindow.ps1 with a 120-second native-host bound, while the
180-second restoration watchdog also removes the saved exact firewall-rule names.
The cached read-only firmware fault report helped identify startup errors.
