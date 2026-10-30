# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import ipaddress
import logging
import socket
import threading
from pathlib import Path

from .native import NativeRadio
from .network import Announcement, find_interface
from .receiver import Receiver, Settings

LOG = logging.getLogger(__name__)


def confirm(sender, files):
    print(f"\nUnverified sender {sender!r} offers {len(files)} file(s):")
    for name in files:
        print(f"  {name!r}")
    try:
        return input("Accept this transfer? [y/N] ").strip().lower() == "y"
    except EOFError:
        return False


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Experimental Windows AirDrop receiver. Requires an AWDL radio backend."
    )
    parser.add_argument("action", choices=["receive"])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("-i", "--interface", default=None)
    source.add_argument("--radio-host", type=Path,
                        help="Manage awdl-host.exe with a preinstalled AX211 development driver")
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--pnvm", type=Path)
    parser.add_argument("--wintun", type=Path)
    parser.add_argument("-n", "--name", default=socket.gethostname())
    parser.add_argument("--output", type=Path, default=Path.cwd() / "received")
    parser.add_argument(
        "--state-dir", type=Path, default=Path.cwd() / ".opendrop-state"
    )
    parser.add_argument("--port", type=int, default=8771)
    parser.add_argument("--max-mib", type=int, default=1024)
    parser.add_argument(
        "--accept", action="store_true", help="Automatically accept file offers"
    )
    parser.add_argument(
        "--loopback-test",
        action="store_true",
        help="Bind only ::1 without discovery, for protocol tests (not AirDrop)",
    )
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535 or not 1 <= args.max_mib <= 4096:
        parser.error("Port must be 0-65535 and max-mib must be 1-4096")
    if args.radio_host:
        if args.loopback_test or not all((args.firmware, args.pnvm, args.wintun)):
            parser.error("--radio-host requires --firmware, --pnvm and --wintun, without --loopback-test")
    elif any((args.firmware, args.pnvm, args.wintun)):
        parser.error("Native firmware/Wintun options require --radio-host")
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    announcement = None
    receiver = None
    native = None
    exit_code = 0
    try:
        if args.loopback_test:
            address = ("::1", args.port)
            print(
                "LOCAL PROTOCOL TEST ONLY: this will not appear on your iPhone.",
                flush=True,
            )
            interface = None
        else:
            if args.radio_host:
                native = NativeRadio.start(args.radio_host, args.firmware, args.pnvm,
                                           args.wintun, args.name)
                interface = find_interface(str(native.index), address=native.address)
            else:
                interface = find_interface(args.interface or "awdl0")
            if ipaddress.ip_address(interface.address).is_loopback:
                raise RuntimeError("Use --loopback-test for loopback testing")
            address = (interface.address, args.port, 0, interface.scope)
        settings = Settings(
            args.output, args.state_dir, args.name, args.max_mib * 1024 * 1024
        )
        receiver = Receiver(
            address, settings, (lambda sender, files: True) if args.accept else confirm
        )
        receiver.start()
        if interface:
            announcement = Announcement(interface, receiver.server_port)
        print(
            f"Receiver listening on [{address[0]}]:{receiver.server_port}", flush=True
        )
        print(f"Destination: {args.output.resolve()}", flush=True)
        if interface and not native:
            print(
                "AirDrop discovery uses the selected external AWDL interface.",
                flush=True,
            )
        if native:
            while not native.ended.wait(0.5):
                native.check()
            native.check()
        else:
            threading.Event().wait()
    except KeyboardInterrupt:
        pass
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"Cannot start receiver: {exc}", flush=True)
        exit_code = 2
    finally:
        for label, resource in (("Discovery", announcement), ("Receiver", receiver),
                                ("Radio", native)):
            if resource:
                try:
                    resource.close()
                except Exception:
                    LOG.exception("%s shutdown failed", label)
                    exit_code = 2
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
