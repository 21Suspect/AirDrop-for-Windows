# AirDrop for Windows — desktop preview

For a fresh checkout, start with [INSTALL.md](INSTALL.md). Read [LIMITATIONS.md](LIMITATIONS.md) before enabling the radio.

A native WPF app for the existing Windows AX211 AirDrop port. The desktop app
runs without elevation. Only its temporary radio helper requests administrator
permission. This is a development preview, not a claim of flawless compatibility.

## Using the app

1. Open **AirDrop for Windows.exe** normally.
2. Click **Turn on** and accept the Windows administrator prompt.
3. Once the app says **Available**, send from the iPhone's native AirDrop sheet.
   Incoming files are accepted automatically by default. Enable **Ask before receiving**
   in Settings to approve each transfer; unattended offers then expire.
4. To send, set the phone to **Everyone for 10 Minutes**, keep it unlocked,
   select it in **Nearby devices**, choose or drop files, and click **Send**.
5. Minimize to the system tray to keep the session active. Turning AirDrop off
   or closing the app stops transfers, removes session firewall rules and
   restores the stock Intel Wi-Fi driver.

Settings choose the advertised name, receive folder and a 1/4/8 GiB limit.
Transfers have progress, cancellation and a local history with folder links.
The receive directory defaults to Pictures/AirDrop, with three flat categories:
Pictures, Videos and Files. UUID/camera filenames become Photo/Video/File plus a
local receive timestamp; meaningful names are preserved, extensions lowercased,
and collisions get a numeric suffix. No transfer subdirectories are created.
Files are validated in private staging before publication and are never opened
automatically. Received files retain Windows' downloaded-file mark.

The compact UI has one main view, settings and paginated recent transfers.
Devices use pages of three and history pages of five, with no scrolling panes.

Real iPhone-to-PC photo and video transfers have succeeded, including automatic
receiving. A real PC-to-iPhone photo transfer also succeeded; the user confirmed
the photo arrived and opened. The sender additionally has automated TLS tests.

Mac uploads can include Finder AppleDouble sidecars and differently normalized
Unicode filenames. The receiver now handles these, discards only bounded metadata
associated with accepted files, and preserves normal file contents. Automated
TLS tests cover both legacy gzip and DVZip. The repeat live Mac PDF transfer
succeeded; the user confirmed it arrived, stayed in Files, and opened.
Apple's newer six-digit AirDrop code pairing is not implemented; a sender that
requires this flow cannot currently complete pairing with the app.

## Isolation and recovery

- The WPF shell talks to its unelevated backend through private process pipes.
  There is no local HTTP control endpoint or cloud service.
- A desktop heartbeat is required every 15 seconds. The backend refreshes the
  radio lease; the native host stops after 20 seconds without renewal.
- An independent elevated restore watchdog checks for an abandoned session.
  It refuses to unload the driver unless DMA shutdown has been verified.
- Per-session state and original driver backups remain under
  %LOCALAPPDATA%/OpenDrop/sessions. An interrupted session is offered recovery
  before another one can start.
- A kernel lease and device-handle cleanup remain in force independently of
  the app. Some hardware faults can still require a restart.
- The file server binds only to the native AWDL IPv6 interface. Temporary
  firewall rules are restricted to that interface and local IPv6 traffic.
- Outgoing TLS pins the certificate seen during discovery for that transfer.
  Everyone-mode device names are self-reported, not Apple-account verified.
- A transfer is reported sent only after the remote receiver returns success.
  This confirms the AirDrop protocol response, not application-level opening.

## Current limits

The custom driver still requires Test Mode, the exact tested AX211 device,
working Ethernet and the local SDK/WDK signing tools. Wi-Fi cannot provide
ordinary networking while the radio is in AirDrop use. Channel 6 and the
verified MAC address are still fixed. The radio data path and throughput have
not been redesigned by the desktop app.

There is no Apple Contacts Only authentication, six-digit AirDrop code pairing,
Internet relay, folder/link
transfer, automatic resume or guaranteed Live Photo semantics. No independent
security audit, sleep/resume certification or long-duration hardware validation
has been completed. A preview GUI does not remove these limitations.

The app never changes boot integrity settings or buys a signing certificate.
For a general normal-mode release, Microsoft's driver-signing program requires
an EV-associated Hardware Developer account and the applicable certification:
https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings
A qualified sponsor/vendor could submit a reviewed driver; none has been arranged.

For internal use, Microsoft also documents Custom Kernel Signers on supported
Windows 11 24H2+ editions (excluding Home), with the required 2026 updates. This
can trust a locally created signing authority without an EV certificate, but
requires user-controlled Secure Boot PK/KEK trust, a signed App Control policy,
and a Windows reset/clean initial setup. Incorrect enrollment or loss of policy
signing keys can prevent boot. This app has not been adapted or tested for that
deployment; the current app still requires Test Mode. The application does not enroll firmware keys or deploy those policies.
https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers

## Building a local app

Install requirements-windows.txt plus pyinstaller==6.22.0 in a build venv.
Build the existing KMDF driver/helpers and Rust AWDL host as documented in
native/intel/README.md and native/awdl/README.md. Then run:

    python tools/package_windows_app.py --driver C:/build/kernel --radio C:/build/awdl-host.exe --firmware C:/firmware/iwx-so-a0-gf-a0-77 --pnvm C:/firmware/iwx-so-a0-gf-a0.pnvm --wintun C:/wintun/wintun.dll --output C:/build/OpenDrop

app/build.py uses the installed .NET Framework 4.8 compiler and Windows SDK
WinRT metadata. package_windows_app.py bundles the Python runtime so the
person running the app does not need a separate Python installation. The
temporary development-driver signing step still depends on SDK/WDK paths
in the supplied driver's build.json. This local package is not yet a general
installer or a public redistribution package.

## Protocol attribution

The sender's plist fields, HTTP sequence, type mappings, DVZip block sizing and
BLE wakeup frame were checked against the MIT-licensed Owly reference at
9a676e11d011c34ea32798a2541f2b1eeb66e731. Existing Owly and AX211 provenance
notes remain in the native source documentation.
The WPF UI and Python sender/controller are GPL-3.0-or-later like this port.
