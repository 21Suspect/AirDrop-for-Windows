# SPDX-License-Identifier: GPL-3.0-or-later
"""Manage a preinstalled AX211 radio host; this never installs a driver or elevates."""
import ipaddress
import json
import logging
import queue
import subprocess
import threading
from collections import deque
from pathlib import Path

LOG = logging.getLogger(__name__)


class NativeRadio:
    def __init__(self, command, *, ready_timeout=45, stop_timeout=15):
        self.stop_timeout = stop_timeout
        self.ended = threading.Event()
        self.errors = deque(maxlen=12)
        self.stopped_cleanly = False
        self.closed = False
        self.ready_messages = queue.Queue(maxsize=1)
        self.process = subprocess.Popen(
            [str(arg) for arg in command],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.error_reader = threading.Thread(target=self._read_errors, daemon=True)
        self.reader.start()
        self.error_reader.start()
        try:
            try:
                ready = self.ready_messages.get(timeout=ready_timeout)
            except queue.Empty as exc:
                raise RuntimeError("The native AWDL host did not become ready") from exc
            if isinstance(ready, Exception):
                raise ready
            self.index = ready["interface_index"]
            self.address = ready["address"]
        except BaseException:
            try:
                self.close()
            except RuntimeError:
                LOG.debug("Native startup cleanup: %s", " | ".join(self.errors))
            raise

    @classmethod
    def start(cls, executable, firmware, pnvm, wintun, name):
        paths = [Path(value).resolve(strict=True) for value in
                 (executable, firmware, pnvm, wintun)]
        if not all(path.is_file() for path in paths):
            raise ValueError("Native host, firmware, PNVM and Wintun must be files")
        return cls([*paths, name])

    def _publish(self, value):
        try:
            self.ready_messages.put_nowait(value)
        except queue.Full:
            pass

    def _read_errors(self):
        for line in iter(lambda: self.process.stderr.readline(4096), ""):
            self.errors.append(line.rstrip()[:4096])

    def _read(self):
        ready_seen = False
        try:
            while True:
                line = self.process.stdout.readline(8193)
                if not line:
                    break
                if len(line) > 8192:
                    raise RuntimeError("Oversized native host status message")
                event = json.loads(line)
                if not isinstance(event, dict):
                    raise TypeError("Invalid native host status")
                if event.get("event") == "ready":
                    if ready_seen:
                        raise RuntimeError("The native host changed its interface unexpectedly")
                    index = event.get("interface_index")
                    address = ipaddress.IPv6Address(event.get("address", ""))
                    if (event.get("version") != 1 or event.get("pid") != self.process.pid
                            or type(index) is not int or not 0 < index < 2**32
                            or not address.is_link_local or address.scope_id is not None
                            or event.get("ibss_configured") is not True):
                        raise RuntimeError("Invalid native AWDL interface announcement")
                    event["address"] = str(address)
                    ready_seen = True
                    self._publish(event)
                elif event.get("event") == "stopped":
                    self.stopped_cleanly = event.get("buffers_released") is True
                elif event.get("event") == "error":
                    raise RuntimeError("The native AWDL host reported a radio failure")
                else:
                    raise RuntimeError("Unknown native host status message")
        except (TypeError, ValueError, RuntimeError) as exc:
            self.errors.append(str(exc))
            self._publish(RuntimeError(str(exc)))
        finally:
            if not ready_seen:
                self._publish(RuntimeError("The native AWDL host exited before readiness"))
            self.ended.set()

    def check(self):
        if self.ended.is_set() or self.process.poll() is not None:
            detail = " | ".join(self.errors) or "the radio process exited"
            raise RuntimeError(f"AWDL radio stopped: {detail}")

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            # EOF also stops the native process if this Python process crashes.
            try:
                self.process.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            try:
                result = self.process.wait(timeout=self.stop_timeout)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=5)
                raise RuntimeError(
                    "Native host shutdown timed out; Windows closed its device handle"
                ) from None
            self.reader.join(timeout=2)
            self.error_reader.join(timeout=2)
            if result != 0 or not self.stopped_cleanly:
                detail = " | ".join(self.errors)
                raise RuntimeError(
                    f"Native host did not confirm firmware shutdown (exit {result}): {detail}"
                )
        finally:
            self.process.stdout.close()
            self.process.stderr.close()
