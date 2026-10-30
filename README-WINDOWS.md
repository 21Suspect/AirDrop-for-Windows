# Windows documentation

Start with [README.md](README.md), then [INSTALL.md](INSTALL.md) for prerequisites, building, first use and recovery.

- [Desktop app guide](DESKTOP-APP.md)
- [Limitations](LIMITATIONS.md)
- [Validation status](WINDOWS-STATUS.md)
- [Native driver and AWDL architecture](native/README.md)

The Windows CLI is a developer interface, not an installer. It requires a real AWDL IPv6 interface; choosing an ordinary Wi-Fi or Ethernet adapter does not add AWDL support.

~~~powershell
python -m opendrop receive -i awdl0 -n "Windows PC" --output .\received
~~~

The CLI asks for consent unless --accept is supplied and uses per-transfer directories. The desktop app uses automatic reception by default and flat Pictures/Videos/Files categories.

For local protocol testing only:

~~~powershell
python -m opendrop receive --loopback-test --accept
~~~

Loopback mode binds only to ::1, disables discovery and cannot appear on an iPhone. Local TLS keys default to .opendrop-state; never publish that directory.
