# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows-capable receive protocol; the AWDL radio is a separate requirement."""

import json
import logging
import plistlib
import socket
import threading
import time
import uuid
import zlib
from collections import deque
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from .archive import InvalidTransfer, receive_archive, safe_name
from .httpbody import BodyReader
from .tls import server_context

LOG = logging.getLogger(__name__)
FLAGS = 0x02 | 0x08 | 0x80  # DVZip, mixed types, Discover


@dataclass
class Settings:
    output: Path
    state: Path
    name: str = "Windows PC"
    byte_limit: int = 1024 * 1024 * 1024
    timeout: float = 30
    approval_seconds: float = 120
    organized: bool = False


def transfer_id(value):
    if value is None:
        return ""
    if isinstance(value, dict):
        value = value.get("id")
    if not isinstance(value, str):
        raise InvalidTransfer("Invalid TransferID")
    try:
        return str(uuid.UUID(value))
    except ValueError as exc:
        raise InvalidTransfer("Invalid TransferID") from exc


class Receiver(ThreadingHTTPServer):
    address_family = socket.AF_INET6
    daemon_threads = True
    block_on_close = False
    allow_reuse_address = False
    request_queue_size = 16

    def __init__(self, address, settings, approve, event=None):
        if settings.byte_limit <= 0 or settings.timeout <= 0:
            raise ValueError("Limits and timeout must be positive")
        self.settings, self.approve = settings, approve
        self.event = event or (lambda *args, **kwargs: None)
        self.uploads = {}
        self.context = server_context(settings.state)
        self.grants = {}
        self.guard = threading.Lock()
        self.prompt_guard = threading.Lock()
        self.slots = threading.BoundedSemaphore(16)
        self.active = set()
        self.worker = None
        self.completed = deque(maxlen=128)
        self.stopping = False
        super().__init__(address, Handler)

    def notify(self, kind, **data):
        try:
            self.event(kind, **data)
        except Exception:
            LOG.exception("Transfer notification failed")

    def cancel_upload(self, identifier):
        with self.guard:
            connection = self.uploads.get(identifier)
        if connection:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def server_bind(self):
        self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        secure = None
        try:
            request.settimeout(self.settings.timeout)
            secure = self.context.wrap_socket(
                request, server_side=True, do_handshake_on_connect=False
            )
            with self.guard:
                if self.stopping:
                    return
                self.active.add(secure)
            secure.do_handshake()
            self.finish_request(secure, client_address)
        except (OSError, ValueError):
            LOG.debug("Peer disconnected or TLS handshake failed")
        finally:
            with self.guard:
                self.active.discard(secure)
            self.shutdown_request(secure if secure is not None else request)
            self.slots.release()

    def start(self):
        if self.worker is not None:
            raise RuntimeError("Receiver already started")
        self.worker = threading.Thread(target=self.serve_forever, daemon=True)
        self.worker.start()

    def close(self):
        with self.guard:
            self.stopping = True
            active = list(self.active)
            self.grants.clear()
        for connection in active:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        if self.worker is not None:
            self.shutdown()
            self.worker.join(timeout=5)
        self.server_close()

    def allow(self, peer, identifier, files):
        with self.guard:
            now = time.monotonic()
            self.grants = {k: v for k, v in self.grants.items() if v[0] > now}
            if len(self.grants) >= 32:
                raise InvalidTransfer("Too many pending transfers")
            self.grants[(peer, identifier)] = (
                now + self.settings.approval_seconds,
                files,
            )

    def take(self, peer, identifier):
        with self.guard:
            grant = self.grants.pop((peer, identifier), None)
        if grant is None or grant[0] <= time.monotonic():
            raise InvalidTransfer("Upload has no unexpired, accepted Ask request")
        return grant[1]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "OpenDropWindows/0.1"
    sys_version = ""

    def log_message(self, fmt, *args):
        LOG.debug("%s", (fmt % args).encode("unicode_escape").decode("ascii"))

    def handle_expect_100(self):
        # Delay the interim response until framing and upload consent are checked.
        return True

    def respond(self, code=200, data=b"", close=False):
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        if data:
            self.send_header("Content-Type", "application/octet-stream")
        if close:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        if data:
            self.wfile.write(data)

    def body(self, limit):
        reader = BodyReader(self.rfile, self.headers, limit)
        expect = self.headers.get("Expect", "").lower().strip()
        if expect and expect != "100-continue":
            raise InvalidTransfer("Unsupported Expect header")
        if expect:
            self.send_response_only(100)
            self.end_headers()
            self.wfile.flush()
        return reader

    def identity(self):
        return {
            "ReceiverComputerName": self.server.settings.name,
            "ReceiverModelName": "Windows PC",
        }

    def read_plist(self):
        data = self.body(1024 * 1024).all()
        try:
            value = plistlib.loads(data) if data else {}
        except (ValueError, TypeError, OverflowError, RecursionError) as exc:
            raise InvalidTransfer("Invalid request plist") from exc
        if not isinstance(value, dict):
            raise InvalidTransfer("Expected a plist dictionary")
        return value

    def do_GET(self):
        self.respond(404, close=True)

    def do_HEAD(self):
        self.respond(404, close=True)

    def do_POST(self):
        try:
            if self.path == "/Discover":
                self.read_plist()
                reply = self.identity()
                reply["ReceiverMediaCapabilities"] = json.dumps({"Version": 1}).encode()
                self.respond(data=plistlib.dumps(reply, fmt=plistlib.FMT_BINARY))
            elif self.path == "/Ask":
                self.ask()
            elif self.path == "/Upload":
                self.upload()
            else:
                self.respond(404, close=True)
        except (InvalidTransfer, ValueError, EOFError, zlib.error) as exc:
            LOG.warning("Rejected transfer: %s", str(exc))
            self.respond(400, close=True)
        except OSError:
            LOG.warning("Transfer interrupted or destination unavailable")
            try:
                self.respond(500, close=True)
            except OSError:
                pass

    def ask(self):
        request = self.read_plist()
        entries = request.get("Files")
        if (
            not isinstance(entries, list)
            or not 1 <= len(entries) <= 256
            or request.get("Items")
        ):
            raise InvalidTransfer(
                "Expected 1-256 files; URL-only transfers unsupported"
            )
        files = []
        for entry in entries:
            if not isinstance(entry, dict) or entry.get("FileIsDirectory", False):
                raise InvalidTransfer("Directory offers unsupported")
            name = safe_name(entry.get("FileBomPath", entry.get("FileName")))
            files.append(name)
        if len({p.casefold() for p in files}) != len(files):
            raise InvalidTransfer("Duplicate Windows file names")
        identifier = transfer_id(request.get("TransferID"))
        sender = request.get("SenderComputerName", "Unknown sender")
        if not isinstance(sender, str) or len(sender) > 256:
            raise InvalidTransfer("Invalid sender name")
        if not self.server.prompt_guard.acquire(blocking=False):
            self.respond(503, close=True)
            return
        try:
            allowed = self.server.approve(sender, files)
        finally:
            self.server.prompt_guard.release()
        if not allowed:
            self.respond(403, close=True)
            return
        self.server.allow(self.client_address[0], identifier, files)
        self.respond(data=plistlib.dumps(self.identity(), fmt=plistlib.FMT_BINARY))

    def upload(self):
        media = self.headers.get("Content-Type", "").split(";", 1)[0].lower().strip()
        if media not in ("application/x-cpio", "application/x-dvzip"):
            self.respond(406, close=True)
            return
        if self.headers.get("Content-Encoding", "identity").lower() not in (
            "identity",
            "gzip",
        ):
            raise InvalidTransfer("Unsupported content encoding")
        identifiers = self.headers.get_all("TransferID", [])
        if len(identifiers) > 1:
            raise InvalidTransfer("Ambiguous TransferID")
        identifier = transfer_id(identifiers[0] if identifiers else None)
        files = self.server.take(self.client_address[0], identifier)
        settings = self.server.settings
        body = self.body(settings.byte_limit + 2 * 1024 * 1024)
        event_id = identifier or uuid.uuid4().hex
        with self.server.guard:
            self.server.uploads[event_id] = self.connection
        total = body.remaining if not body.chunked else 0
        self.server.notify("receiving", id=event_id, files=files, done=0, total=total)
        progress = ProgressReader(body, lambda done: self.server.notify(
            "receive_progress", id=event_id, done=done, total=total))
        try:
            destination, names = receive_archive(
                progress, media, settings.output, files, settings.byte_limit,
                staging_root=settings.state / "incoming" if settings.organized else None,
                organized=settings.organized
            )
            with self.server.guard:
                self.server.completed.append((destination, names))
            self.server.notify("received", id=event_id, files=names, path=str(destination))
            LOG.info("Received %d file(s) in %s", len(names), destination)
            self.respond(close=True)
        except BaseException as exc:
            self.server.notify("receive_failed", id=event_id, error=str(exc))
            raise
        finally:
            with self.server.guard:
                self.server.uploads.pop(event_id, None)


class ProgressReader:
    def __init__(self, source, callback):
        self.source, self.callback = source, callback
        self.last = 0.0

    def read(self, size):
        data = self.source.read(size)
        now = time.monotonic()
        if now - self.last >= 0.15:
            self.callback(self.source.total)
            self.last = now
        return data
