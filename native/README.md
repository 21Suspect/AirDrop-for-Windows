# Native Windows radio implementation

The working preview combines a custom AX211 KMDF driver with a Rust [Owly/Wintun adapter](awdl/README.md). Real Apple device transfers have succeeded on the one supported hardware variant. See [current validation](../WINDOWS-STATUS.md), [limitations](../LIMITATIONS.md) and [complete build instructions](../INSTALL.md).

- [intel/README.md](intel/README.md): driver build and historical hardware bring-up.
- [intel/PROVENANCE.md](intel/PROVENANCE.md): pinned references and licensing.
- [awdl/README.md](awdl/README.md): Rust host, queues and lifecycle.
- radio_probe.c: the earlier stock Windows Native Wi-Fi API probe. The stock AX211 driver rejected monitor mode; this probe is not the custom radio backend.

Only the exact tested PCI subsystem is supported. The native layer is experimental and currently requires Test Mode. Building does not install drivers; app startup requests administrator approval separately.
