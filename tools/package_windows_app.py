# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a local development app. Does not install a driver or change boot settings."""
import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    for key in ("driver", "firmware", "pnvm", "wintun", "radio", "output"):
        parser.add_argument("--" + key, required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    wintun_notice = args.wintun.parent / "WINTUN-LICENSE.txt"
    if not wintun_notice.is_file():
        wintun_notice = args.wintun.parent / "LICENSE.txt"
    notices = [
        (args.firmware.parent / "iwx-license", "Intel-firmware.txt"),
        (wintun_notice, "Wintun.txt"),
        (root / "native/awdl/licenses/owly-MIT.txt", "Owly-MIT.txt"),
    ]
    for source, _ in notices:
        if not source.is_file():
            raise FileNotFoundError("Keep the dependency license beside its binary: " + str(source))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="opendrop-app-build-"))
    spec = importlib.util.spec_from_file_location("desktop_build", root / "app/build.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.build(stage / "shell")
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--name", "OpenDrop.Backend",
                    "--distpath", str(stage / "dist"), "--workpath", str(stage / "work"),
                    "--specpath", str(stage), "--paths", str(root), "--collect-submodules", "zeroconf",
                    str(root / "tools/app_backend.py")], check=True)
    shutil.copytree(stage / "dist/OpenDrop.Backend", output / "backend", dirs_exist_ok=True)
    shutil.copytree(stage / "shell", output, dirs_exist_ok=True)
    driver = output / "assets/driver"
    driver.mkdir(parents=True, exist_ok=True)
    for name in ("opendrop_ax211_km.sys", "opendrop_ax211_km.inf", "build.json",
                 "ax211-probe.exe", "ax211-stage.exe", "radio-check.exe"):
        shutil.copy2(args.driver / name, driver / name)
    shutil.copy2(args.radio, driver / "awdl-host.exe")
    for file, name in ((args.firmware, "firmware.bin"), (args.pnvm, "pnvm.bin"), (args.wintun, "wintun.dll")):
        shutil.copy2(file, output / "assets" / name)
    intel = output / "assets/intel"
    intel.mkdir(exist_ok=True)
    for script in (root / "native/intel").glob("*.ps1"):
        shutil.copy2(script, intel / script.name)
    config = {"backend": "backend/OpenDrop.Backend.exe", "backend_args": [],
              "driver": "assets/driver", "intel_source": "assets/intel",
              "firmware": "assets/firmware.bin", "pnvm": "assets/pnvm.bin", "wintun": "assets/wintun.dll"}
    (output / "app-config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
    for name in ("LICENSE", "DESKTOP-APP.md"):
        if (root / name).is_file():
            shutil.copy2(root / name, output / name)
    (output / "licenses").mkdir(exist_ok=True)
    for source, name in notices:
        shutil.copy2(source, output / "licenses" / name)
    shutil.copy2(root / "native/intel/PROVENANCE.md", output / "PROVENANCE.md")
    (output / "READ-ME.txt").write_text(
        "AirDrop for Windows — local development preview\n\n"
        "Launch AirDrop for Windows.exe normally. Choose Turn on AirDrop, then accept Windows administrator permission.\n"
        "The custom AX211 driver currently needs Test Mode. No boot/security settings are changed by this app.\n"
        "Requires the existing Windows SDK/WDK on this development PC for temporary driver signing.\n"
        "Wi-Fi is reserved for AirDrop while enabled; Ethernet must remain connected.\n"
        "Incoming files are saved automatically; Settings can require approval. To send, set the iPhone to Everyone for 10 Minutes.\n"
        "Minimize to the system tray; closing the app stops AirDrop and restores Intel Wi-Fi.\n"
        "Files are sorted into Pictures, Videos and Files inside Pictures/AirDrop. Settings and diagnostics: %LOCALAPPDATA%/OpenDrop.\n"
        "Real iPhone transfers have worked in both directions. Mac code pairing and long-session reliability remain incomplete.\n",
        encoding="utf-8")
    print("Built:", output / "AirDrop for Windows.exe")


if __name__ == "__main__":
    main()
