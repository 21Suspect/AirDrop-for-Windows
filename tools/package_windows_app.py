# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a local development app. Does not install a driver or change boot settings."""
import argparse
import hashlib
import importlib.metadata
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import tomllib


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
    driver_files = ["opendrop_ax211_km.sys", "opendrop_ax211_km.inf",
                    "ax211-probe.exe", "ax211-stage.exe", "radio-check.exe"]
    release = (args.driver / "release-driver.json").is_file()
    if release:
        metadata = json.loads((args.driver / "release-driver.json").read_text(encoding="utf-8-sig"))
        expected = {"opendrop_ax211_km.sys", "opendrop_ax211_km.inf", "opendrop_ax211_km.cat", "release.cer"}
        if metadata.get("version") != 1 or metadata.get("signing") != "test-only" or set(metadata["files"]) != expected:
            raise ValueError("Invalid release driver metadata")
        for name in expected:
            if hashlib.sha256((args.driver / name).read_bytes()).hexdigest().lower() != metadata["files"][name].lower():
                raise ValueError("Release driver hash mismatch: " + name)
        driver_files += ["release-driver.json", "release.cer", "opendrop_ax211_km.cat"]
    else:
        driver_files += ["build.json"]
    for name in driver_files:
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
    for name in ("LICENSE", "DESKTOP-APP.md", "INSTALL.md", "LIMITATIONS.md", "THIRD-PARTY-NOTICES.md"):
        if (root / name).is_file():
            shutil.copy2(root / name, output / name)
    (output / "licenses").mkdir(exist_ok=True)
    for source, name in notices:
        shutil.copy2(source, output / "licenses" / name)
    shutil.copy2(root / "native/intel/PROVENANCE.md", output / "PROVENANCE.md")
    # Preserve notices from every installed distribution, including bundled runtimes.
    dependencies = []
    for distribution in importlib.metadata.distributions():
        package = distribution.metadata.get("Name", "unknown")
        dependencies.append({"name": package, "version": distribution.version})
        for entry in distribution.files or ():
            if entry.name.lower().startswith(("license", "copying", "notice", "authors")) and entry.suffix.lower() not in (".py", ".pyc"):
                source = Path(distribution.locate_file(entry))
                if source.is_file():
                    destination = output / "licenses" / "python" / package / Path(str(entry)).name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    # Keep distinct paths when a package has multiple same-named notices.
                    if destination.exists() and destination.read_bytes() != source.read_bytes():
                        destination = destination.with_name(hashlib.sha256(str(entry).encode()).hexdigest()[:8] + "-" + destination.name)
                    shutil.copy2(source, destination)
    (output / "licenses" / "python-packages.json").write_text(json.dumps(dependencies, indent=2), encoding="utf-8")
    cargo_lock = tomllib.loads((root / "native/awdl/Cargo.lock").read_text(encoding="utf-8"))
    cargo_sources = Path.home() / ".cargo/registry/src"
    for package in cargo_lock["package"]:
        if not package.get("source", "").startswith("registry+"):
            continue
        name = package["name"] + "-" + package["version"]
        candidates = list(cargo_sources.glob("*/" + name))
        if len(candidates) != 1:
            raise FileNotFoundError("Build the Rust host first to obtain dependency notices: " + name)
        crate_notices = [p for p in candidates[0].iterdir()
                         if p.is_file() and p.name.lower().startswith(("license", "copying", "notice"))]
        if not crate_notices:
            raise FileNotFoundError("Missing Rust dependency notices: " + name)
        destination = output / "licenses" / "rust" / name
        destination.mkdir(parents=True, exist_ok=True)
        for notice in crate_notices:
            shutil.copy2(notice, destination)
    rustc = shutil.which("rustc") or str(Path.home() / ".cargo/bin/rustc.exe")
    rust_root = Path(subprocess.check_output([rustc, "--print", "sysroot"], text=True).strip())
    rust_docs = rust_root / "share/doc/rust"
    shutil.copytree(rust_docs / "licenses", output / "licenses" / "rust-runtime", dirs_exist_ok=True)
    shutil.copy2(rust_docs / "COPYRIGHT-library.html", output / "licenses" / "rust-runtime")
    python_license = Path(sys.base_prefix) / "LICENSE.txt"
    if not python_license.is_file():
        raise FileNotFoundError("Python runtime license is missing")
    shutil.copy2(python_license, output / "licenses" / "Python.txt")
    if (root / "installer" / "Check-Prerequisites.ps1").is_file():
        setup = output / "assets" / "setup"
        setup.mkdir(exist_ok=True)
        shutil.copy2(root / "installer/Check-Prerequisites.ps1", setup)
    (output / "READ-ME.txt").write_text(
        "AirDrop for Windows — local development preview\n\n"
        "Launch AirDrop for Windows.exe normally. Choose Turn on AirDrop, then accept Windows administrator permission.\n"
        "The custom AX211 driver currently needs Test Mode. No boot/security settings are changed by this app.\n" +
        ("Prebuilt TEST-signed driver included; no SDK/WDK is needed. This does not enable normal-mode driver trust.\n" if release else "Requires local SDK/WDK for temporary driver signing.\n") +
        "Wi-Fi is reserved for AirDrop while enabled; Ethernet must remain connected.\n"
        "Incoming files are saved automatically; Settings can require approval. To send, set the iPhone to Everyone for 10 Minutes.\n"
        "Minimize to the system tray; closing the app stops AirDrop and restores Intel Wi-Fi.\n"
        "Files are sorted into Pictures, Videos and Files inside Pictures/AirDrop. Settings and diagnostics: %LOCALAPPDATA%/OpenDrop.\n"
        "Real iPhone transfers have worked in both directions. Mac code pairing and long-session reliability remain incomplete.\n",
        encoding="utf-8")
    print("Built:", output / "AirDrop for Windows.exe")


if __name__ == "__main__":
    main()
