# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real Windows child lifetime and separate stdout/stderr logging."""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Windows process runner")
ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "native/intel/Invoke-BoundedTool.ps1"


def ps_quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def run_helper(tmp_path, child, seconds=10):
    shell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    fixture = tmp_path / "child script.ps1"
    fixture.write_text(child, encoding="utf-8")
    output, error = tmp_path / "stdout.txt", tmp_path / "stderr.txt"
    script = (
        "$ErrorActionPreference='Stop'\n"
        f"$argsForChild=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',"
        f"'-File',('\\\"' + {ps_quote(fixture)} + '\\\"'))\n"
        f"$code=& {ps_quote(RUNNER)} -FilePath {ps_quote(shell)} "
        f"-ArgumentList $argsForChild -OutputPath {ps_quote(output)} "
        f"-ErrorPath {ps_quote(error)} -TimeoutSeconds {seconds}\n"
        "Write-Output $code\n"
    ).replace("\\\"", '"')
    command = tmp_path / "runner.ps1"
    command.write_text(script, encoding="utf-8")
    result = subprocess.run(
        [str(shell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
         "-File", str(command)], check=False, capture_output=True, text=True, timeout=25,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    return result, output, error, shell


def test_stderr_diagnostic_does_not_lose_stdout_or_exit_code(tmp_path):
    result, output, error, _ = run_helper(
        tmp_path,
        "[Console]::Out.WriteLine('{\\\"ok\\\":false}'); "
        "[Console]::Error.WriteLine('firmware diagnostic'); exit 7".replace("\\\"", '"'),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "7"
    assert json.loads(output.read_text(encoding="utf-8-sig")) == {"ok": False}
    assert error.read_text(encoding="utf-8-sig").strip() == "firmware diagnostic"


def test_timeout_terminates_only_its_owned_child(tmp_path):
    result, output, _, shell = run_helper(
        tmp_path, "[Console]::Out.WriteLine($PID); Start-Sleep -Seconds 30", seconds=2
    )
    assert result.returncode != 0
    assert "exceeded its 2 second bound" in result.stderr
    child_pid = int(output.read_text(encoding="utf-8-sig").strip())
    alive = subprocess.run(
        [str(shell), "-NoProfile", "-NonInteractive", "-Command",
         f"if(Get-Process -Id {child_pid} -ErrorAction SilentlyContinue) {{ exit 1 }}"],
        check=False, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW,
    )
    assert alive.returncode == 0


def test_successful_helper_returns_zero_and_preserves_output(tmp_path):
    result, output, error, _ = run_helper(
        tmp_path, "[Console]::Out.WriteLine('completed'); exit 0"
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "0"
    assert output.read_text(encoding="utf-8-sig").strip() == "completed"
    assert error.read_text(encoding="utf-8-sig") == ""
