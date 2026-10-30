# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded Apple DVZip / ODC CPIO decoding without libarchive DLLs."""

import gzip
import os
import re
import shutil
import stat
import struct
import tempfile
import unicodedata
import uuid
import zlib
from pathlib import Path, PurePosixPath

BLOCK_LIMIT = 1024 * 1024
RESERVED = {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"}
RESERVED |= {f"{p}{n}" for p in ("COM", "LPT") for n in "123456789\u00b9\u00b2\u00b3"}


class InvalidTransfer(ValueError):
    pass


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        block = stream.read(size - len(result))
        if not block:
            raise InvalidTransfer("Truncated transfer")
        result.extend(block)
    return bytes(result)


def safe_name(name):
    if not isinstance(name, str) or not name or len(name) > 1024:
        raise InvalidTransfer("Invalid file name")
    # Apple file offers and CPIO entries can use different Unicode forms.
    # Compare and save one canonical spelling (notably macOS decomposed accents).
    name = unicodedata.normalize("NFC", name)
    while name.startswith("./"):
        name = name[2:]
    parts = name.split("/")
    if any(
        not p
        or p in (".", "..")
        or p.endswith((" ", "."))
        or len(p) > 240
        or p.split(".")[0].upper() in RESERVED
        or re.search(r'[<>:"\\|?*\x00-\x1f\x7f]', p)
        for p in parts
    ):
        raise InvalidTransfer("Unsafe Windows file name")
    return str(PurePosixPath(*parts))


class DVZipReader:
    """Each block is stored (high bit) or an independent bounded zlib stream."""

    def __init__(self, stream, limit):
        self.stream = stream
        self.limit = limit
        self.buffer = b""
        self.total = 0
        self.ended = False

    def read(self, size):
        if size < 0:
            raise ValueError("A bounded read size is required")
        if not size:
            return b""
        if not self.buffer and not self.ended:
            header = struct.unpack(">I", exact(self.stream, 4))[0]
            length = header & 0x7FFFFFFF
            if length == 0:
                self.ended = True
                return b""
            if length > BLOCK_LIMIT:
                raise InvalidTransfer("DVZip block exceeds limit")
            payload = exact(self.stream, length)
            if header >> 31:
                decoded = payload
            else:
                decoder = zlib.decompressobj()
                try:
                    decoded = decoder.decompress(payload, BLOCK_LIMIT + 1)
                except zlib.error as exc:
                    raise InvalidTransfer("Invalid DVZip block") from exc
                if (
                    len(decoded) > BLOCK_LIMIT
                    or not decoder.eof
                    or decoder.unused_data
                    or decoder.unconsumed_tail
                ):
                    raise InvalidTransfer("Invalid or oversized DVZip block")
            if not decoded:
                raise InvalidTransfer("Empty DVZip data block")
            self.total += len(decoded)
            if self.total > self.limit:
                raise InvalidTransfer("Decoded archive exceeds limit")
            self.buffer = decoded
        data, self.buffer = self.buffer[:size], self.buffer[size:]
        return data


def extract_cpio(stream, staging, expected, byte_limit, file_limit=256):
    """Validate all content, discarding only announced files' Finder sidecars."""
    expected = [safe_name(name) for name in expected]
    if len({name.casefold() for name in expected}) != len(expected):
        raise InvalidTransfer("Duplicate Windows file names")
    files = []
    seen = set()
    total = 0
    metadata_total = 0
    entries = 0
    while True:
        header = exact(stream, 76)
        if header[:6] != b"070707":
            raise InvalidTransfer("Expected Apple's ODC CPIO archive")
        try:
            if any(c < ord("0") or c > ord("7") for c in header[6:]):
                raise ValueError()
            mode = int(header[18:24], 8)
            links = int(header[36:42], 8)
            namesize = int(header[59:65], 8)
            size = int(header[65:76], 8)
        except ValueError as exc:
            raise InvalidTransfer("Invalid CPIO header") from exc
        if not 1 <= namesize <= 4096:
            raise InvalidTransfer("Invalid CPIO name length")
        raw_name = exact(stream, namesize)
        if not raw_name.endswith(b"\0") or b"\0" in raw_name[:-1]:
            raise InvalidTransfer("Invalid CPIO name")
        try:
            name = raw_name[:-1].decode("utf-8")
        except UnicodeError as exc:
            raise InvalidTransfer("Invalid UTF-8 file name") from exc
        if name == "TRAILER!!!":
            if size or set(files) != set(expected):
                raise InvalidTransfer("Archive does not match the accepted file list")
            return files
        entries += 1
        if entries > file_limit * 4:
            raise InvalidTransfer("Too many archive entries")
        if name in (".", "./") and stat.S_ISDIR(mode) and size == 0:
            continue
        name = safe_name(name)
        folded = name.casefold()
        if folded in seen:
            raise InvalidTransfer("Duplicate Windows file name")
        seen.add(folded)
        target = staging.joinpath(*PurePosixPath(name).parts)
        if stat.S_ISDIR(mode) and size == 0:
            if not any(p.startswith(name + "/") for p in expected):
                raise InvalidTransfer("Unannounced directory")
            target.mkdir(parents=True, exist_ok=True)
            continue
        if not stat.S_ISREG(mode) or links != 1:
            raise InvalidTransfer("Only regular files are accepted")
        metadata = False
        if name not in expected:
            path = PurePosixPath(name)
            # macOS includes unlisted ._name AppleDouble entries alongside a
            # listed name. They contain Finder metadata, not another offered
            # file. Never create them, and never accept arbitrary extra files.
            sibling = path.with_name(path.name[2:]).as_posix() if (
                path.name.startswith("._") and len(path.name) > 2
            ) else None
            metadata = sibling in expected
            if not metadata:
                raise InvalidTransfer(f"Unannounced file: {name!r}")
        total += size
        if metadata:
            metadata_total += size
            if metadata_total > 1024 * 1024:
                raise InvalidTransfer("Finder metadata exceeds limit")
        if total > byte_limit or len(files) >= file_limit:
            raise InvalidTransfer("Transfer exceeds file or byte limit")
        if metadata:
            left = size
            while left:
                left -= len(exact(stream, min(left, 65536)))
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as dest:
            left = size
            while left:
                data = exact(stream, min(left, 65536))
                dest.write(data)
                left -= len(data)
        files.append(name)


def receive_archive(body, content_type, output, expected, byte_limit, *, staging_root=None, organized=False):
    """Validate privately, then publish into a transfer directory or flat categories."""
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    staging_parent = Path(staging_root).resolve() if staging_root else output
    staging_parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".partial-", dir=staging_parent))
    try:
        # Bound the decoded archive, including headers and paths, separately.
        archive_limit = byte_limit + 1024 * 1024
        if content_type == "application/x-dvzip":
            stream = DVZipReader(body, archive_limit)
        else:
            # Upstream sends a gzip-compressed CPIO without Content-Encoding.
            prefix = exact(body, 2)
            stream = PrefixReader(prefix, body)
            if prefix == b"\x1f\x8b":
                stream = gzip.GzipFile(fileobj=stream, mode="rb")
        files = extract_cpio(stream, staging, expected, byte_limit)
        if isinstance(stream, DVZipReader):
            # A complete structural trailer permits replying before iOS sends
            # the final HTTP zero-chunk. Never treat a timeout as completion.
            if stream.buffer.strip(b"\0"):
                raise InvalidTransfer("Unexpected data after CPIO trailer")
        else:
            # Legacy framing must terminate, including the gzip checksum.
            padding = 0
            while True:
                tail = stream.read(65536)
                if not tail:
                    break
                padding += len(tail)
                if tail.strip(b"\0") or padding > 1024 * 1024:
                    raise InvalidTransfer("Unexpected trailing archive data")
        # Do not auto-open received content. Mark it as downloaded on Windows.
        if os.name == "nt":
            for name in files:
                path = staging.joinpath(*PurePosixPath(name).parts)
                with open(
                    str(path) + ":Zone.Identifier", "x", encoding="ascii"
                ) as zone:
                    zone.write("[ZoneTransfer]\nZoneId=3\n")
        if organized:
            from .organize import publish_files
            destination, files = publish_files(staging, output, files)
            # This is a fresh directory created above, within the checked parent.
            if staging.resolve().parent != staging_parent.resolve():
                raise RuntimeError("Unexpected staging directory")
            shutil.rmtree(staging)
            return destination, files
        destination = output / ("transfer-" + uuid.uuid4().hex)
        staging.rename(destination)
        return destination, files
    except BaseException:
        if staging.exists():
            if staging.resolve().parent != staging_parent.resolve():
                raise RuntimeError("Unexpected staging directory") from None
            shutil.rmtree(staging)
        raise


class PrefixReader:
    def __init__(self, prefix, stream):
        self.prefix, self.stream = prefix, stream

    def read(self, size):
        if size < 0:
            raise ValueError("A bounded read size is required")
        prefix, self.prefix = self.prefix[:size], self.prefix[size:]
        return (
            prefix + self.stream.read(size - len(prefix))
            if size > len(prefix)
            else prefix
        )
