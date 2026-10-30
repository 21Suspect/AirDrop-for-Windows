"""Real subprocess lifecycle checks; these do not start firmware or radio hardware."""
import json
import sys
from types import SimpleNamespace

import pytest

from opendrop.windows import cli, native, network
from opendrop.windows.native import NativeRadio

# Windows venv python.exe is a launcher with a different child PID. A native
# radio host is one process, so use the actual base interpreter for this fixture.
CHILD_PYTHON = sys._base_executable

READY = {
    "event": "ready", "version": 1, "interface_index": 24,
    "address": "fe80::ad:21ff:fe00:1", "ibss_configured": True,
}


def child_code(event=READY, *, after="", before=""):
    return (
        "import sys,json,os,time\n" + before + "\n"
        f"event=json.loads({json.dumps(json.dumps(event))})\n"
        "event.setdefault('pid',os.getpid())\n"
        "print(json.dumps(event),flush=True)\n"
        "sys.stdin.read()\n" + after + "\n"
        "print(json.dumps({'event':'stopped','buffers_released':True}),flush=True)\n"
    )


@pytest.fixture
def processes(monkeypatch):
    opened = []
    original = native.subprocess.Popen

    def spawn(*args, **kwargs):
        process = original(*args, **kwargs)
        opened.append(process)
        return process

    monkeypatch.setattr(native.subprocess, "Popen", spawn)
    yield opened
    for process in opened:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


def test_managed_host_readiness_and_eof_shutdown(processes):
    radio = NativeRadio([CHILD_PYTHON, "-u", "-c", child_code()], stop_timeout=3)
    assert radio.index == 24 and radio.address == READY["address"]
    radio.check()
    radio.close()
    radio.close()  # Repeated cleanup is harmless.
    assert radio.stopped_cleanly and radio.process.returncode == 0
    assert not radio.reader.is_alive() and not radio.error_reader.is_alive()


@pytest.mark.parametrize("changes", [
    {"interface_index": True}, {"interface_index": 0}, {"interface_index": 2**32},
    {"address": "::1"}, {"address": "2001:db8::1"},
    {"address": "fe80::1%24"}, {"pid": -1}, {"version": 99},
    {"ibss_configured": False},
])
def test_invalid_ready_message_stops_the_child_before_receiver_start(processes, changes):
    with pytest.raises(RuntimeError, match="Invalid native"):
        NativeRadio([CHILD_PYTHON, "-u", "-c", child_code(READY | changes)],
                    stop_timeout=3)
    assert len(processes) == 1 and processes[0].poll() is not None


def test_child_error_is_not_treated_as_readiness(processes):
    script = "import sys\nprint('{\"event\":\"error\"}',flush=True)\nsys.exit(1)"
    with pytest.raises(RuntimeError, match="reported a radio failure"):
        NativeRadio([CHILD_PYTHON, "-u", "-c", script], stop_timeout=2)
    assert processes[0].poll() is not None


def test_readiness_timeout_cannot_leave_a_process_running(processes):
    with pytest.raises(RuntimeError, match="did not become ready"):
        NativeRadio([CHILD_PYTHON, "-u", "-c", "import time; time.sleep(30)"],
                    ready_timeout=0.2, stop_timeout=0.2)
    assert processes[0].poll() is not None


def test_oversized_status_message_is_bounded_and_child_is_closed(processes):
    script = child_code(before="print('x'*9000,flush=True)")
    with pytest.raises(RuntimeError, match="Oversized"):
        NativeRadio([CHILD_PYTHON, "-u", "-c", script], stop_timeout=2)
    assert processes[0].poll() is not None


def test_runtime_exit_reaches_receiver_and_unconfirmed_stop_fails(processes):
    script = "import sys,json,os\n" + (
        f"event=json.loads({json.dumps(json.dumps(READY))})\n"
        "event['pid']=os.getpid()\nprint(json.dumps(event),flush=True)\n"
        "sys.stdin.readline()\nsys.exit(1)\n"
    )
    radio = NativeRadio([CHILD_PYTHON, "-u", "-c", script], stop_timeout=2)
    radio.process.stdin.write("fail\n")
    radio.process.stdin.flush()
    assert radio.ended.wait(3)
    with pytest.raises(RuntimeError, match="AWDL radio stopped"):
        radio.check()
    with pytest.raises(RuntimeError, match="did not confirm"):
        radio.close()


def test_interface_selection_uses_native_hosts_exact_address(monkeypatch):
    addresses = [
        SimpleNamespace(is_IPv6=True, ip=(ip, 0, 24), nice_name="OpenDrop AWDL test")
        for ip in ("fe80::dead:beef", READY["address"])
    ]
    adapter = SimpleNamespace(name="adapter", nice_name="OpenDrop AWDL test",
                              index=24, ips=addresses)
    monkeypatch.setattr(network.ifaddr, "get_adapters", lambda: [adapter])
    assert network.find_interface("24", address=READY["address"]).address == READY["address"]
    with pytest.raises(RuntimeError, match="No IPv6 interface"):
        network.find_interface("24", address="fe80::bad")


def test_discovery_close_failure_still_stops_receiver_and_radio(monkeypatch, tmp_path):
    closed = []

    def interrupt(_):
        raise KeyboardInterrupt

    radio = SimpleNamespace(index=24, address=READY["address"],
                            ended=SimpleNamespace(wait=interrupt),
                            close=lambda: closed.append("radio"))
    monkeypatch.setattr(cli.NativeRadio, "start", lambda *a: radio)
    monkeypatch.setattr(cli, "find_interface",
                        lambda *a, **k: network.Interface("test", 24, READY["address"], 24))
    receiver = SimpleNamespace(server_port=8771, start=lambda: None,
                               close=lambda: closed.append("receiver"))
    monkeypatch.setattr(cli, "Receiver", lambda *a: receiver)

    def broken_discovery_close():
        closed.append("discovery")
        raise RuntimeError("interface already gone")

    monkeypatch.setattr(cli, "Announcement",
                        lambda *a: SimpleNamespace(close=broken_discovery_close))
    result = cli.main(["receive", "--radio-host", "host.exe", "--firmware", "fw",
                       "--pnvm", "nv", "--wintun", "wintun.dll", "--state-dir", str(tmp_path)])
    assert result == 2
    assert closed == ["discovery", "receiver", "radio"]
