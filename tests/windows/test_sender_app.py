# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual TLS/socket round trips, consent, identity pinning and cancellation."""
import os
import threading
import time

import pytest

from opendrop.windows.app_backend import Controller, validate_settings
from opendrop.windows.receiver import Receiver, Settings
from opendrop.windows.sender import (
    Cancellation,
    Cancelled,
    Device,
    discover,
    prepare,
    send,
)


@pytest.fixture
def server(tmp_path):
    events = []
    receiver = Receiver(("::1", 0), Settings(tmp_path / "received", tmp_path / "identity",
                        "Test peer", byte_limit=8 * 1024**2, timeout=3),
                        lambda sender, files: True, lambda kind, **data: events.append((kind, data)))
    receiver.start()
    yield receiver, events
    receiver.close()


@pytest.mark.parametrize("flags", [0, 2])
def test_send_multiple_files_over_real_tls_and_ack(server, tmp_path, flags):
    receiver, events = server
    paths = [tmp_path / "photo one.jpg", tmp_path / "Grüße.txt"]
    content = [os.urandom(200000), "A file with UTF-8 text 🦉".encode()]
    for path, data in zip(paths, content):
        path.write_bytes(data)
    peer = discover(Device("test", "::1", receiver.server_port, 0, flags))
    assert peer.name == "Test peer" and len(peer.fingerprint) == 64
    progress = []
    result = send(peer, paths, "Test PC", "abcdef012345", event=lambda *e: progress.append(e))
    assert result == [p.name for p in paths]
    destination, names = receiver.completed[-1]
    assert names == result
    assert [(destination / p.name).read_bytes() for p in paths] == content
    assert any(phase == "confirming" for phase, _, _ in progress)
    assert events[-1][0] == "received"


def test_decline_leaves_no_files(server, tmp_path):
    receiver, events = server
    receiver.approve = lambda *args: False
    path = tmp_path / "private.txt"
    path.write_text("Do not upload without consent")
    with pytest.raises(RuntimeError, match="declined"):
        send(Device("x", "::1", receiver.server_port, 0, 2), [path], "PC", "id")
    assert not receiver.completed
    assert not events


def test_certificate_change_prevents_upload(server, tmp_path):
    receiver, _ = server
    peer = discover(Device("test", "::1", receiver.server_port, 0, 2))
    peer.fingerprint = "0" * 64
    path = tmp_path / "photo.jpg"
    path.write_bytes(b"photo")
    with pytest.raises(RuntimeError, match="changed its identity"):
        send(peer, [path], "PC", "id")
    assert not receiver.completed


def test_cancel_mid_upload_removes_partial_files(server, tmp_path):
    receiver, events = server
    path = tmp_path / "large.bin"
    path.write_bytes(os.urandom(2 * 1024**2))
    token = Cancellation()
    def progress(phase, done, total):
        if phase == "sending":
            token.cancel()
    with pytest.raises((Cancelled, OSError)):
        send(Device("x", "::1", receiver.server_port, 0, 2), [path], "PC", "id", token, progress)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and not any(kind == "receive_failed" for kind, _ in events):
        time.sleep(0.05)
    assert not receiver.completed
    assert any(kind == "receive_failed" for kind, _ in events)
    assert not list(receiver.settings.output.glob(".partial-*"))


def test_prepare_rejects_duplicates_size_and_early_cancel(tmp_path):
    path = tmp_path / "same.txt"
    path.write_bytes(b"data")
    with pytest.raises(ValueError, match="different names"):
        prepare([path, path], True, Cancellation(), lambda *e: None, 1000)
    with pytest.raises(ValueError, match="size limit"):
        prepare([path], True, Cancellation(), lambda *e: None, 1)
    token = Cancellation()
    token.cancel()
    with pytest.raises(Cancelled):
        prepare([path], True, token, lambda *e: None, 1000)


def test_app_requires_explicit_approval_and_shutdown_declines(tmp_path):
    events = []
    controller = Controller({}, tmp_path, lambda kind, **data: events.append((kind, data)))
    controller.settings["ask_before_receiving"] = True
    result = []
    try:
        worker = threading.Thread(target=lambda: result.append(controller.approve("Phone", ["photo.jpg"])))
        worker.start()
        deadline = time.monotonic() + 2
        while not any(e[0] == "offer" for e in events) and time.monotonic() < deadline:
            time.sleep(0.01)
        offer = next(data for kind, data in events if kind == "offer")
        assert not result
        controller.command({"command": "accept", "id": "wrong-offer"})
        assert not result
        controller.command({"command": "accept", "id": offer["id"]})
        worker.join(1)
        assert result == [True]
        worker = threading.Thread(target=lambda: result.append(controller.approve("Phone", ["second.jpg"])))
        worker.start()
        controller.command({"command": "shutdown"})
        worker.join(1)
        assert result[-1] is False
    finally:
        controller.close()


def test_app_history_persists_and_settings_validate(tmp_path):
    controller = Controller({}, tmp_path, lambda *args, **kwargs: None)
    controller.record("received", ["a.txt"], path=str(tmp_path))
    controller.close()
    second = Controller({}, tmp_path, lambda *args, **kwargs: None)
    try:
        assert second.history[-1]["files"] == ["a.txt"]
        assert validate_settings({"name": "Example’s PC", "output": str(tmp_path)})["limit_gib"] == 4
        for name in ('', 'evil"argument', 'bad\nname', 'ü' * 40):
            with pytest.raises(ValueError):
                validate_settings({"name": name})
    finally:
        second.close()


def test_discovery_resolves_ipv6_only_service(monkeypatch):
    import ipaddress
    from types import SimpleNamespace

    from zeroconf import ServiceInfo

    from opendrop.windows.network import Discovery, Interface

    name = "peer._airdrop._tcp.local."
    info = ServiceInfo("_airdrop._tcp.local.", name,
                       addresses=[ipaddress.IPv6Address("fe80::1234").packed], port=8771,
                       properties={b"flags": b"138"}, server="peer.local.")
    # The legacy property deliberately returns IPv4 only.
    assert info.addresses == []
    discovery = Discovery.__new__(Discovery)
    discovery.zc = SimpleNamespace(get_service_info=lambda *args, **kwargs: info)
    discovery.interface = Interface("AWDL", 42, "fe80::123", 42)
    discovery.lock = threading.Lock()
    discovery.closed = threading.Event()
    discovery.pending, discovery.names, discovery.devices = {name}, {name}, {}
    snapshots = []
    discovery.changed = snapshots.append
    monkeypatch.setattr("opendrop.windows.sender.discover", lambda device: device)
    discovery._resolve(name)
    assert snapshots[-1][0].address == "fe80::1234"
    assert snapshots[-1][0].scope == 42
    assert snapshots[-1][0].flags == 138


def test_app_auto_accepts_by_default_and_can_require_consent(tmp_path):
    events = []
    controller = Controller({}, tmp_path, lambda kind, **data: events.append((kind, data)))
    try:
        assert controller.settings["ask_before_receiving"] is False
        assert controller.approve("Nearby phone", ["photo.jpg"]) is True
        assert events[-1][0] == "auto_accepted"
        assert not any(kind == "offer" for kind, _ in events)
        controller.command({"command": "settings", "settings": dict(
            controller.settings, ask_before_receiving=True)})
        assert controller.settings["ask_before_receiving"] is True
        controller.stop.set()
        assert controller.approve("Nearby phone", ["photo.jpg"]) is False
    finally:
        controller.close()


def test_consent_setting_changes_while_online_without_radio_restart(tmp_path):
    controller = Controller({}, tmp_path, lambda *args, **kwargs: None)
    try:
        controller.status = "online"
        controller.command({"command": "settings", "settings": dict(
            controller.settings, ask_before_receiving=True)})
        assert controller.settings["ask_before_receiving"] is True
        with pytest.raises(RuntimeError, match="Turn off"):
            controller.command({"command": "settings", "settings": dict(
                controller.settings, name="Another PC")})
        assert controller.settings["name"] == "Windows PC"
    finally:
        controller.close()


def test_organized_transfer_publishes_flat_readable_names_and_preserves_duplicates(server, tmp_path):
    receiver, events = server
    receiver.settings.organized = True
    paths = [tmp_path / "IMG_1234.PNG",
             tmp_path / "B6A8D31C-7D88-4314-9F58-F6772F95A1E8.MP4",
             tmp_path / "Meeting notes.txt"]
    for i, path in enumerate(paths):
        path.write_bytes(bytes([i]) * 8000)
    for _ in range(2):
        send(Device("test", "::1", receiver.server_port, 0, 2), paths, "PC", "id")
    destination = receiver.settings.output
    received = [p for p in destination.rglob("*") if p.is_file()]
    assert len(received) == 6
    assert {p.parent.name for p in received} == {"Pictures", "Videos", "Files"}
    assert all(len(p.relative_to(destination).parts) == 2 for p in received)
    assert len(list((destination / "Pictures").glob("Photo *.png"))) == 2
    assert len(list((destination / "Videos").glob("Video *.mp4"))) == 2
    assert (destination / "Files/Meeting notes.txt").read_bytes() == paths[2].read_bytes()
    assert (destination / "Files/Meeting notes (2).txt").read_bytes() == paths[2].read_bytes()
    assert sorted(p.read_bytes() for p in received) == sorted(
        p.read_bytes() for p in paths for _ in range(2))
    assert not list(receiver.settings.state.rglob(".partial-*"))
    assert sum(kind == "received" for kind, _ in events) == 2
