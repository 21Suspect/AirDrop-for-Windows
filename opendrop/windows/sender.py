# SPDX-License-Identifier: GPL-3.0-or-later
"""Scoped AirDrop sender. Wire format follows the pinned Owly protocol reference."""
import gzip
import hashlib
import http.client
import ipaddress
import os
import plistlib
import secrets
import socket
import ssl
import struct
import tempfile
import threading
import uuid
import zlib
from dataclasses import dataclass
from pathlib import Path

from .archive import safe_name


class Cancelled(Exception):
    pass


class Cancellation:
    def __init__(self):
        self.event = threading.Event()
        self.lock = threading.Lock()
        self.socket = None

    def check(self):
        if self.event.is_set():
            raise Cancelled("Transfer canceled")

    def attach(self, sock):
        with self.lock:
            self.socket = sock
            if self.event.is_set() and sock:
                sock.close()
        self.check()

    def cancel(self):
        self.event.set()
        with self.lock:
            if self.socket:
                try:
                    self.socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self.socket.close()


@dataclass
class Device:
    id: str
    address: str
    port: int
    scope: int
    flags: int = 0
    name: str = ""
    model: str = ""
    fingerprint: str = ""


class Connection(http.client.HTTPConnection):
    def __init__(self, device, cancel, timeout=15):
        super().__init__(device.address, device.port, timeout=timeout)
        self.device, self.cancel = device, cancel
        self.fingerprint = ""

    def connect(self):
        self.cancel.check()
        ip = ipaddress.IPv6Address(self.device.address)
        if not (ip.is_link_local and self.device.scope > 0) and not ip.is_loopback:
            raise ValueError("AirDrop connections must use the selected local IPv6 interface")
        raw = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        raw.settimeout(self.timeout)
        self.cancel.attach(raw)
        try:
            raw.connect((str(ip), self.port, 0, self.device.scope))
            # AirDrop Everyone uses self-signed identities, not public web PKI.
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            self.sock = context.wrap_socket(raw, server_hostname=None)
            self.cancel.attach(self.sock)
            self.fingerprint = hashlib.sha256(self.sock.getpeercert(binary_form=True)).hexdigest()
            if self.device.fingerprint and self.fingerprint != self.device.fingerprint:
                raise RuntimeError("This device changed its identity. Refresh nearby devices before sending.")
        except BaseException:
            raw.close()
            if self.sock:
                self.sock.close()
            raise


def response(connection):
    reply = connection.getresponse()
    body = reply.read(1024 * 1024 + 1)
    if len(body) > 1024 * 1024:
        raise ValueError("Oversized AirDrop response")
    return reply.status, body


def post_plist(connection, path, value):
    body = plistlib.dumps(value, fmt=plistlib.FMT_BINARY)
    connection.request("POST", path, body, {
        "User-Agent": "AirDrop/1.0", "Content-Type": "application/octet-stream",
        "Connection": "keep-alive",
    })
    return response(connection)


def discover(device, cancel=None):
    connection = Connection(device, cancel or Cancellation(), timeout=5)
    try:
        code, body = post_plist(connection, "/Discover", {})
        if code != 200:
            return None
        value = plistlib.loads(body)
        name = value.get("ReceiverComputerName")
        if not isinstance(name, str) or not name or len(name) > 256:
            return None
        device.name = "".join(c for c in name if c.isprintable())
        model = value.get("ReceiverModelName", "")
        device.model = model[:128] if isinstance(model, str) else ""
        device.fingerprint = connection.fingerprint
        return device
    finally:
        connection.close()


UTI = {
    ".jpg": "public.jpeg", ".jpeg": "public.jpeg", ".png": "public.png",
    ".heic": "public.heic", ".heif": "public.heif", ".gif": "com.compuserve.gif",
    ".mov": "com.apple.quicktime-movie", ".mp4": "public.mpeg-4",
    ".pdf": "com.adobe.pdf", ".txt": "public.plain-text", ".zip": "public.zip-archive",
    ".mp3": "public.mp3", ".json": "public.json", ".csv": "public.comma-separated-values-text",
}


def cpio_header(name, size, inode=1, mtime=0, mode=0o100644):
    name = name.encode("utf-8") + b"\0"
    if size < 0 or size >= 8**11 or len(name) >= 8**6:
        raise ValueError("File exceeds CPIO format limits")
    return (b"070707" + "".join(f"{n:06o}" for n in (0, inode, mode, 0, 0, 1, 0)).encode()
            + f"{mtime:011o}{len(name):06o}{size:011o}".encode() + name)


class DVZipWriter:
    def __init__(self, stream):
        self.stream, self.buffer = stream, bytearray()

    def piece(self, block):
        compressed = zlib.compress(block, level=1)
        data, flag = (compressed, 0) if len(compressed) < len(block) else (block, 0x80000000)
        self.stream.write(struct.pack(">I", len(data) | flag))
        self.stream.write(data)

    def write(self, value):
        self.buffer.extend(value)
        while len(self.buffer) >= 65536:
            self.piece(bytes(self.buffer[:65536]))
            del self.buffer[:65536]

    def close(self):
        if self.buffer:
            self.piece(bytes(self.buffer))
            self.buffer.clear()


def prepare(paths, dvzip, cancel, event, limit):
    if not 1 <= len(paths) <= 256:
        raise ValueError("Choose between 1 and 256 files")
    files, total, names = [], 0, set()
    for value in paths:
        path = Path(value)
        if path.is_symlink() or not path.is_file():
            raise ValueError("Choose regular files; folders and links are not supported yet")
        name = safe_name(path.name)
        if name.casefold() in names:
            raise ValueError("Selected files must have different names")
        names.add(name.casefold())
        stat = path.stat()
        total += stat.st_size
        if total > limit:
            raise ValueError("Selected files exceed the transfer size limit")
        files.append((path, stat))
    archive = tempfile.TemporaryFile(prefix="opendrop-send-")  # noqa: SIM115 -- ownership passes to send()
    encoder = DVZipWriter(archive) if dvzip else gzip.GzipFile(fileobj=archive, mode="wb", compresslevel=1, mtime=0)
    offer, done = [], 0
    try:
        for i, (path, initial) in enumerate(files, 1):
            cancel.check()
            with path.open("rb") as source:
                before = os.fstat(source.fileno())
                if (before.st_size, before.st_mtime_ns) != (initial.st_size, initial.st_mtime_ns):
                    raise RuntimeError("A selected file changed while preparing the transfer")
                encoder.write(cpio_header("./" + path.name, before.st_size, i, max(0, int(before.st_mtime))))
                remaining = before.st_size
                while remaining:
                    cancel.check()
                    block = source.read(min(65536, remaining))
                    if not block:
                        raise RuntimeError("A selected file changed while reading")
                    encoder.write(block)
                    remaining -= len(block)
                    done += len(block)
                    event("preparing", done, total)
                after = os.fstat(source.fileno())
                if (after.st_size, after.st_mtime_ns) != (before.st_size, before.st_mtime_ns):
                    raise RuntimeError("A selected file changed while preparing the transfer")
            offer.append({"FileName": path.name, "FileBomPath": "./" + path.name,
                          "FileIsDirectory": False, "FileSize": before.st_size,
                          "FileType": UTI.get(path.suffix.lower(), "public.data"),
                          "ShouldConvertMediaFormats": False})
        encoder.write(cpio_header("TRAILER!!!", 0, mode=0))
        encoder.close()
        length = archive.tell()
        archive.seek(0)
        return offer, archive, length
    except BaseException:
        archive.close()
        raise


def send(device, paths, name, sender_id, cancel=None, event=lambda *args: None,
         limit=4 * 1024**3):
    cancel = cancel or Cancellation()
    offer, archive, length = prepare(paths, bool(device.flags & 2), cancel, event, limit)
    identifier = str(uuid.uuid4()).upper()
    connection = Connection(device, cancel, timeout=30)
    try:
        cancel.check()
        event("waiting", 0, length)
        code, _ = post_plist(connection, "/Ask", {
            "TransferID": {"id": identifier}, "TransferType": {"files": {}},
            "SenderComputerName": name, "SenderModelName": "Windows",
            "SenderID": sender_id or secrets.token_hex(6), "BundleID": "com.apple.finder",
            "ConvertMediaFormats": False, "Files": offer, "Items": [],
        })
        if code != 200:
            raise RuntimeError(f"The recipient declined or is unavailable (HTTP {code})")
        cancel.check()
        connection.putrequest("POST", "/Upload")
        for key, value in {
            "TransferID": identifier, "TotalBytes": "0", "User-Agent": "AirDrop/1.0",
            "Content-Type": "application/x-dvzip" if device.flags & 2 else "application/x-cpio",
            "Connection": "keep-alive", "Content-Length": str(length),
        }.items():
            connection.putheader(key, value)
        connection.endheaders()
        done = 0
        while True:
            cancel.check()
            block = archive.read(65536)
            if not block:
                break
            connection.send(block)
            done += len(block)
            event("sending", done, length)
        event("confirming", done, length)
        code, _ = response(connection)
        if code != 200:
            raise RuntimeError(f"The recipient did not confirm receipt (HTTP {code})")
        return [file["FileName"] for file in offer]
    finally:
        connection.close()
        archive.close()
