# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise installer detection and package staging without changing system state."""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from opendrop.windows.app_backend import Controller

ROOT = Path(__file__).resolve().parents[2]
PS = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


@pytest.mark.parametrize("release", [True, False])
def test_session_stages_matching_package_without_private_keys_or_build_tools(tmp_path, release):
    source = tmp_path / "driver"
    source.mkdir()
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    names = ["opendrop_ax211_km.sys", "opendrop_ax211_km.inf", "ax211-probe.exe",
             "ax211-stage.exe", "awdl-host.exe", "radio-check.exe"]
    names += ["release-driver.json", "release.cer", "opendrop_ax211_km.cat"] if release else ["build.json"]
    for name in names:
        (source / name).write_bytes(b"fixture")
    (source / "never-copy.pfx").write_bytes(b"private key placeholder")
    controller = Controller({"driver": str(source), "intel_source": str(scripts),
                                 "firmware": "firmware", "pnvm": "pnvm", "wintun": "wintun"}, tmp_path / "data",
                            lambda *args, **kwargs: None)
    try:
        session = controller._stage()
        assert {p.name for p in (session / "kernel").iterdir()} == set(names)
        assert json.loads((session / "config.json").read_text())["name"] == "Windows PC"
    finally:
        controller.close()


@pytest.mark.skipif(sys.platform != "win32", reason="Windows setup detector")
@pytest.mark.parametrize("supported", [True, False])
@pytest.mark.parametrize("wow64", [True, False])
def test_installer_detection_rejects_other_hardware_without_installation(tmp_path, supported, wow64):
    report = tmp_path / "report.ini"
    device = "'PCI\\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11\\fixture'" if supported else "'PCI\\VEN_1234&DEV_5678\\fixture'"
    script = tmp_path / "check.ps1"
    script.write_text(
        "function Get-PnpDevice { param([switch]$PresentOnly,[string]$Class) "
        "if($Class -eq 'Bluetooth') { return [pscustomobject]@{Status='OK'} }; "
        f"[pscustomobject]@{{InstanceId={device};Status='OK'}} }}\n"
        "function Get-NetAdapter { @() }\n"
        "function Get-ItemProperty { [pscustomobject]@{UEFISecureBootEnabled=1;SystemStartOptions='TESTSIGNING'} }\n"
        "function Import-Certificate { throw 'Unexpected certificate import' }\n"
        "function Start-Process { throw 'Unexpected process installation' }\n"
        f"& {quote(ROOT / 'installer/Check-Prerequisites.ps1')} -OutputPath {quote(report)}\n",
        encoding="utf-8")
    env = dict(os.environ)
    if wow64:
        env.update(PROCESSOR_ARCHITECTURE="x86", PROCESSOR_ARCHITEW6432="AMD64")
    result = subprocess.run([str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                            check=False, capture_output=True, text=True, timeout=25, env=env)
    assert result.returncode == 0, result.stderr
    content = report.read_text(encoding="utf-16")
    # Supported hardware still requires the minimum OS build on the executing host.
    expected = supported and sys.getwindowsversion().build >= 26100
    assert f"supported={int(expected)}" in content
    assert "secureboot=On" in content


@pytest.mark.skipif(sys.platform != "win32", reason="Windows uninstall detector")
@pytest.mark.parametrize("state", ["none", "completed", "unfinished", "missing", "corrupt"])
def test_uninstall_preserves_unfinished_or_unreadable_recovery_state(tmp_path, state):
    data = tmp_path / "OpenDrop"
    data.mkdir()
    session = tmp_path / "session"
    session.mkdir()
    if state != "none":
        (data / "session.json").write_text(json.dumps({"path": str(session)}), encoding="utf-8")
    if state in ("completed", "unfinished"):
        (session / "state.json").write_text(json.dumps({"Completed": state == "completed"}), encoding="utf-8")
    if state == "corrupt":
        (data / "session.json").write_text("not json", encoding="utf-8")
    report = tmp_path / "report.ini"
    env = dict(os.environ, LOCALAPPDATA=str(tmp_path))
    result = subprocess.run([str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                             str(ROOT / "installer/Check-Prerequisites.ps1"),
                             "-CheckRemoval", "-OutputPath", str(report)], env=env,
                            check=False, capture_output=True, text=True, timeout=25)
    assert result.returncode == 0, result.stderr
    assert f"removable={int(state in ('none', 'completed'))}" in report.read_text(encoding="utf-16")
    assert session.is_dir()


@pytest.mark.skipif(sys.platform != "win32", reason="Windows release verifier")
@pytest.mark.parametrize("mutation", ["version", "path", "hash"])
def test_release_verifier_rejects_invalid_manifest_before_trusting_certificate(tmp_path, mutation):
    names = ["opendrop_ax211_km.sys", "opendrop_ax211_km.inf", "opendrop_ax211_km.cat", "release.cer"]
    metadata = {"version": 1, "signing": "test-only", "thumbprint": "0" * 40,
                "files": {name: "0" * 64 for name in names}}
    for name in names:
        (tmp_path / name).write_bytes(b"damaged")
    if mutation == "version":
        metadata["version"] = 99
    if mutation == "path":
        metadata["files"]["../outside"] = "0" * 64
    (tmp_path / "release-driver.json").write_text(json.dumps(metadata))
    script = tmp_path / "verify.ps1"
    script.write_text("$ErrorActionPreference='Stop'\n"
                      "function Import-Certificate { throw 'Unexpected trust change' }\n"
                      f". {quote(ROOT / 'native/intel/Release-Driver.ps1')}\n"
                      f"Get-ReleaseDriverMetadata {quote(tmp_path)}\n", encoding="utf-8")
    result = subprocess.run([str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                            check=False, capture_output=True, text=True, timeout=25)
    assert result.returncode != 0
    assert "Unexpected trust change" not in result.stderr
    assert "release" in result.stderr.lower()
