# SPDX-License-Identifier: GPL-3.0-or-later
"""Unelevated desktop controller. The UI communicates over private stdio pipes."""
import argparse
import ctypes
import json
import logging
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import uuid
from logging.handlers import RotatingFileHandler
from pathlib import Path

from .network import Announcement, Discovery, Interface
from .receiver import Receiver, Settings
from .sender import Cancellation, Cancelled, send

LOG = logging.getLogger(__name__)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def validate_settings(value):
    name = value.get("name", "Windows PC").strip()
    if not name or len(name.encode("utf-8")) > 63 or any(ord(c) < 32 or c == '"' for c in name):
        raise ValueError("Choose a device name of 1–63 UTF-8 bytes, without quotes or control characters")
    output = Path(value.get("output", str(Path.home() / "Pictures" / "AirDrop"))).expanduser()
    if not output.is_absolute():
        raise ValueError("The receive folder must be an absolute path")
    limit = int(value.get("limit_gib", 4))
    if limit not in (1, 4, 8):
        raise ValueError("Choose a 1, 4 or 8 GB transfer limit")
    ask = value.get("ask_before_receiving", False)
    if type(ask) is not bool:
        raise ValueError("Ask before receiving must be on or off")
    return {"name": name, "output": str(output), "limit_gib": limit, "ask_before_receiving": ask}


ELEVATED_SCRIPT = r"""
$ErrorActionPreference='Stop'
Start-Transcript -Path (Join-Path $PSScriptRoot 'helper.log') -Force | Out-Null
try {
    $config=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'config.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $parameters=@{StatePath=(Join-Path $PSScriptRoot 'state.json');BuildDirectory=(Join-Path $PSScriptRoot 'kernel');
        DriverKind='KMDF';AppSession=$true;FirmwarePath=$config.firmware;PnvmPath=$config.pnvm;
        WintunPath=$config.wintun;ReceiverPython=$config.program;DeviceName=$config.name}
    & (Join-Path $PSScriptRoot 'src\Test-IntelDriver.ps1') @parameters
} catch {
    @{error=$_.Exception.Message} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'helper-error.json') -Encoding UTF8
} finally { Stop-Transcript | Out-Null }
"""


class Controller:
    def __init__(self, config, data, emit):
        self.config, self.data, self.emit = config, Path(data), emit
        self.data.mkdir(parents=True, exist_ok=True)
        self.settings_path = self.data / "settings.json"
        self.settings = validate_settings(read_json(self.settings_path) if self.settings_path.exists() else {})
        self.history_path = self.data / "history.json"
        try:
            self.history = read_json(self.history_path)[-100:]
        except (OSError, ValueError):
            self.history = []
        self.lock = threading.RLock()
        self.status = "offline"
        self.session = None
        self.worker = None
        self.helper = None
        self.stop = threading.Event()
        self.closed = threading.Event()
        self.last_ping = time.monotonic()
        self.receiver = self.announcement = self.discovery = None
        self.devices = {}
        self.approvals = {}
        self.send_cancel = None
        self.send_thread = None
        self.transfer_files = {}
        self.last_progress = {}
        self.journal = self.data / "session.json"
        self.previous = None
        if self.journal.exists():
            try:
                previous = Path(read_json(self.journal)["path"])
                state = read_json(previous / "state.json")
                if not state.get("Completed"):
                    self.previous = previous
                    self.status = "recovery"
            except (OSError, ValueError, KeyError):
                pass
        self.emit("hello", settings=self.settings, history=self.history, status=self.status,
                  recovery=str(self.previous or ""), version="0.2.0-preview.1")
        self.guardian = threading.Thread(target=self._guard, daemon=True)
        self.guardian.start()

    def _guard(self):
        while not self.closed.wait(1):
            if time.monotonic() - self.last_ping > 15:
                LOG.warning("Desktop controller heartbeat expired")
                self.stop.set()
                self.cancel_all()
                self.closed.set()
                break
            if self.session and not self.stop.is_set():
                try:
                    (self.session / "heartbeat").touch()
                except OSError:
                    self.stop.set()

    def state(self, status, detail=""):
        self.status = status
        self.emit("state", status=status, detail=detail)

    def command(self, message):
        kind = message.get("command")
        if kind == "ping":
            self.last_ping = time.monotonic()
        elif kind == "enable":
            if self.status not in ("offline", "error") or (self.worker and self.worker.is_alive()):
                raise RuntimeError("Wait for the current session to finish")
            if self.previous:
                raise RuntimeError("Restore the previous driver session first")
            self.stop.clear()
            self.state("starting", "Preparing the radio. Windows may ask for administrator permission.")
            self.worker = threading.Thread(target=self._run, daemon=False)
            self.worker.start()
        elif kind == "disable":
            self.stop.set()
            self.cancel_all()
        elif kind == "settings":
            if self.status not in ("offline", "error", "online"):
                raise RuntimeError("Wait for the radio to finish starting or stopping")
            settings = validate_settings(message["settings"])
            if self.status == "online" and any(settings[key] != self.settings[key]
                                               for key in ("name", "output", "limit_gib")):
                raise RuntimeError("Turn off AirDrop before changing its name, folder or size limit")
            Path(settings["output"]).mkdir(parents=True, exist_ok=True)
            with self.lock:
                atomic_json(self.settings_path, settings)
                self.settings = settings
            self.emit("settings", settings=settings)
        elif kind in ("accept", "decline"):
            with self.lock:
                pending = self.approvals.get(message.get("id"))
                if pending:
                    pending["allow"] = kind == "accept"
                    pending["ready"].set()
        elif kind == "send":
            self._send(message)
        elif kind == "cancel":
            identifier = message.get("id", "")
            if identifier == "outgoing" and self.send_cancel:
                self.send_cancel.cancel()
            elif self.receiver:
                self.receiver.cancel_upload(identifier)
        elif kind == "recover":
            if self.status != "recovery" or not self.previous:
                raise RuntimeError("No recovery is pending")
            self.state("restoring", "Restoring the Intel Wi-Fi driver")
            self.worker = threading.Thread(target=self._recover, daemon=False)
            self.worker.start()
        elif kind == "shutdown":
            self.stop.set()
            self.cancel_all()
            self.closed.set()
        else:
            raise ValueError("Unknown app command")

    def cancel_all(self):
        with self.lock:
            for pending in self.approvals.values():
                pending["allow"] = False
                pending["ready"].set()
        if self.send_cancel:
            self.send_cancel.cancel()

    def approve(self, sender, files):
        if self.stop.is_set():
            return False
        if not self.settings["ask_before_receiving"]:
            self.emit("auto_accepted", sender="".join(c for c in sender if c.isprintable()), files=files)
            return True
        identifier = uuid.uuid4().hex
        pending = {"ready": threading.Event(), "allow": False}
        with self.lock:
            if self.stop.is_set():
                return False
            self.approvals[identifier] = pending
        self.emit("offer", id=identifier, sender="".join(c for c in sender if c.isprintable()),
                  files=files, seconds=30)
        try:
            pending["ready"].wait(30)
            return pending["allow"] and not self.stop.is_set()
        finally:
            with self.lock:
                self.approvals.pop(identifier, None)
            self.emit("offer_closed", id=identifier)

    def record(self, direction, files, peer="", path="", error=""):
        item = {"id": uuid.uuid4().hex, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "direction": direction, "files": files, "peer": peer, "path": path,
                "status": "failed" if error else "complete", "error": error}
        with self.lock:
            self.history.append(item)
            self.history = self.history[-100:]
            atomic_json(self.history_path, self.history)
        self.emit("history", item=item)

    def receive_event(self, kind, **value):
        if kind == "receiving":
            self.transfer_files[value["id"]] = value["files"]
        elif kind == "received":
            self.transfer_files.pop(value["id"], None)
            self.record("received", value["files"], path=value["path"])
        elif kind == "receive_failed":
            files = self.transfer_files.pop(value["id"], [])
            self.record("received", files, error=value["error"])
        self.emit(kind, **value)

    def found(self, devices):
        with self.lock:
            self.devices = {d.id: d for d in devices}
        self.emit("devices", devices=[{"id": d.id, "name": d.name, "model": d.model} for d in devices])

    def _send(self, message):
        if self.status != "online":
            raise RuntimeError("Turn on AirDrop before sending")
        if self.send_thread and self.send_thread.is_alive():
            raise RuntimeError("Finish or cancel the current outgoing transfer first")
        with self.lock:
            device = self.devices.get(message.get("device"))
        if not device:
            raise ValueError("The selected device is no longer available")
        paths = message.get("files")
        if not isinstance(paths, list) or not all(isinstance(p, str) for p in paths):
            raise ValueError("Choose files to send")
        self.send_cancel = Cancellation()
        token = self.send_cancel
        files = [Path(p).name for p in paths]
        self.emit("sending", id="outgoing", files=files, peer=device.name)
        def progress(phase, done, total):
            now = time.monotonic()
            key = (phase, total)
            if done == total or key != self.last_progress.get("key") or now - self.last_progress.get("at", 0) > 0.15:
                self.last_progress.update(key=key, at=now)
                self.emit("send_progress", id="outgoing", phase=phase, done=done, total=total)
        def worker():
            try:
                completed = send(device, paths, self.settings["name"], self.announcement.identifier,
                                 token, progress, self.settings["limit_gib"] * 1024**3)
                self.record("sent", completed, peer=device.name)
                self.emit("sent", id="outgoing", files=completed)
            except Exception as exc:
                LOG.exception("Outgoing transfer")
                error = "Transfer canceled" if token.event.is_set() else str(exc)
                self.record("sent", files, peer=device.name, error=error)
                self.emit("send_failed", id="outgoing", error=error)
        self.send_thread = threading.Thread(target=worker, daemon=True)
        self.send_thread.start()

    def elevate(self, script, args=()):
        from ctypes import wintypes
        class ExecuteInfo(ctypes.Structure):
            _fields_ = [
                ("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG),
                ("hwnd", wintypes.HWND), ("lpVerb", wintypes.LPCWSTR),
                ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int),
                ("hInstApp", wintypes.HINSTANCE), ("lpIDList", ctypes.c_void_p),
                ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                ("dwHotKey", wintypes.DWORD), ("hIcon", wintypes.HANDLE),
                ("hProcess", wintypes.HANDLE),
            ]
        ps = str(Path(os.environ["WINDIR"]) / "System32/WindowsPowerShell/v1.0/powershell.exe")
        params = subprocess.list2cmdline(["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), *args])
        info = ExecuteInfo()
        info.cbSize, info.fMask, info.nShow = ctypes.sizeof(info), 0x40, 0
        info.lpVerb, info.lpFile, info.lpParameters = "runas", ps, params
        info.lpDirectory = str(script.parent)
        shell = ctypes.WinDLL("shell32", use_last_error=True)
        shell.ShellExecuteExW.argtypes = [ctypes.POINTER(ExecuteInfo)]
        shell.ShellExecuteExW.restype = wintypes.BOOL
        if not shell.ShellExecuteExW(ctypes.byref(info)):
            error = ctypes.get_last_error()
            raise RuntimeError(f"Administrator permission was canceled or unavailable (Windows {error}). Click Turn on to retry.")
        self.helper = info.hProcess

    def helper_ended(self):
        if not self.helper:
            return True
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.WaitForSingleObject.restype = ctypes.c_uint32
        return kernel.WaitForSingleObject(self.helper, 0) == 0

    def close_helper(self):
        if self.helper:
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            kernel.CloseHandle(self.helper)
            self.helper = None

    def _stage(self):
        session = self.data / "sessions" / uuid.uuid4().hex
        session.mkdir(parents=True)
        shutil.copytree(self.config["intel_source"], session / "src")
        (session / "kernel").mkdir()
        driver = Path(self.config["driver"])
        files = ["opendrop_ax211_km.sys", "opendrop_ax211_km.inf",
                 "ax211-probe.exe", "ax211-stage.exe", "awdl-host.exe", "radio-check.exe"]
        if (driver / "release-driver.json").is_file():
            files += ["release-driver.json", "release.cer", "opendrop_ax211_km.cat"]
        else:
            files += ["build.json"]
        for file in files:
            shutil.copy2(driver / file, session / "kernel" / file)
        config = {key: self.config[key] for key in ("firmware", "pnvm", "wintun")}
        config.update(program=sys.executable if getattr(sys, "frozen", False) else sys._base_executable,
                      name=self.settings["name"])
        atomic_json(session / "config.json", config)
        (session / "run.ps1").write_text(ELEVATED_SCRIPT, encoding="utf-8")
        (session / "heartbeat").touch()
        atomic_json(self.journal, {"path": str(session)})
        self.session = session
        self.emit("session", path=str(session))
        return session

    def _wait_ready(self, session):
        deadline = time.monotonic() + 180
        while not self.stop.wait(0.2):
            ready = session / "receiver-ready.json"
            if ready.exists():
                return read_json(ready)
            error = session / "helper-error.json"
            if error.exists():
                raise RuntimeError(read_json(error).get("error", "Radio setup failed"))
            state = session / "state.json"
            if state.exists():
                value = read_json(state)
                if value.get("Completed"):
                    raise RuntimeError(value.get("InstallError") or "Radio setup ended before readiness")
            if self.helper_ended():
                raise RuntimeError("The administrator helper ended before the radio was ready. Open diagnostics for details.")
            if time.monotonic() >= deadline:
                raise TimeoutError("Radio startup timed out. Inspect diagnostics before retrying.")
        raise Cancelled("Startup canceled")

    def _run(self):
        session, failure = None, ""
        try:
            session = self._stage()
            self.elevate(session / "run.ps1")
            ready = self._wait_ready(session)
            if self.stop.is_set():
                raise Cancelled("Startup canceled")
            interface = Interface("OpenDrop AWDL", ready["interface_index"], ready["address"], ready["interface_index"])
            settings = Settings(Path(self.settings["output"]), self.data / "identity",
                                self.settings["name"], self.settings["limit_gib"] * 1024**3, timeout=60, organized=True)
            self.receiver = Receiver((interface.address, 8771, 0, interface.scope), settings,
                                     self.approve, self.receive_event)
            self.receiver.start()
            self.announcement = Announcement(interface, 8771)
            self.discovery = Discovery(self.announcement, interface, self.found)
            self.state("online", "Visible as " + self.settings["name"])
            last_stats = 0
            while not self.stop.wait(0.25):
                if (session / "receiver-ended.json").exists():
                    value = read_json(session / "receiver-ended.json")
                    raise RuntimeError("The radio session ended. Turn on again to reconnect." if value.get("ok")
                                       else "The radio stopped unexpectedly; restoring Wi-Fi.")
                if time.monotonic() - last_stats > 2:
                    try:
                        self.emit("radio", **read_json(session / "radio-ready.stats.json"))
                    except (OSError, ValueError):
                        pass
                    last_stats = time.monotonic()
        except Cancelled:
            pass
        except Exception as exc:
            LOG.exception("Radio session")
            failure = str(exc)
            self.emit("error", message=failure)
        finally:
            self.state("stopping", "Closing transfers and restoring Wi-Fi")
            self.cancel_all()
            if self.send_thread:
                self.send_thread.join(timeout=5)
            for resource in (self.discovery, self.announcement, self.receiver):
                if resource:
                    try:
                        resource.close()
                    except Exception:
                        LOG.exception("Closing app resource")
            self.discovery = self.announcement = self.receiver = None
            self.found([])
            if session:
                (session / "heartbeat.stop").touch()
                self.stop.set()
                deadline = time.monotonic() + 90
                while not self.helper_ended() and time.monotonic() < deadline:
                    time.sleep(0.25)
                statefile = session / "state.json"
                restored = self.helper_ended()
                if statefile.exists():
                    restored = restored and self.wait_restored(session)
                if not restored:
                    self.previous = session
            self.close_helper()
            self.session = None
            if self.previous:
                self.state("recovery", "Driver restoration needs attention. Open diagnostics or try Restore Wi-Fi.")
            else:
                self.state("error" if failure else "offline", failure or "Ready when you are")

    def wait_restored(self, session):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                state = read_json(session / "state.json")
                if state.get("Completed") and not state.get("UnsafeFirmwareState") and state.get("RestoredProblemCode") == 0:
                    return True
                if state.get("RequiresRestart"):
                    return False
            except (OSError, ValueError):
                pass
            time.sleep(0.5)
        return False

    def _recover(self):
        try:
            self.elevate(self.previous / "src/Test-IntelDriver.ps1",
                         ["-Mode", "Restore", "-StatePath", str(self.previous / "state.json")])
            if not self.wait_restored(self.previous):
                raise RuntimeError("Recovery was not verified. Read diagnostics; a restart may be required.")
            self.previous = None
            self.state("offline", "Intel Wi-Fi driver restored")
        except Exception as exc:
            LOG.exception("Recovery failed")
            self.emit("error", message=str(exc))
            self.state("recovery", str(exc))
        finally:
            self.close_helper()

    def close(self):
        self.closed.set()
        self.stop.set()
        self.cancel_all()
        if self.worker:
            self.worker.join(timeout=110)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--data", default=str(Path(os.environ.get("LOCALAPPDATA", Path.home())) / "OpenDrop"))
    args = parser.parse_args()
    if os.name == "nt" and ctypes.windll.shell32.IsUserAnAdmin():
        raise SystemExit("Start OpenDrop normally. Only its radio helper should run as administrator.")
    data = Path(args.data)
    data.mkdir(parents=True, exist_ok=True)
    log = RotatingFileHandler(data / "app.log", maxBytes=2 * 1024**2, backupCount=3, encoding="utf-8")
    logging.basicConfig(level=logging.INFO, handlers=[log])
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    output_lock = threading.Lock()
    def emit(event, **value):
        with output_lock:
            print(json.dumps(dict(event=event, **value), ensure_ascii=False), flush=True)
    config_path = Path(args.config).resolve()
    config = read_json(config_path)
    for key in ("driver", "intel_source", "firmware", "pnvm", "wintun"):
        if key in config:
            config[key] = str((config_path.parent / config[key]).resolve())
    controller = Controller(config, data, emit)
    commands = queue.Queue(maxsize=100)
    def reader():
        try:
            while True:
                line = sys.stdin.readline(1024 * 1024 + 1)
                if not line or len(line) > 1024 * 1024:
                    break
                try:
                    message = json.loads(line)
                    if not isinstance(message, dict):
                        raise TypeError("Invalid app command")
                    commands.put(message, timeout=1)
                except (ValueError, TypeError, queue.Full):
                    break
        finally:
            controller.closed.set()
            controller.stop.set()
            controller.cancel_all()
    threading.Thread(target=reader, daemon=True).start()
    try:
        while not controller.closed.is_set():
            try:
                message = commands.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                controller.command(message)
            except Exception as exc:
                LOG.exception("Desktop command")
                emit("error", message=str(exc))
    finally:
        controller.close()
        emit("closed")


if __name__ == "__main__":
    main()
