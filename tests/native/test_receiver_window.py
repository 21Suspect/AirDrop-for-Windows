# SPDX-License-Identifier: GPL-3.0-or-later
"""Real helper process lifetime with mocked Windows adapter/firewall cmdlets."""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Windows helper")
ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "native/intel/Invoke-ReceiverWindow.ps1"


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


@pytest.mark.parametrize("mode", ["ok", "scoped", "wrong_pid", "wrong_interface", "wrong_address", "rule_failure", "radio_failure", "app", "stale_app"])
def test_receiver_window_scopes_rules_and_cleans_own_process(tmp_path, mode):
    if mode in ("app", "stale_app"):
        heartbeat = tmp_path / "heartbeat"
        heartbeat.touch()
        if mode == "stale_app":
            os.utime(heartbeat, (1, 1))
    child = tmp_path / "fake radio.py"
    child.write_text(
        "import os,sys,json,time\nfrom pathlib import Path\n"
        f"mode={mode!r}\n"
        "ready={'version':1,'pid':os.getpid()+(mode=='wrong_pid'),"
        "'interface_index':123,'address':'fe80::ad:21ff:fe00:1','ibss_configured':True}\n"
        "Path(sys.argv[-1]).write_text(json.dumps(ready))\n"
        "print(json.dumps(dict(ready,event='ready')),flush=True)\n"
        "time.sleep(2)\n"
        "print(json.dumps({'event':'stopped','buffers_released':True}),flush=True)\n"
        "sys.exit(1 if mode=='radio_failure' else 0)\n", encoding="utf-8"
    )
    script = r"""
$ErrorActionPreference='Stop'
$global:taskAdded=New-Object Collections.ArrayList
$global:taskRemoved=New-Object Collections.ArrayList
$global:taskRemaining=New-Object Collections.ArrayList
$global:taskDirectory=TEST_DIRECTORY
$global:taskMode=TEST_MODE
function Get-NetIPInterface {
 param($InterfaceIndex,$AddressFamily)
 $r=Get-Content -LiteralPath (Join-Path $global:taskDirectory 'radio-ready.json') -Raw | ConvertFrom-Json
 $a=if($global:taskMode -eq 'wrong_interface') {'Ethernet'} else {"OpenDrop AWDL $($r.pid)"}
 [pscustomobject]@{InterfaceAlias=$a}
}
function Get-NetIPAddress {
 param($InterfaceIndex,$AddressFamily)
 $ip=if($global:taskMode -eq 'scoped') {'fe80::ad:21ff:fe00:1%123'} elseif($global:taskMode -eq 'wrong_address') {'fe80::999'} else {'fe80::ad:21ff:fe00:1'}
 [pscustomobject]@{IPAddress=$ip}
}
function New-NetFirewallRule {
 [CmdletBinding()]
 param($Action,$Enabled,$Profile,$InterfaceAlias,$Name,$DisplayName,$Direction,
       $Protocol,$LocalPort,$RemotePort,$Program,$RemoteAddress,$IcmpType)
 $copy=@{}
 foreach($key in $PSBoundParameters.Keys) {$copy[$key]=$PSBoundParameters[$key]}
 if($global:taskMode -eq 'rule_failure' -and $global:taskAdded.Count -eq 2) {throw 'mock rule failure'}
 $null=$global:taskAdded.Add($copy)
 $null=$global:taskRemaining.Add($Name)
}
function Get-NetFirewallRule {
 [CmdletBinding()]param($Name)
 if($global:taskRemaining.Contains($Name)) { [pscustomobject]@{Name=$Name} }
}
function Remove-NetFirewallRule {
 [CmdletBinding()]param([Parameter(ValueFromPipeline)]$InputObject)
 process {
  $null=$global:taskRemoved.Add($InputObject.Name)
  $global:taskRemaining.Remove($InputObject.Name)
 }
}
$taskFailure=$null
try {
 $callArgs=@{RadioHost=PYTHON_EXE;FirmwarePath=FAKE_SCRIPT;PnvmPath='unused';WintunPath='unused';
 SessionDirectory=TEST_DIRECTORY;PythonProgram=PYTHON_EXE;
 RulePrefix='OpenDrop-Test-0123456789abcdef0123456789abcdef';Seconds=10;
 AppSession=($global:taskMode -in @('app','stale_app'));DeviceName='App test PC'}
 $taskCode=& RUNNER_SCRIPT @callArgs
} catch { $taskFailure=$_.Exception.Message }
[pscustomobject]@{error=$taskFailure;added=@($global:taskAdded);removed=@($global:taskRemoved);
 remaining=@($global:taskRemaining);code=$taskCode} |
 ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $global:taskDirectory 'calls.json') -Encoding UTF8
"""
    for key, value in {
        "TEST_DIRECTORY": tmp_path, "TEST_MODE": mode,
        "PYTHON_EXE": sys._base_executable, "FAKE_SCRIPT": child, "RUNNER_SCRIPT": RUNNER,
    }.items():
        script = script.replace(key, quote(value))
    wrapper = tmp_path / "run.ps1"
    wrapper.write_text(script, encoding="utf-8")
    shell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run(
        [str(shell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
         "-File", str(wrapper)], capture_output=True, text=True, timeout=20, check=False,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    assert result.returncode == 0, result.stderr
    calls = json.loads((tmp_path / "calls.json").read_text(encoding="utf-8-sig"))
    report = json.loads((tmp_path / "firmware-init.json").read_text(encoding="utf-8-sig"))
    assert calls["remaining"] == []
    assert sorted(calls["removed"]) == sorted(rule["Name"] for rule in calls["added"])
    assert (calls["error"] is None) == (mode in ("ok", "scoped", "app"))
    assert report["ok"] == (mode in ("ok", "scoped", "app"))
    if mode in ("wrong_pid", "wrong_interface", "wrong_address", "stale_app"):
        assert calls["added"] == []
        assert not (tmp_path / "receiver-ready.json").exists()
    else:
        assert len(calls["added"]) == (2 if mode == "rule_failure" else 5)
    for rule in calls["added"]:
        assert rule["InterfaceAlias"].startswith("OpenDrop AWDL ")
        assert rule["RemoteAddress"] in ("fe80::/10", ["fe80::/10", "ff02::fb"],
                                         ["fe80::/10", "ff02::/16"])
        if rule["Protocol"] in ("TCP", "UDP"):
            assert rule["Program"] == sys._base_executable
        if rule["Protocol"] == "TCP":
            assert rule["LocalPort"] == 8771 and rule["Direction"] == "Inbound"
    assert (tmp_path / "receiver-ended.json").exists()

    if mode == "app":
        ready = json.loads((tmp_path / "receiver-ready.json").read_text(encoding="utf-8-sig"))
        assert "receiver_stop_unix_ms" not in ready
