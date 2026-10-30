# SPDX-License-Identifier: GPL-3.0-or-later
"""Unelevated receiver for a bounded, separately elevated development radio."""
import argparse
import ctypes
import ipaddress
import json
import logging
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from opendrop.windows.network import Announcement, find_interface
from opendrop.windows.receiver import Receiver, Settings

LOG = logging.getLogger(__name__)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "win32" or ctypes.windll.shell32.IsUserAnAdmin():
        raise RuntimeError("Run the receiver without administrator privileges on Windows")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    session = args.session.resolve(strict=True)
    ready_file = session / "receiver-ready.json"
    end_file = session / "receiver-ended.json"
    receiver = announcement = None
    report = {"started": False, "completed_transfers": [], "error": None}
    try:
        timeout = time.monotonic() + 180
        while not ready_file.exists():
            if end_file.exists():
                raise RuntimeError("Radio setup ended before receiver readiness")
            if time.monotonic() >= timeout:
                raise TimeoutError("Radio setup did not become ready within 180 seconds")
            time.sleep(0.2)
        ready = json.loads(ready_file.read_text(encoding="utf-8-sig"))
        address = ipaddress.IPv6Address(ready["address"])
        index = ready["interface_index"]
        if (ready.get("version") != 1 or type(index) is not int or index <= 0
                or not address.is_link_local or address.scope_id is not None):
            raise RuntimeError("Invalid radio readiness record")
        interface = find_interface(str(index), address=str(address))
        receiver = Receiver(
            (interface.address, 8771, 0, interface.scope),
            Settings(args.output, session / "receiver-state", "Windows PC"),
            lambda sender, files: True,
        )
        receiver.start()
        announcement = Announcement(interface, receiver.server_port)
        report["started"] = True
        report["interface_index"] = index
        report["destination"] = str(args.output.resolve())
        report["receiver_elevated"] = False
        (session / "receiver-listening.json").write_text(json.dumps(report), encoding="utf-8")
        print("READY FOR QUICK AIRDROP TEST: Windows PC", flush=True)
        print(f"Incoming files will be saved automatically in {args.output.resolve()}", flush=True)
        stop_ms = ready.get("receiver_stop_unix_ms")
        seconds = min(150, max(0, stop_ms / 1000 - time.time())) if type(stop_ms) is int else 150
        deadline = time.monotonic() + seconds
        while not end_file.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    except Exception as error:
        report["error"] = str(error)
        LOG.exception("Receiver test failed")
    finally:
        for resource in (announcement, receiver):
            if resource:
                try:
                    resource.close()
                except Exception as error:
                    report["error"] = str(error)
                    LOG.exception("Receiver shutdown failed")
        if receiver:
            report["completed_transfers"] = [
                {"directory": str(directory), "files": names}
                for directory, names in receiver.completed
            ]
        (session / "receiver-result.json").write_text(
            json.dumps(report, indent=2), encoding="utf-8"
        )
        print(json.dumps(report), flush=True)
    return 0 if report["started"] and not report["error"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
