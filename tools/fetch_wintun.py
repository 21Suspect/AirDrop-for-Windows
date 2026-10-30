#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the official unmodified AMD64 Wintun DLL and its distribution license."""
import argparse
import hashlib
import io
import json
import urllib.request
import zipfile
from pathlib import Path

URL = "https://www.wintun.net/builds/wintun-0.14.1.zip"
ZIP_SHA256 = "07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51"
DLL_SHA256 = "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce"
MAX_DOWNLOAD = 16 * 1024 * 1024


def fetch(destination: Path):
    with urllib.request.urlopen(URL, timeout=30) as response:
        data = response.read(MAX_DOWNLOAD + 1)
    if len(data) > MAX_DOWNLOAD or hashlib.sha256(data).hexdigest() != ZIP_SHA256:
        raise ValueError("Wintun archive does not match the official pinned SHA256")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        dll = archive.read("wintun/bin/amd64/wintun.dll")
        license_text = archive.read("wintun/LICENSE.txt")
    if hashlib.sha256(dll).hexdigest() != DLL_SHA256:
        raise ValueError("Wintun AMD64 DLL does not match its pinned SHA256")
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "wintun.dll").write_bytes(dll)
    (destination / "WINTUN-LICENSE.txt").write_bytes(license_text)
    result = {"source": URL, "archive_sha256": ZIP_SHA256, "dll_sha256": DLL_SHA256}
    (destination / "wintun-provenance.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    print(json.dumps(fetch(args.destination)))
