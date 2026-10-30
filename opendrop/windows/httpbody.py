# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict, size-bounded HTTP request framing."""

import re

from .archive import InvalidTransfer, exact


class BodyReader:
    def __init__(self, stream, headers, limit):
        self.stream = stream
        self.limit = limit
        self.total = 0
        self.remaining = 0
        self.done = False
        lengths = headers.get_all("Content-Length", [])
        encodings = headers.get_all("Transfer-Encoding", [])
        if len(lengths) > 1 or len(encodings) > 1 or (lengths and encodings):
            raise InvalidTransfer("Ambiguous HTTP request framing")
        self.chunked = bool(encodings)
        if self.chunked:
            if encodings[0].lower().strip() != "chunked":
                raise InvalidTransfer("Unsupported transfer encoding")
        elif lengths:
            if not re.fullmatch(r"[0-9]+", lengths[0].strip()):
                raise InvalidTransfer("Invalid Content-Length")
            self.remaining = int(lengths[0])
            if self.remaining > limit:
                raise InvalidTransfer("Request exceeds size limit")
            self.done = self.remaining == 0
        else:
            raise InvalidTransfer("Request body requires framing")

    def line(self):
        line = self.stream.readline(8193)
        if len(line) > 8192 or not line.endswith(b"\r\n"):
            raise InvalidTransfer("Invalid HTTP chunk framing")
        return line[:-2]

    def next_chunk(self):
        line = self.line()
        size = line.split(b";", 1)[0]
        if not re.fullmatch(rb"[0-9A-Fa-f]{1,16}", size):
            raise InvalidTransfer("Invalid chunk length")
        self.remaining = int(size, 16)
        if self.total + self.remaining > self.limit:
            raise InvalidTransfer("Request exceeds size limit")
        if not self.remaining:
            trailer_size = 0
            while True:
                trailer = self.line()
                trailer_size += len(trailer) + 2
                if trailer_size > 8192:
                    raise InvalidTransfer("HTTP trailers exceed limit")
                if not trailer:
                    break
            self.done = True

    def read(self, size):
        if size < 0:
            raise ValueError("A bounded read size is required")
        if not size or self.done:
            return b""
        if self.chunked and not self.remaining:
            self.next_chunk()
            if self.done:
                return b""
        amount = min(size, self.remaining)
        data = exact(self.stream, amount)
        self.remaining -= amount
        self.total += amount
        if not self.remaining:
            if self.chunked:
                if exact(self.stream, 2) != b"\r\n":
                    raise InvalidTransfer("Missing chunk delimiter")
            else:
                self.done = True
        return data

    def all(self):
        result = bytearray()
        while True:
            data = self.read(65536)
            if not data:
                return bytes(result)
            result.extend(data)
