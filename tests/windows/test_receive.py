import base64
import gzip
import http.client
import io
import os
import plistlib
import socket
import ssl
import struct
import time
import uuid
import zlib
from email.message import Message
from pathlib import Path

import pytest

from opendrop.windows.archive import InvalidTransfer, receive_archive, safe_name
from opendrop.windows.httpbody import BodyReader
from opendrop.windows.receiver import Receiver, Settings

PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jFEsAAAAASUVORK5CYII="
)


def record(name, data=b"", mode=0o100644, links=1):
    raw = name.encode("utf-8") + b"\0"
    # ODC CPIO format specification; no production encoder is used by tests.
    fields = (0, 1, mode, 0, 0, links, 0)
    return (
        b"070707"
        + "".join(f"{n:06o}" for n in fields).encode()
        + f"{0:011o}{len(raw):06o}{len(data):011o}".encode()
        + raw
        + data
    )


def archive(entries):
    return b"".join(record(name, data) for name, data in entries) + record(
        "TRAILER!!!", mode=0
    )


def dvzip(cpio, compressed=True, block_size=131072):
    out = bytearray()
    for start in range(0, len(cpio), block_size):
        block = cpio[start : start + block_size]
        data = zlib.compress(block) if compressed else block
        out.extend(struct.pack(">I", len(data) | (0 if compressed else 0x80000000)))
        out.extend(data)
    return bytes(out)


def headers(**values):
    result = Message()
    for key, value in values.items():
        result[key.replace("_", "-")] = str(value)
    return result


def connect(server):
    return http.client.HTTPSConnection(
        "::1", server.server_port, timeout=4, context=ssl._create_unverified_context()
    )


def ask(connection, entries, identifier=None, chunked=False):
    request = {
        "SenderComputerName": "Test iPhone (simulated)",
        "Files": [
            {"FileName": name, "FileBomPath": "./" + name, "FileIsDirectory": False}
            for name, _ in entries
        ],
    }
    if identifier:
        request["TransferID"] = {"id": identifier}
    body = plistlib.dumps(request, fmt=plistlib.FMT_BINARY)
    connection.request(
        "POST",
        "/Ask",
        body=iter([body[:19], body[19:]]) if chunked else body,
        encode_chunked=chunked,
    )
    response = connection.getresponse()
    response.read()
    return response.status


@pytest.fixture
def receiver(tmp_path):
    instance = Receiver(
        ("::1", 0),
        Settings(
            tmp_path / "received",
            tmp_path / "state",
            timeout=2,
            byte_limit=4 * 1024 * 1024,
        ),
        lambda sender, files: True,
    )
    instance.start()
    yield instance
    instance.close()


@pytest.mark.parametrize("compressed", [False, True])
def test_tls_dvzip_multiphoto_and_chunked_ask(receiver, compressed):
    entries = [("IMG_0001.PNG", PNG), ("IMG_0002.PNG", PNG)]
    identifier = str(uuid.uuid4())
    conn = connect(receiver)
    try:
        assert ask(conn, entries, identifier, chunked=True) == 200
        data = dvzip(archive(entries), compressed, block_size=137)
        conn.request(
            "POST",
            "/Upload",
            body=iter([data[:3], data[3:41], data[41:]]),
            headers={"Content-Type": "application/x-dvzip", "TransferID": identifier},
            encode_chunked=True,
        )
        response = conn.getresponse()
        assert response.status == 200
        response.read()
    finally:
        conn.close()
    destination, files = receiver.completed[0]
    assert files == [e[0] for e in entries]
    assert [(destination / name).read_bytes() for name, _ in entries] == [PNG, PNG]
    if os.name == "nt":
        assert (
            "ZoneId=3"
            in Path(str(destination / files[0]) + ":Zone.Identifier").read_text()
        )


def test_ios_withheld_zero_chunk_and_single_continue(receiver):
    entries = [("photo.png", PNG)]
    identifier = str(uuid.uuid4())
    connection = connect(receiver)
    assert ask(connection, entries, identifier) == 200
    connection.close()
    raw = socket.create_connection(("::1", receiver.server_port), timeout=3)
    secure = ssl._create_unverified_context().wrap_socket(
        raw, server_hostname="localhost"
    )
    try:
        data = dvzip(archive(entries))
        secure.sendall(
            (
                "POST /Upload HTTP/1.1\r\nContent-Type: application/x-dvzip\r\n"
                "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n"
                f"TransferID: {identifier}\r\n\r\n"
            ).encode()
        )
        stream = secure.makefile("rb")
        assert stream.readline() == b"HTTP/1.1 100 Continue\r\n"
        assert stream.readline() == b"\r\n"
        # Deliberately never send the terminal HTTP chunk, as current iOS can do.
        secure.sendall(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        final = http.client.HTTPResponse(secure)
        final.begin()
        assert final.status == 200
        final.read()
        stream.close()
    finally:
        secure.close()
    assert len(receiver.completed) == 1


@pytest.mark.parametrize("compress", [False, True])
def test_legacy_cpio_content_length(receiver, compress):
    entries = [("photo.png", PNG)]
    conn = connect(receiver)
    try:
        assert ask(conn, entries) == 200
        data = archive(entries)
        if compress:
            data = gzip.compress(data)
        conn.request(
            "POST", "/Upload", body=data, headers={"Content-Type": "application/x-cpio"}
        )
        reply = conn.getresponse()
        assert reply.status == 200
        reply.read()
    finally:
        conn.close()


def test_upload_without_consent(receiver):
    conn = connect(receiver)
    try:
        conn.request(
            "POST",
            "/Upload",
            body=dvzip(archive([("p.png", PNG)])),
            headers={"Content-Type": "application/x-dvzip"},
        )
        reply = conn.getresponse()
        assert reply.status == 400
        reply.read()
    finally:
        conn.close()
    assert not receiver.settings.output.exists()


def test_decline_and_wrong_transfer_id(receiver):
    receiver.approve = lambda sender, files: False
    conn = connect(receiver)
    assert ask(conn, [("p.png", PNG)]) == 403
    conn.close()
    receiver.approve = lambda sender, files: True
    conn = connect(receiver)
    assert ask(conn, [("p.png", PNG)], str(uuid.uuid4())) == 200
    conn.request(
        "POST",
        "/Upload",
        body=dvzip(archive([("p.png", PNG)])),
        headers={
            "Content-Type": "application/x-dvzip",
            "TransferID": str(uuid.uuid4()),
        },
    )
    reply = conn.getresponse()
    assert reply.status == 400
    reply.read()
    conn.close()
    assert not receiver.completed


def test_expired_approval(receiver):
    receiver.settings.approval_seconds = -1
    conn = connect(receiver)
    assert ask(conn, [("p.png", PNG)]) == 200
    conn.request(
        "POST",
        "/Upload",
        body=dvzip(archive([("p.png", PNG)])),
        headers={"Content-Type": "application/x-dvzip"},
    )
    reply = conn.getresponse()
    assert reply.status == 400
    reply.read()
    conn.close()
    assert not receiver.completed


def test_new_connection_works_while_another_stalls_tls(receiver):
    stalled = socket.create_connection(("::1", receiver.server_port), timeout=2)
    conn = connect(receiver)
    start = time.monotonic()
    try:
        conn.request("POST", "/Discover", body=plistlib.dumps({}))
        reply = conn.getresponse()
        assert reply.status == 200
        assert plistlib.loads(reply.read())["ReceiverComputerName"] == "Windows PC"
        assert time.monotonic() - start < 1.5
    finally:
        conn.close()
        stalled.close()


@pytest.mark.parametrize(
    "name",
    [
        "../escape",
        "/absolute",
        "C:/drive",
        r"foo\bar",
        "file:stream",
        "CON.png",
        "aux",
        "LPT1.txt",
        "COM\u00b9.png",
        "folder/../escape",
        "trailing.",
        "trailing ",
        "a//b",
        "nul.txt",
        "bad\nname",
        "./",
        "a/*",
    ],
)
def test_windows_path_rejection(name):
    with pytest.raises(InvalidTransfer):
        safe_name(name)


@pytest.mark.parametrize(
    "payload",
    [
        record("../escape.png", PNG) + record("TRAILER!!!", mode=0),
        record("p.png", b"target", mode=0o120777) + record("TRAILER!!!", mode=0),
        record("p.png", PNG, links=2) + record("TRAILER!!!", mode=0),
        archive([("p.png", PNG), ("P.PNG", PNG)]),
        archive([("surprise.png", PNG)]),
        archive([("p.png", PNG)])[:-10],
        record("p.png", b"TRAILER!!! inside file content"),
    ],
)
def test_archive_failures_leave_no_files(tmp_path, payload):
    with pytest.raises(InvalidTransfer):
        receive_archive(
            io.BytesIO(dvzip(payload)),
            "application/x-dvzip",
            tmp_path,
            ["p.png"],
            1024 * 1024,
        )
    assert list(tmp_path.iterdir()) == []


def test_decompression_bomb_and_byte_quota(tmp_path):
    payload = zlib.compress(b"x" * (2 * 1024 * 1024))
    bomb = struct.pack(">I", len(payload)) + payload
    with pytest.raises(InvalidTransfer):
        receive_archive(
            io.BytesIO(bomb), "application/x-dvzip", tmp_path, ["p.png"], 10
        )
    with pytest.raises(InvalidTransfer):
        receive_archive(
            io.BytesIO(dvzip(archive([("p.png", PNG)]))),
            "application/x-dvzip",
            tmp_path,
            ["p.png"],
            1,
        )
    assert list(tmp_path.iterdir()) == []


def test_do_not_overwrite_and_reject_corrupt_gzip(tmp_path):
    data = archive([("p.png", PNG)])
    destinations = []
    for _ in range(2):
        dest, _ = receive_archive(
            io.BytesIO(gzip.compress(data)),
            "application/x-cpio",
            tmp_path,
            ["p.png"],
            1000,
        )
        destinations.append(dest)
    assert destinations[0] != destinations[1]
    assert all((p / "p.png").read_bytes() == PNG for p in destinations)
    truncated = gzip.compress(data)[:-5]
    with pytest.raises((EOFError, InvalidTransfer)):
        receive_archive(
            io.BytesIO(truncated), "application/x-cpio", tmp_path, ["p.png"], 1000
        )
    assert len(list(tmp_path.iterdir())) == 2


@pytest.mark.parametrize(
    "raw",
    [
        b"g\r\n",
        b"1\nx\r\n0\r\n\r\n",
        b"2\r\nx",
        b"1\r\nxXX0\r\n\r\n",
        b"80000000\r\n",
        b"0\r\nincomplete",
    ],
)
def test_reject_malformed_chunks(raw):
    with pytest.raises(InvalidTransfer):
        BodyReader(io.BytesIO(raw), headers(Transfer_Encoding="chunked"), 100).all()


def test_reject_ambiguous_http_framing():
    for h in [
        headers(Content_Length=3, Transfer_Encoding="chunked"),
        headers(Content_Length="-1"),
        headers(Transfer_Encoding="gzip, chunked"),
        Message(),
    ]:
        with pytest.raises(InvalidTransfer):
            BodyReader(io.BytesIO(), h, 100)
    h = headers(Content_Length=3)
    h["Content-Length"] = "3"
    with pytest.raises(InvalidTransfer):
        BodyReader(io.BytesIO(), h, 100)


def test_bounded_chunk_extensions_and_trailers():
    reader = BodyReader(
        io.BytesIO(b"3;foo=bar\r\nabc\r\n0\r\nX-Test: yes\r\n\r\n"),
        headers(Transfer_Encoding="chunked"),
        3,
    )
    assert reader.all() == b"abc"


def test_incomplete_network_upload_has_no_committed_files(receiver):
    entries = [("p.png", PNG)]
    conn = connect(receiver)
    assert ask(conn, entries) == 200
    truncated = dvzip(archive(entries))[:-8]
    conn.request(
        "POST",
        "/Upload",
        body=truncated,
        headers={"Content-Type": "application/x-dvzip"},
    )
    reply = conn.getresponse()
    assert reply.status == 400
    reply.read()
    conn.close()
    assert not receiver.completed
    assert list(receiver.settings.output.iterdir()) == []


def test_independent_libarchive_fixture(tmp_path):
    # Produced with Windows bsdtar 3.8.8 --format=odc, not our test encoder.
    cpio = (
        Path(__file__)
        .with_name("fixtures")
        .joinpath("libarchive-photo.cpio")
        .read_bytes()
    )
    destination, files = receive_archive(
        io.BytesIO(cpio), "application/x-cpio", tmp_path, ["photo.png"], 10000
    )
    assert files == ["photo.png"]
    assert (destination / "photo.png").read_bytes() == PNG


def test_windows_friendly_alias_and_ipv6_scope(monkeypatch):
    import ifaddr

    from opendrop.windows.network import find_interface

    ip = ifaddr.IP(("fe80::1234", 0, 77), 64, "awdl0")
    adapter = ifaddr.Adapter(
        "{SYNTHETIC-GUID}", "Wintun Userspace Tunnel", [ip], index=12
    )
    monkeypatch.setattr(ifaddr, "get_adapters", lambda: [adapter])
    result = find_interface("awdl0")
    assert (result.index, result.scope, result.address) == (77, 77, "fe80::1234")
    with pytest.raises(RuntimeError, match="No IPv6 interface"):
        find_interface("nonexistent")


def test_transfer_approval_is_single_use(receiver):
    entries = [("photo.png", PNG)]
    identifier = str(uuid.uuid4())
    conn = connect(receiver)
    assert ask(conn, entries, identifier) == 200
    for expected_status in (200, 400):
        conn.request(
            "POST",
            "/Upload",
            body=dvzip(archive(entries)),
            headers={"Content-Type": "application/x-dvzip", "TransferID": identifier},
        )
        reply = conn.getresponse()
        assert reply.status == expected_status
        reply.read()
        conn.close()
    assert len(receiver.completed) == 1


@pytest.mark.parametrize("media", ["application/x-cpio", "application/x-dvzip"])
def test_mac_finder_sidecars_and_unicode_normalization_over_tls(receiver, media):
    receiver.settings.organized = True
    offered = "Broschu\u0308re Semper1.pdf"
    canonical = "Broschüre Semper1.pdf"
    payload = b"%PDF-1.7\nTest document bytes\n%%EOF"
    conn = connect(receiver)
    try:
        assert ask(conn, [(offered, payload)]) == 200
        cpio = archive([("._" + offered, b"Finder metadata"),
                        (canonical, payload)])
        conn.request("POST", "/Upload",
                     body=dvzip(cpio) if media.endswith("dvzip") else gzip.compress(cpio),
                     headers={"Content-Type": media})
        reply = conn.getresponse()
        assert reply.status == 200
        reply.read()
    finally:
        conn.close()
    destination, names = receiver.completed[-1]
    assert names == ["Files/" + canonical]
    assert (destination / names[0]).read_bytes() == payload
    assert [p.relative_to(destination).as_posix() for p in destination.rglob("*")
            if p.is_file()] == names
    assert not list(receiver.settings.state.rglob(".partial-*"))


@pytest.mark.parametrize("entries", [
    [("._surprise.pdf", b"metadata"), ("p.png", PNG)],
    [("._p.png", b"x"), ("._p.png", b"x"), ("p.png", PNG)],
    [("._p.png", b"x" * (1024 * 1024 + 1)), ("p.png", PNG)],
    [("._p.png", b"metadata")],  # Metadata cannot substitute for the actual file.
])
def test_sidecars_remain_bounded_and_tied_to_an_actual_accepted_file(tmp_path, entries):
    with pytest.raises(InvalidTransfer):
        receive_archive(io.BytesIO(gzip.compress(archive(entries))),
                        "application/x-cpio", tmp_path, ["p.png"], 4 * 1024 * 1024)
    assert not list(tmp_path.iterdir())


def test_canonical_unicode_collisions_rejected(tmp_path):
    with pytest.raises(InvalidTransfer, match="Duplicate"):
        receive_archive(io.BytesIO(archive([("é.txt", b"a"), ("e\u0301.txt", b"b")])),
                        "application/x-cpio", tmp_path, ["é.txt"], 1000)
    assert not list(tmp_path.iterdir())
