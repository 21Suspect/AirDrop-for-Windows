# Limitations of this preview

This describes the implementation published on 2026-10-30, not promised future features.

## Hardware and connectivity

| Area | Current behavior |
| --- | --- |
| Tested host | One Windows 11 x64 PC, Intel AX211 PCI 8086:7A70 / subsystem 00948086 / revision 11 |
| Other adapters | Not supported by the current exact-device match; changing the INF alone is insufficient |
| Networking | AX211 is reserved for AirDrop; ordinary Wi-Fi is unavailable until the Intel driver is restored |
| Ethernet | An active wired adapter is required by the helper |
| Radio | Fixed channel 6, fixed local MAC 02:ad:21:00:00:01, legacy OFDM, limited transmit concurrency |
| Multiple PCs | Fixed identities can collide; do not run multiple copies nearby |
| Speed | Much slower than native Apple-to-Apple AirDrop; one roughly 6 MB photo took about 28 seconds on the test setup |
| Range/interference | Channel congestion and peer scheduling can affect discovery, speed and reliability |
| Power management | Sleep/resume, hibernation, hot-unplug and long-duration operation are not qualified |

## Driver trust and installation

The app uses a custom KMDF kernel driver that is not Microsoft production-signed. The current launch path checks for Windows Test Mode and creates a short-lived local test-signing certificate. It requires the original SDK/WDK paths recorded in build.json, administrative permission and a disconnected target Wi-Fi adapter. There is no consumer installer, portable binary package, automatic updater or universal hardware support.

Test Mode relaxes the normal driver trust requirement. The current route needs Secure Boot off, displays a watermark and can conflict with software that enforces normal boot integrity. Turning Test Mode off prevents this driver from starting after reboot. Test Mode does not itself require turning off antivirus; this project does not instruct users to disable Defender.

Microsoft documents [test-signing behavior](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option) and [production/test driver signing offerings](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings). A general normal-mode release needs an appropriate trusted signing/deployment route; none has been completed here.

Microsoft's [Custom Kernel Signers](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers) provides another internal-use route on eligible updated Windows 11 editions. It requires user-controlled Secure Boot trust, signed App Control policy and reset/initial setup. It is not implemented or tested by this project and is not a switch for this app.

## AirDrop compatibility

- Everyone-mode discovery and transfer only. Device names are self-reported, not verified Apple identities.
- No Apple-account sign-in or “My Devices” / “Meine Geräte” recognition.
- No Contacts Only authentication.
- No six-digit AirDrop code pairing. If an Apple device requires that flow, the app cannot supply the code. Some observed transfers proceeded without it; this is not a reliable workaround.
- No Internet relay, automatic resume, directory offers or URL/link offers.
- Live Photos, photo-library metadata, albums and editing history are not guaranteed to retain Apple's full semantics.
- macOS AppleDouble metadata sidecars are discarded after validation; the main file is retained.
- Filenames that look like camera names or UUIDs become a media type plus local receive time. That timestamp is not necessarily the capture date.
- A “sent” result means the receiver returned protocol success. It does not prove that the other device opened or imported the file.
- Future iOS/macOS changes may break interoperability.

## Receiving and privacy

**Auto-accept is on by default.** Anyone nearby who can discover the receiver can offer files while it is enabled. Use Settings → Ask before receiving when that is not appropriate. Approval does not authenticate an Apple account.

The receiver bounds file counts and total payload, validates paths, stages content before publication, avoids overwriting existing files and adds the Windows downloaded-file mark. Files are not opened automatically. These checks are not malware scanning or an independent security audit. Transfer limits do not reserve free disk space.

The app has no cloud transfer service. Settings, TLS identity, history, logs and recovery state are local under %LOCALAPPDATA%/OpenDrop. Logs/history can contain device names, filenames and local paths; redact them before filing issues. Received files default to %USERPROFILE%/Pictures/AirDrop.

## Reliability and recovery

Normal stop/restoration has been checked on the test PC. Heartbeats, leases and a separate restoration watchdog are implemented, but a controlled complete app-crash test and broad hardware fault testing have not been completed. Firmware/DMA faults may require a restart; the helper deliberately refuses unsafe driver unload when it cannot verify shutdown.

Do not delete session records or original-driver backups while recovery is pending. See [recovery instructions](INSTALL.md#recovery-and-removal). No independent security audit, WHCP certification or production reliability claim is made.
