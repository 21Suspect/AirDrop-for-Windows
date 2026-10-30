#!/usr/bin/env python3
"""Fetch the unmodified, pinned Intel SO/GF firmware used by this development port.

The OpenBSD checksum manifest is authenticated with its pinned firmware key.
No firmware is loaded into hardware by this command.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import re
import tarfile
import urllib.request
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

BASE = "https://firmware.openbsd.org/firmware/snapshots/"
PACKAGE = "iwx-firmware-20251021p0.tgz"
PUBLIC_KEY = "RWRjzXzC89s1iQC9P3xgcdeN6OUoJz9t7ggTY0xBb1W8PfeOzAdwod66"
FILES = {
    "iwx-so-a0-gf-a0-77": "87590ea9535a8939fddb296bc7622f3a900f28f47b45cb4e435a2a1d5f496ccd",
    "iwx-so-a0-gf-a0.pnvm": "b37f038b3467ed568d107a9c1dfd916fa7e8cbd58b2f9bec0c2c53297ccfcab7",
    "iwx-license": "16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5",
}


def download(name: str, limit: int) -> bytes:
    with urllib.request.urlopen(BASE + name, timeout=30) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{name} exceeds its size limit")
    return data


def fetch(destination: Path) -> dict:
    manifest = download("SHA256.sig", 64 * 1024)
    lines = manifest.split(b"\n", 2)
    if len(lines) != 3:
        raise ValueError("Malformed signed manifest")
    key = base64.b64decode(PUBLIC_KEY, validate=True)
    signature = base64.b64decode(lines[1], validate=True)
    if len(signature) != 74 or len(key) != 42 or key[:2] != b"Ed" or key[:10] != signature[:10]:
        raise ValueError("Unexpected firmware signing key")
    Ed25519PublicKey.from_public_bytes(key[10:]).verify(signature[10:], lines[2])
    match = re.findall(rb"^SHA256 \(" + re.escape(PACKAGE.encode()) +
                       rb"\) = ([A-Za-z0-9+/=]+)$", lines[2], flags=re.MULTILINE)
    if len(match) != 1:
        raise ValueError("Pinned firmware package is not uniquely listed")
    expected = base64.b64decode(match[0], validate=True)
    archive = download(PACKAGE, 32 * 1024 * 1024)
    if hashlib.sha256(archive).digest() != expected:
        raise ValueError("Firmware package checksum mismatch")
    verified = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as package:
        for member in package.getmembers():
            name = member.name.removeprefix("firmware/")
            if member.name != "firmware/" + name or name not in FILES:
                continue
            if name in verified or not member.isfile() or member.size > 8 * 1024 * 1024:
                raise ValueError("Unexpected firmware archive member")
            stream = package.extractfile(member)
            if stream is None:
                raise ValueError("Missing firmware member")
            data = stream.read(8 * 1024 * 1024 + 1)
            if hashlib.sha256(data).hexdigest() != FILES[name]:
                raise ValueError(f"Pinned image checksum mismatch: {name}")
            verified[name] = data
    if verified.keys() != FILES.keys():
        raise ValueError("Firmware or required Intel license is missing")
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in verified.items():
        temporary = destination / (name + ".tmp")
        temporary.write_bytes(data)
        temporary.replace(destination / name)
    (destination / "SHA256.sig").write_bytes(manifest)
    report = {
        "source": BASE + PACKAGE,
        "package_sha256": expected.hex(),
        "manifest_signature_verified": True,
        "signing_key_git_blob": "d8a2f02d074effef34b01399c1d4c3b98d14be3a",
        "files": FILES,
        "loaded_on_hardware": False,
    }
    (destination / "provenance.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Directory for firmware and Intel's license")
    args = parser.parse_args()
    print(json.dumps(fetch(args.directory), indent=2))


if __name__ == "__main__":
    main()
