"""Validate device-controlled response lengths and NVM channel restrictions."""
import ctypes as c
import os
import struct
from pathlib import Path

import pytest


class Response(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in
                ("opcode", "group", "index", "queue", "packet_bytes", "payload_bytes")]


class Nvm(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in
                ("version", "board", "addresses", "flags", "mac", "tx", "rx", "lar", "count")]
    _fields_ += [("channels", c.c_uint32 * 110)]


@pytest.fixture(scope="module")
def native():
    override = os.environ.get("IWL_FW_LIBRARY")
    root = Path(__file__).resolve().parents[2] / "build" / "native"
    paths = [Path(override)] if override else [
        *root.rglob("iwl_fw_test.dll"), *root.rglob("libiwl_fw_test.so"),
        *root.rglob("libiwl_fw_test.dylib"),
    ]
    assert len(paths) == 1
    dll = c.CDLL(str(paths[0].resolve()))
    dll.iwl_response_parse.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Response)]
    dll.iwl_nvm_decode.argtypes = [c.c_void_p, c.c_size_t, c.c_uint, c.POINTER(Nvm)]
    return dll


def test_packet_header_excludes_length_word_and_preserves_unsolicited_bit(native):
    data = struct.pack("<I4B", 0x20000008, 0xFE, 12, 7, 0x80) + bytes(4)
    result = Response()
    assert native.iwl_response_parse(c.create_string_buffer(data), len(data), c.byref(result)) == 0
    assert [getattr(result, f[0]) for f in result._fields_] == [0xFE, 12, 7, 0x80, 12, 4]
    for end in range(len(data)):
        assert native.iwl_response_parse(c.create_string_buffer(data), end, c.byref(result)) == 2


@pytest.mark.parametrize("data,expected", [
    (bytes(8), 1), (struct.pack("<I4B", 0x55550000, 1, 0, 0, 0x80), 1),
    (struct.pack("<I4B", 3, 1, 0, 0, 0x80), 2),
    (struct.pack("<I4B", 5, 1, 0, 0, 0x80), 2),
])
def test_empty_and_malformed_rx_buffer(native, data, expected):
    result = Response()
    assert native.iwl_response_parse(c.create_string_buffer(data), len(data), c.byref(result)) == expected
    assert bytes(result) == bytes(c.sizeof(Response))


def payload(version):
    data = bytearray(468 if version == 4 else 128)
    struct.pack_into("<IHBBIIII", data, 0, 0, 0x1234, 3, 2, 0x1F, 3, 1, 1)
    if version == 4:
        struct.pack_into("<I", data, 24, 110)
        struct.pack_into("<I", data, 28 + 5 * 4, 0x10B)
    else:
        struct.pack_into("<H", data, 24 + 5 * 2, 0x10B)
    return data


@pytest.mark.parametrize("version", [3, 4])
def test_nvm_decode_preserves_channel_permissions(native, version):
    data, nvm = payload(version), Nvm()
    assert native.iwl_nvm_decode(c.create_string_buffer(bytes(data)), len(data), version, c.byref(nvm)) == 0
    assert (nvm.version, nvm.board, nvm.addresses, nvm.mac) == (0x1234, 3, 2, 0x1F)
    assert (nvm.tx, nvm.rx, nvm.lar, nvm.count) == (3, 1, 1, 110 if version == 4 else 51)
    assert nvm.channels[5] == 0x10B
    assert nvm.channels[0] == 0  # Absent permission must remain absent.
    for end in range(len(data)):
        assert native.iwl_nvm_decode(c.create_string_buffer(bytes(data)), end, version, c.byref(nvm)) == 1
        assert bytes(nvm) == bytes(c.sizeof(Nvm))


@pytest.mark.parametrize("offset,value", [(24, 111), (24, 0), (12, 0), (16, 8), (20, 2)])
def test_bad_device_capability_values_do_not_escape_bounds(native, offset, value):
    data, nvm = payload(4), Nvm()
    struct.pack_into("<I", data, offset, value)
    assert native.iwl_nvm_decode(c.create_string_buffer(bytes(data)), len(data), 4, c.byref(nvm)) == 1
    assert bytes(nvm) == bytes(c.sizeof(Nvm))
