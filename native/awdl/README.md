# Windows adapters for Owly

Development code for a native Windows AX211/Wintun transport. **The user's
iPhone discovered this PC and sent a real image successfully.** The verified path
uses HT20 IBSS, Owly, Wintun and a separately unelevated Python receiver.
See ../../WINDOWS-STATUS.md for the test evidence and remaining limitations.

The portable owly-awdl dependency is pinned to
[9a676e11d011c34ea32798a2541f2b1eeb66e731](https://github.com/t4t5/owly/tree/9a676e11d011c34ea32798a2541f2b1eeb66e731).
Its MIT notice is retained in licenses/owly-MIT.txt. This adapter is
GPL-3.0-or-later. Intel/OpenBSD notices remain in ../intel; see
[PROVENANCE.md](../intel/PROVENANCE.md).

## Transport and lifecycle

- Bounded radio queues preserve pending frames until the kernel accepts them.
  A closed worker side becomes a terminal error.
- Shared C AX211 codecs validate descriptor bounds, strip receive padding/FCS,
  normalize hardware-deaggregated frames, and preserve signal/channel metadata.
- The AX211 worker owns an exclusive device handle, uses a high-resolution
  one-millisecond waitable timer, and continually renews the kernel's ten-second
  lease. Device close and lease expiry both stop firmware.
- Wintun uses a pinned official DLL, its own receive worker, bounded IPv6
  packets, a link-local address, MTU 1500 and no default route or forwarding.
- Bridge owns the Owly engine, AX211 worker and Wintun together. Shutdown joins
  the engine, closes the adapter and verifies firmware shutdown.
- awdl-host reports versioned JSON readiness to its parent. Any input byte or
  EOF requests shutdown; closing/crashing the parent closes its pipe.
- The receiver validates the reported PID, IPv6 address and interface index,
  notices host failures, and attempts all cleanup operations.

TX currently uses legacy OFDM rates and one NVM-permitted antenna. No Ack
unicast data is expressed in QoS control; ordinary DATA gains a QoS header.
The HT20 IBSS MAC supports incoming phone traffic; a complete iPhone transfer
passed. Individual over-the-air ACKs were not independently captured.
Development uses channel 6 and the
fixed local MAC 02:ad:21:00:00:01; this is not a release identity scheme.

## Build and test without administrator access

Requires Rust 1.88 or later and a C compiler (MSVC C++ Build Tools on Windows).
From the repository root:

    cargo test --locked --manifest-path native/awdl/Cargo.toml
    cargo clippy --locked --manifest-path native/awdl/Cargo.toml --all-targets -- -D warnings
    cargo fmt --manifest-path native/awdl/Cargo.toml -- --check
    cargo build --release --locked --manifest-path native/awdl/Cargo.toml --bins

Seven adapter tests pass on this Windows PC. Two real Owly engines discover
each other and exchange 20 IPv6 packets through simulated radio descriptors,
the shared C codecs and Windows radio queues. The 61 upstream Owly tests passed
earlier. These software tests do not prove radio or iPhone compatibility.
GitHub CI exercises software only; refer to the repository's Actions page for remote results.

## Native development tools

The matching AX211 development driver must already be installed to open its
administrator-only interface. Firmware, PNVM and Wintun are external, pinned
downloads. These executables do not install a Wi-Fi driver or request elevation.

    radio-check.exe firmware pnvm wintun.dll 30 monitor
    radio-check.exe firmware pnvm wintun.dll 30 ibss

radio-check uses the same Bridge startup/shutdown path as awdl-host. It bounds
runtime to 10..60 seconds, reports RX/TX/peer counters, and verifies shutdown.
The IBSS mode additionally requires a successful beacon notification.
Use Test-IntelDriver.ps1 -IbssRadio in ../intel to install temporarily,
run the check and restore Intel's driver.

    awdl-host.exe firmware pnvm wintun.dll "Windows PC"

Managed mode writes a ready event to stdout, then runs until its controlling
stdin closes or receives a byte. Stop/error events also use stdout; diagnostics
use stderr. Its readiness means local transport initialization only.

    awdl-host.exe firmware pnvm wintun.dll "Windows PC" --bounded 90 ready.json

Bounded development mode ignores stdin, writes readiness to the supplied file,
and stops after 10..120 seconds. It is intended for an elevated radio process
with a separately launched, unelevated receiver. ../../tools/Start-AirDropTest.ps1
coordinates the bounded path and temporary scoped firewall rules. This bounded
native-host/receiver path passed a real iPhone transfer. Both host modes request
the verified IBSS configuration.

## Hardware evidence

The separate netif-check test passed a real Windows IPv6 UDP exchange in both
directions over Wintun, then removed the temporary adapter. It does not use RF.

The continuous AX211 test completed 1,024 transmissions and 662 RX frames.
Explicit stop, device-handle closure and ten-second lease expiry all released
buffers after verified DMA shutdown.

Live Owly + AX211 + Wintun then ran for 30 seconds: 891 TX completions, 606 RX
frames, 19,701 exchanges, no receive-queue drops and a 6,385 microsecond maximum
polling gap. No AWDL peer was observed. This earlier run used the monitor
configuration. Intel's oem46.inf driver was restored with ProblemCode 0.

The IBSS prerequisite test passed with 904 TX completions, zero failures,
654 RX frames and a successful beacon. The subsequent bounded receiver test
observed one AWDL peer, 4,424 TX completions with zero failures, and 2,568 AWDL RX
frames. A 3,048,375-byte image was saved and fully decoded. Stock driver,
temporary adapter, certificates and firewall cleanup were independently checked.
