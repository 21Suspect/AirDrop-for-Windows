# SPDX-License-Identifier: GPL-3.0-or-later
import ipaddress
import logging
import secrets
import threading
from dataclasses import dataclass

import ifaddr
from zeroconf import IPVersion, ServiceInfo, Zeroconf

from .receiver import FLAGS

LOG = logging.getLogger(__name__)


@dataclass
class Interface:
    name: str
    index: int
    address: str
    scope: int


def find_interface(name, address=None):
    for adapter in ifaddr.get_adapters():
        names = {adapter.name, adapter.nice_name, str(adapter.index)}
        names.update(ip.nice_name for ip in adapter.ips)
        if name not in names:
            continue
        addresses = [ip for ip in adapter.ips if ip.is_IPv6
                     and (address is None or ipaddress.ip_address(ip.ip[0]) ==
                          ipaddress.ip_address(address))]
        addresses.sort(key=lambda ip: not ipaddress.ip_address(ip.ip[0]).is_link_local)
        if addresses:
            ip = addresses[0]
            scope = (
                (ip.ip[2] or adapter.index)
                if ipaddress.ip_address(ip.ip[0]).is_link_local
                else 0
            )
            return Interface(ip.nice_name, scope or adapter.index, ip.ip[0], scope)
    raise RuntimeError(
        f"No IPv6 interface matching {name!r}. A working AWDL radio/backend must "
        "create an interface first. Start the native host with the development "
        "driver installed, or supply an already running AWDL interface."
    )


class Announcement:
    def __init__(self, interface, port):
        self.zc = Zeroconf(interfaces=[interface.index], ip_version=IPVersion.V6Only)
        identifier = secrets.token_hex(6)
        self.identifier = identifier
        self.info = ServiceInfo(
            "_airdrop._tcp.local.",
            identifier + "._airdrop._tcp.local.",
            addresses=[ipaddress.ip_address(interface.address).packed],
            port=port,
            properties={b"flags": str(FLAGS).encode()},
            server="opendrop-" + identifier + ".local.",
            interface_index=interface.index,
        )
        self.stop_event = threading.Event()
        self.worker = None
        try:
            self.zc.register_service(self.info)
            self.worker = threading.Thread(target=self.repeat, daemon=True)
            self.worker.start()
        except BaseException:
            self.zc.close()
            raise

    def repeat(self):
        while not self.stop_event.wait(5):
            try:
                self.zc.update_service(self.info)
            except Exception:
                LOG.warning("mDNS announcement failed", exc_info=True)

    def close(self):
        self.stop_event.set()
        if self.worker:
            self.worker.join(timeout=10)
        try:
            self.zc.unregister_service(self.info)
        finally:
            self.zc.close()


class Discovery:
    """Bounded resolver pool; callbacks never block Zeroconf's event loop."""
    def __init__(self, announcement, interface, changed):
        import concurrent.futures

        from zeroconf import ServiceBrowser
        self.zc, self.interface, self.changed = announcement.zc, interface, changed
        self.own = announcement.info.name
        self.lock = threading.Lock()
        self.names, self.pending, self.devices = set(), set(), {}
        self.closed = threading.Event()
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=2, thread_name_prefix="discover")
        self.browser = ServiceBrowser(self.zc, "_airdrop._tcp.local.", listener=self)
        self.worker = threading.Thread(target=self.repeat, daemon=True)
        self.worker.start()

    def add_service(self, zc, kind, name):
        if name == self.own:
            return
        with self.lock:
            if len(self.names) >= 64 and name not in self.names:
                return
            self.names.add(name)
        self.resolve(name)

    def update_service(self, zc, kind, name):
        self.add_service(zc, kind, name)

    def remove_service(self, zc, kind, name):
        with self.lock:
            self.names.discard(name)
            self.devices.pop(name, None)
            snapshot = list(self.devices.values())
        self.changed(snapshot)

    def resolve(self, name):
        with self.lock:
            if self.closed.is_set() or name in self.pending or len(self.pending) >= 8:
                return
            self.pending.add(name)
        self.pool.submit(self._resolve, name)

    def _resolve(self, name):
        from .sender import Device, discover
        found = None
        try:
            info = self.zc.get_service_info("_airdrop._tcp.local.", name, timeout=1500)
            if info and 0 < info.port <= 65535:
                for raw in info.addresses_by_version(IPVersion.V6Only):
                    if len(raw) != 16:
                        continue
                    address = ipaddress.IPv6Address(raw)
                    if not address.is_link_local or str(address) == self.interface.address:
                        continue
                    flags = int(info.properties.get(b"flags", b"0"))
                    found = discover(Device(name, str(address), info.port, self.interface.index, flags))
                    if found:
                        break
        except Exception:
            LOG.debug("A nearby device is not accepting discovery yet", exc_info=True)
        finally:
            with self.lock:
                self.pending.discard(name)
                if name in self.names and not self.closed.is_set():
                    if found:
                        self.devices[name] = found
                    else:
                        self.devices.pop(name, None)
                snapshot = list(self.devices.values())
            if not self.closed.is_set():
                self.changed(snapshot)

    def repeat(self):
        while not self.closed.wait(10):
            with self.lock:
                names = list(self.names)
            for name in names:
                self.resolve(name)

    def close(self):
        self.closed.set()
        self.browser.cancel()
        self.worker.join(timeout=2)
        self.pool.shutdown(wait=True, cancel_futures=True)
