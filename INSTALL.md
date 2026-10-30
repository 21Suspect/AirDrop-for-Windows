# Installation and local build

This is a **source-only developer preview**, not an installer for arbitrary Windows PCs. The full app must be built on the computer where it will run: its temporary driver-signing process still uses local SDK/WDK paths. Installing the PyPI opendrop package alone will not make Windows appear in AirDrop.

Read [LIMITATIONS.md](LIMITATIONS.md) first. No command in the build sections installs a driver or changes boot settings.

## 1. Check the hardware

The tested host is Windows 11 Pro x64 with this exact device match:

~~~text
PCI\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11
~~~

Inspect Device Manager → Network adapters → Intel Wi-Fi adapter → Properties → Details → Hardware Ids, or run this read-only PowerShell command:

~~~powershell
Get-PnpDevice -PresentOnly |
    Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11\*' } |
    Select-Object FriendlyName, Status, InstanceId
~~~

If nothing matches, stop: this driver does not support your hardware. Do not broaden the INF to force installation. Other AX211 subsystem/revision variants have not been validated.

You also need working Bluetooth and a connected Ethernet adapter. Disconnect the target Wi-Fi adapter from its network before starting a radio session; leave the hardware enabled. The launcher checks these conditions.

## 2. Install build prerequisites

Use official installers:

- [Git for Windows](https://git-scm.com/downloads/win).
- [Python](https://www.python.org/downloads/windows/), x64; 3.11 is the locally tested build version.
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with Desktop development with C++, MSVC x64/x86 tools and Windows SDK 10.0.26100.0. Local validation used MSVC 14.44.35207.
- [Rust](https://rustup.rs/), MSVC x64 toolchain, version 1.88 or newer.
- [CMake](https://cmake.org/download/) 3.20 or newer for native software tests.
- .NET Framework 4.8 and the Windows SDK WinRT metadata (the desktop shell uses the framework compiler).
- WDK NuGet package **Microsoft.Windows.WDK.x64 10.0.26100.6584**, extracted as below. The build scripts currently use the 26100 WDK layout and KMDF 1.33.

Restart your terminal after installing tools. Use a normal, unelevated **Developer PowerShell for VS 2022** with x64 build tools available. If your PowerShell policy prevents running local scripts, use an appropriate trusted development environment; this guide does not require a machine-wide policy bypass.

## 3. Get the source and dependencies

The examples keep all generated files outside the checkout. Run the following in the same PowerShell session, stopping if any command fails:

~~~powershell
$buildRoot = Join-Path $env:LOCALAPPDATA 'AirDropBuild'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
git clone https://github.com/21Suspect/AirDrop-for-Windows.git "$buildRoot\src"
Set-Location "$buildRoot\src"

py -3.11 -m venv "$buildRoot\venv"
$python = "$buildRoot\venv\Scripts\python.exe"
& $python -m pip install -e . pytest pyinstaller==6.22.0
~~~

Download the pinned WDK from NuGet, verify its SHA512 and extract it:

~~~powershell
$wdkZip = "$buildRoot\wdk.zip"
Invoke-WebRequest -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/10.0.26100.6584/microsoft.windows.wdk.x64.10.0.26100.6584.nupkg' -OutFile $wdkZip
$wdkHash = [Convert]::ToBase64String(
    [Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($wdkZip)))
if ($wdkHash -ne 'jhddaBnhMDqt3GVr32RVTtaR0KHmZDjY0JCTMn10OQpk4+AShXCK9QA09vBazp8ni1Mju84qM5SGXfIfnyOJ+g==') {
    throw 'WDK package checksum mismatch'
}
Expand-Archive -LiteralPath $wdkZip -DestinationPath "$buildRoot\wdk"
$wdkRoot = "$buildRoot\wdk\c"
~~~

Fetch the pinned Intel firmware/PNVM and official Wintun binary:

~~~powershell
& $python tools/fetch_intel_firmware.py "$buildRoot\firmware"
& $python tools/fetch_wintun.py "$buildRoot\wintun"
~~~

These downloaders retain dependency licenses and verify pinned hashes; the Intel downloader also authenticates OpenBSD's signed manifest. If a download is unavailable or validation fails, stop and report the error. Do not substitute arbitrary firmware or remove verification. Keep iwx-license and WINTUN-LICENSE.txt beside their respective downloads.

## 4. Build the driver, helpers and AWDL host

From the checkout root:

~~~powershell
& ./native/intel/Build-IntelDriver.ps1 -WdkRoot $wdkRoot -OutDir "$buildRoot\umdf"
& ./native/intel/Build-IntelDmaDriver.ps1 -WdkRoot $wdkRoot -OutDir "$buildRoot\kernel"
Copy-Item -LiteralPath "$buildRoot\umdf\ax211-probe.exe","$buildRoot\umdf\ax211-stage.exe" -Destination "$buildRoot\kernel"

$env:CARGO_TARGET_DIR = "$buildRoot\rust-target"
cargo build --release --locked --manifest-path native/awdl/Cargo.toml --bins
Copy-Item -LiteralPath "$buildRoot\rust-target\release\radio-check.exe" -Destination "$buildRoot\kernel"
~~~

The first script also builds an earlier UMDF prototype; only its probe/staging helpers are copied into the desktop package. The app uses the KMDF driver. Build scripts run INF verification/catalog generation but do not install either driver. Keep the build.json produced by the KMDF build; its WDK paths are needed at runtime.

## Software checks without radio access

~~~powershell
& $python -m pytest tests/windows -q
cmake -S native/intel -B "$buildRoot\native-tests"
cmake --build "$buildRoot\native-tests" --config Release
$env:IWL_FW_LIBRARY = "$buildRoot\native-tests\Release\iwl_fw_test.dll"
& $python -m pytest tests/native -q
cargo test --locked --manifest-path native/awdl/Cargo.toml
cargo fmt --manifest-path native/awdl/Cargo.toml -- --check
cargo clippy --locked --manifest-path native/awdl/Cargo.toml --all-targets -- -D warnings
~~~

These tests do not install drivers or prove over-the-air compatibility. If using a single-configuration CMake generator, adjust IWL_FW_LIBRARY to the actual DLL location.

## 5. Package the desktop app

~~~powershell
& $python tools/package_windows_app.py --driver "$buildRoot\kernel" --radio "$buildRoot\rust-target\release\awdl-host.exe" --firmware "$buildRoot\firmware\iwx-so-a0-gf-a0-77" --pnvm "$buildRoot\firmware\iwx-so-a0-gf-a0.pnvm" --wintun "$buildRoot\wintun\wintun.dll" --output "$buildRoot\app"
~~~

The package includes a Python runtime, WPF shell, matching driver/helper binaries and dependency notices. Leave its directory structure intact. Do not copy it to an arbitrary other PC and expect it to work: hardware checks and local signing-tool paths still apply.

## 6. Prepare a development boot

**This is a deliberate machine-security change, separate from building.** The app itself never enables Test Mode or disables Secure Boot.

The present driver needs Test Mode. Microsoft documents [enabling test-signed code](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option). Secure Boot blocks this route. Firmware settings and BitLocker recovery must be understood before any boot changes; this preview is best evaluated on a dedicated development installation. Do not disable antivirus for this app.

The supplied helper refuses to enable Test Mode if Secure Boot is enabled or the system volume is encrypted. It backs up BCD and does not reboot automatically. If those prerequisites already match your deliberately prepared development PC, run in **Administrator PowerShell**, from the checkout:

~~~powershell
./native/intel/Set-DriverTestMode.ps1 -Mode Enable -BackupDirectory "$env:LOCALAPPDATA\AirDropBootBackup"
~~~

Read the result and restart manually after saving work. If preflight or BCDEdit fails, stop and diagnose it; do not bypass the helper's safety checks. A successful build alone is not enough to start the driver.

## 7. First transfer

1. Connect Ethernet, disconnect Wi-Fi from its network and keep Bluetooth on.
2. Launch AirDrop for Windows.exe from the built app directory **normally**, not as administrator.
3. In Settings choose the advertised name, destination and whether to ask before receiving. **Auto-accept from anyone is the default.**
4. Choose **Turn on** and accept the radio helper's administrator prompt.
5. Wait for **Available**, open the Apple device's share sheet and choose the PC under AirDrop. Try a small disposable file first.
6. To send from Windows, put the Apple device in **Everyone for 10 Minutes**, keep it unlocked, select it in Nearby devices, choose files and press **Send**.
7. Files arrive in Pictures, Videos or Files under the configured folder. The default root is %USERPROFILE%/Pictures/AirDrop.
8. Choose **Turn off** or exit the app to stop the radio and restore Intel's Wi-Fi driver. Minimize keeps it active in the tray.

If the Apple device requests a six-digit code, that pairing flow is not supported. There is no code to display in this version. Consult [LIMITATIONS.md](LIMITATIONS.md) for other unsupported behavior.

## Recovery and removal

Wait for a normal stop before removing files. If startup reports a previous unfinished session, use the app's recovery flow. Retain %LOCALAPPDATA%/OpenDrop/sessions and the original driver backup until restoration is verified.

For manual recovery, locate the exact affected session directory. In Administrator PowerShell invoke its copied script with its own state.json:

~~~powershell
& 'C:\path\to\affected-session\src\Test-IntelDriver.ps1' -Mode Restore -StatePath 'C:\path\to\affected-session\state.json'
~~~

Do not use an unrelated session or guess an OEM INF number. If recovery reports unsafe DMA state, do not force-unload the driver. Save other work, restart as directed by the reported recovery condition, then retry recovery and confirm the stock Intel driver is restored.

After successful restoration and closing the app, remove the generated app/build directories if unwanted. Received files and %LOCALAPPDATA%/OpenDrop are separate and are retained unless you deliberately remove them.

To leave Test Mode, first stop/recover the app. Run the supplied helper with -Mode Disable from Administrator PowerShell and restart manually. The AirDrop custom driver will then stop working. Restore firmware security settings separately only after verifying recovery and your system's boot configuration; the app cannot supply a normal-mode signed driver.

## Troubleshooting

- **Expected one exact AX211:** unsupported hardware, missing device or wrong revision/subsystem.
- **Wi-Fi must be disconnected / Ethernet required:** satisfy the prerequisites rather than removing the checks.
- **WDK no longer available:** rebuild against the retained local WDK path and repackage.
- **Driver needs Test Mode:** confirm the current boot, not only a setting for the next restart.
- **PC not visible:** wait for Available, check Bluetooth and proximity, then retry discovery. Do not interpret software-test success as radio success.
- **Slow transfers:** expected in this prototype; fixed-channel scheduling and the radio path are limited.
- **Connection failed after a pairing prompt:** six-digit pairing is unsupported.
- **Recovery pending:** preserve the session directory and follow the recovery section.

For an issue, include Windows version, adapter hardware ID, app/source commit and a short reproduction. Redact local usernames, filenames, peer names, paths and certificates/keys from logs. Never upload received personal files or an entire session directory.
