# Windows validation status

As of 2026-10-30, this is a working development preview on one Windows 11 x64 PC with the exact AX211 device documented in [INSTALL.md](INSTALL.md).

## Observed hardware transfers

| Direction | Result |
| --- | --- |
| iPhone → PC | Multiple real photos and a video received; image data decoded successfully |
| PC → iPhone | Photo arrived and opened, confirmed by the user |
| Mac → PC | PDF arrived, remained in Files and opened, confirmed by the user |

The phone was reported by its owner as iPhone 17 Pro Max / iOS 27.2; that version was not independently queried. The Mac's exact OS version was not established. This is evidence for these sessions, not a device/OS support matrix.

A roughly 6 MB image took approximately 28 seconds. File digests were recorded locally, but were not independently compared with source-device digests. Personal files, packet captures and machine-specific logs are excluded from this repository.

## Radio and lifecycle checks

- Hardware firmware startup, PNVM/NVM initialization and bounded RX/TX tests passed.
- Continuous tests exercised hardware queue wrap beyond 1,024 transmissions.
- Explicit stop, handle close and kernel lease expiry verified DMA shutdown in bounded tests.
- Wintun passed a real IPv6 UDP exchange before over-the-air integration.
- Normal desktop shutdown restored Intel's driver and removed temporary session firewall rules and the signing certificate.
- One approximately 4.5-minute session had 9,466 successful TX completions and zero failed TX. A later approximately 17-minute session also completed restoration.
- A complete controlled desktop-crash test, suspend/resume matrix and long-duration stress testing remain outstanding.

## Software checks

The Windows test suite includes real loopback TLS transfers, CPIO/gzip/DVZip decoding, size/path checks, consent, cancellation, organized output, macOS sidecars and Unicode normalization. Native tests exercise the portable C codecs and PowerShell process/receiver helpers. Rust tests exercise the AWDL bridge with simulated radio queues.

Publication checks on Windows: **86 Windows tests passed; 341 native/helper tests passed with 3 optional fixture tests skipped; 7 Rust tests passed.** Ruff, Rust formatting and Clippy passed. Both driver builds passed INF/catalog checks. A fresh desktop package built successfully using freshly verified firmware and Wintun downloads. The packaged app was not started for another hardware session during publication.

Run the commands in [INSTALL.md](INSTALL.md#software-checks-without-radio-access) for current counts. GitHub Actions runs software tests only; it cannot validate AX211 radio behavior, Windows driver installation, Bluetooth or an Apple transfer.

See [LIMITATIONS.md](LIMITATIONS.md) for unsupported features and deployment limits.

## Downloadable installer preview

The EXE bundles a prebuilt test-signed driver and all runtime components. Setup detects hardware, current boot requirements and connectivity, rejects unsupported hardware, and refuses installation while the desktop app is running. Removal also rejects unfinished recovery. Test Mode preparation is optional, explicitly confirmed and never restarts the machine.

Publication validation includes 100 Windows tests (staging, unsupported devices, 32-bit host detection, recovery state and damaged release manifests), 341 native/helper tests with 3 optional skips, and successful driver/app/installer compilation. The wizard is checked on the development PC. A clean-machine installation and a radio transfer using the newly packaged pre-signed driver path have not yet been validated. The earlier actual phone/Mac transfers used the same radio implementation through local developer signing.
