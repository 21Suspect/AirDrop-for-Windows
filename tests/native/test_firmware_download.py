"""Verify authentication and selective extraction before Intel firmware is used."""
import base64
import hashlib
import importlib.util
import io
import tarfile
from pathlib import Path

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

_SPEC = importlib.util.spec_from_file_location(
    "fetch_intel_firmware", Path(__file__).resolve().parents[2] / "tools" / "fetch_intel_firmware.py"
)
fetcher = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(fetcher)


def archive_for(files, extra=None):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name, contents in files.items():
            info = tarfile.TarInfo("firmware/" + name)
            info.size = len(contents)
            archive.addfile(info, io.BytesIO(contents))
        if extra:
            info, contents = extra
            archive.addfile(info, io.BytesIO(contents))
    return buffer.getvalue()


@pytest.fixture
def signed_download(monkeypatch):
    private = Ed25519PrivateKey.generate()
    key_id = b"Edtest-key"
    public = key_id + private.public_key().public_bytes_raw()
    files = {"iwx-so-a0-gf-a0-77": b"independent fake firmware", "iwx-license": b"test license"}
    monkeypatch.setattr(fetcher, "PUBLIC_KEY", base64.b64encode(public).decode())
    monkeypatch.setattr(fetcher, "FILES", {
        name: hashlib.sha256(data).hexdigest() for name, data in files.items()
    })
    network = {}

    def set_archive(archive):
        payload = b"SHA256 (" + fetcher.PACKAGE.encode() + b") = "
        payload += base64.b64encode(hashlib.sha256(archive).digest()) + b"\n"
        signature = key_id + private.sign(payload)
        network["SHA256.sig"] = b"untrusted comment: test fixture\n"
        network["SHA256.sig"] += base64.b64encode(signature) + b"\n" + payload
        network[fetcher.PACKAGE] = archive

    def download(name, limit):
        data = network[name]
        assert len(data) <= limit
        return data

    monkeypatch.setattr(fetcher, "download", download)
    set_archive(archive_for(files))
    return files, network, set_archive


def test_authenticated_download_keeps_license_and_provenance(signed_download, tmp_path):
    files, _, _ = signed_download
    target = tmp_path / "firmware"
    report = fetcher.fetch(target)
    assert report["manifest_signature_verified"]
    assert report["loaded_on_hardware"] is False
    assert (target / "provenance.json").is_file()
    for name, contents in files.items():
        assert (target / name).read_bytes() == contents


def test_tampered_signature_writes_nothing(signed_download, tmp_path):
    _, network, _ = signed_download
    lines = network["SHA256.sig"].split(b"\n", 2)
    signature = bytearray(base64.b64decode(lines[1]))
    signature[-1] ^= 1
    network["SHA256.sig"] = lines[0]+b"\n"+base64.b64encode(signature)+b"\n"+lines[2]
    target = tmp_path / "firmware"
    with pytest.raises(InvalidSignature):
        fetcher.fetch(target)
    assert not target.exists()


def test_tampered_archive_writes_nothing(signed_download, tmp_path):
    _, network, _ = signed_download
    network[fetcher.PACKAGE] += b"tampering"
    target = tmp_path / "firmware"
    with pytest.raises(ValueError, match="package checksum mismatch"):
        fetcher.fetch(target)
    assert not target.exists()


def test_signed_but_unpinned_image_writes_nothing(signed_download, tmp_path):
    files, _, set_archive = signed_download
    set_archive(archive_for({**files, "iwx-so-a0-gf-a0-77": b"wrong firmware"}))
    target = tmp_path / "firmware"
    with pytest.raises(ValueError, match="Pinned image checksum mismatch"):
        fetcher.fetch(target)
    assert not target.exists()


def test_missing_license_writes_nothing(signed_download, tmp_path):
    files, _, set_archive = signed_download
    set_archive(archive_for({"iwx-so-a0-gf-a0-77": files["iwx-so-a0-gf-a0-77"]}))
    target = tmp_path / "firmware"
    with pytest.raises(ValueError, match="license is missing"):
        fetcher.fetch(target)
    assert not target.exists()


def test_duplicate_member_rejected(signed_download, tmp_path):
    files, _, set_archive = signed_download
    info = tarfile.TarInfo("firmware/iwx-license")
    info.size = len(files["iwx-license"])
    set_archive(archive_for(files, (info, files["iwx-license"])))
    target = tmp_path / "firmware"
    with pytest.raises(ValueError, match="archive member"):
        fetcher.fetch(target)
    assert not target.exists()


def test_symlink_cannot_supply_firmware(signed_download, tmp_path):
    files, _, set_archive = signed_download
    info = tarfile.TarInfo("firmware/iwx-so-a0-gf-a0-77")
    info.type = tarfile.SYMTYPE
    info.linkname = "../../outside"
    set_archive(archive_for({"iwx-license": files["iwx-license"]}, (info, b"")))
    target = tmp_path / "firmware"
    with pytest.raises(ValueError, match="archive member"):
        fetcher.fetch(target)
    assert not target.exists()


def test_other_paths_are_never_extracted(signed_download, tmp_path):
    files, _, set_archive = signed_download
    info = tarfile.TarInfo("../outside")
    info.size = 4
    set_archive(archive_for(files, (info, b"data")))
    target = tmp_path / "firmware"
    fetcher.fetch(target)
    assert not (tmp_path / "outside").exists()
    assert sorted(p.name for p in target.iterdir()) == sorted(
        [*files, "SHA256.sig", "provenance.json"]
    )
