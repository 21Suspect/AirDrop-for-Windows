# AirDrop for Windows

An experimental Windows desktop app that sends and receives files through Apple's native AirDrop sheet, using an Intel AX211, a custom Windows driver, and [Owly](https://github.com/t4t5/owly).

**Working on one tested PC. Not a general Windows AirDrop release.** Real iPhone → PC photos/videos, PC → iPhone photos, and Mac → PC PDF transfers have succeeded. Compatibility with other computers and Apple software versions is unverified.

**[Installation and build instructions](INSTALL.md) · [Limitations](LIMITATIONS.md) · [App guide](DESKTOP-APP.md) · [Validation](WINDOWS-STATUS.md)**

## Before you install

- Windows 11 x64 and the exact AX211 hardware ID below are the only tested configuration.
- The custom kernel driver currently requires **Windows Test Mode**, administrator access, and Secure Boot off.
- **Ordinary Wi-Fi is unavailable while AirDrop is on.** A connected Ethernet adapter is required by the launcher.
- There is **no ready-to-install binary release**. Build locally with the Windows SDK/WDK, C++ tools, Rust and Python. The SDK/WDK must remain available for temporary driver signing.
- **Incoming files from anyone nearby are accepted automatically by default.** Turn on **Ask before receiving** in Settings if you want approval prompts.
- This is experimental kernel/radio software. Crashes, lost connectivity and recovery requiring a restart remain possible.

The exact current match is:
~~~text
PCI\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11
~~~
An adapter being called “Intel AX211” is not enough. Other subsystem IDs, Intel models, USB adapters and ARM PCs are unsupported by this build.

## What the app does

- Nearby device discovery, file selection and drag-and-drop sending.
- Automatic receiving or optional per-transfer approval.
- Progress, cancellation, recent transfers, settings and system tray operation.
- Compact native WPF interface with paginated lists.
- Flat **Pictures**, **Videos** and **Files** folders under the chosen receive directory.
- Readable timestamp names for UUID/camera filenames, preserved meaningful names and collision suffixes.
- Temporary radio sessions with driver backup, scoped firewall rules and attempted restoration of the original Intel driver on shutdown.

To receive, open the app, choose **Turn on**, approve the radio helper's administrator prompt and wait for **Available**. To send, make the Apple device discoverable using **Everyone for 10 Minutes**, choose it in the app and send files. See the [app guide](DESKTOP-APP.md).

## Main limitations

The radio is fixed to channel 6, uses a fixed development MAC and has low throughput. Only one nearby instance of this prototype should be active. Contacts Only, Apple-account ownership / “My Devices”, six-digit AirDrop code pairing, Internet relay, folder/URL offers and automatic resume are not implemented. Live Photo behavior is not guaranteed. There has been no independent security audit or broad sleep/resume, crash, long-duration or multi-device qualification. See [LIMITATIONS.md](LIMITATIONS.md) before changing your system.

## How it works

~~~text
Windows WPF app ↔ unelevated Python backend (mDNS + AirDrop HTTPS)
                                ↕
                         Wintun IPv6 interface
                                ↕
                         Owly AWDL engine
                                ↕
                 custom KMDF driver ↔ Intel AX211
~~~

Bluetooth supplies a discovery wakeup advertisement; file data travels over Wi-Fi. The elevated helper owns radio setup and restoration; the UI and file receiver stay unelevated. Installing only the Python package does not give Windows AWDL support.

## Development and attribution

Start with [INSTALL.md](INSTALL.md) for a complete local build. [Native notes](native/README.md) explain the lower layers. Automated tests exercise protocol validation and software queues; they do not establish hardware compatibility.

This fork builds on [seemoo-lab/OpenDrop](https://github.com/seemoo-lab/opendrop), base commit 11fe7ba7861093b302bc0637e8cb10adf2d29337, and uses [t4t5/Owly](https://github.com/t4t5/owly) at pinned commit 9a676e11d011c34ea32798a2541f2b1eeb66e731. Original source history, notices, [GPL license](LICENSE) and [upstream documentation](README-UPSTREAM.md) are retained. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and [Intel provenance](native/intel/PROVENANCE.md).

AirDrop is an Apple trademark. This project is not affiliated with or endorsed by Apple, Microsoft or Intel. There is no claim of official certification.
