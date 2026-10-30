"""Metadata versions drive the sizes of commands accepted by Intel API 77."""
import ctypes as c
import os
import struct
from pathlib import Path

import pytest
from test_firmware import HEADER, tlv


class Metadata(c.Structure):
    _fields_ = [("count", c.c_uint32), ("api", c.c_uint32 * 16),
                ("capabilities", c.c_uint32 * 16), ("flow", c.c_uint32),
                ("event", c.c_uint32), ("calibration", c.c_uint32),
                ("commands", c.c_uint8 * 2048)]


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
    dll.iwl_metadata_parse.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Metadata)]
    dll.iwl_metadata_version.argtypes = [c.POINTER(Metadata), c.c_uint, c.c_uint, c.c_int]
    dll.iwl_metadata_bit.argtypes = [c.POINTER(Metadata), c.c_uint, c.c_int]
    return dll


def parse(native, data):
    result = Metadata()
    code = native.iwl_metadata_parse(c.create_string_buffer(data), len(data), c.byref(result))
    return code, result


BASE = HEADER + tlv(36, struct.pack("<III", 77, 0, 0))


def test_metadata_word_indices_and_command_versions(native):
    data = (BASE + tlv(29, struct.pack("<II", 1, 1 << 16))
            + tlv(30, struct.pack("<II", 1, 1))
            + tlv(48, bytes([2, 12, 1, 4, 0x1C, 1, 10, 7]))
            + tlv(22, struct.pack("<III", 0, 0x12345678, 0x9ABCDEF0)))
    code, meta = parse(native, data)
    assert code == 0
    assert meta.count == 2
    assert (meta.flow, meta.event, meta.calibration) == (0x12345678, 0x9ABCDEF0, 1)
    assert native.iwl_metadata_bit(c.byref(meta), 48, 0) == 1
    assert native.iwl_metadata_bit(c.byref(meta), 32, 1) == 1
    assert native.iwl_metadata_bit(c.byref(meta), 512, 1) == 0
    assert native.iwl_metadata_version(c.byref(meta), 12, 2, 1) == 4
    assert native.iwl_metadata_version(c.byref(meta), 1, 0x1C, 0) == 10
    assert native.iwl_metadata_version(c.byref(meta), 255, 255, 0) == 99


@pytest.mark.parametrize("tail", [
    b"\0", tlv(48, b"\0"), tlv(48, bytes(2052)),
    tlv(29, struct.pack("<II", 16, 0)),
    tlv(30, bytes(8)) * 2, tlv(22, bytes(12)) * 2,
    tlv(36, struct.pack("<III", 77, 0, 0)),
])
def test_invalid_or_duplicate_metadata_returns_no_partial_result(native, tail):
    code, meta = parse(native, BASE + tail)
    assert code != 0
    assert bytes(meta) == bytes(c.sizeof(Metadata))


def test_published_duplicate_version_entries_keep_reference_order(native):
    code, meta = parse(native, BASE + tlv(48, bytes([23, 14, 1, 0, 23, 14, 99, 1])))
    assert code == 0
    assert meta.count == 2
    assert native.iwl_metadata_version(c.byref(meta), 14, 23, 0) == 1
    assert native.iwl_metadata_version(c.byref(meta), 14, 23, 1) == 0


def test_authenticated_image_metadata(native):
    filename = os.environ.get("IWL_FW_IMAGE")
    if not filename:
        pytest.skip("External Intel firmware is not bundled")
    code, meta = parse(native, Path(filename).read_bytes())
    assert code == 0
    assert meta.count == 191
    assert meta.calibration == 1
    assert (meta.flow, meta.event) == (0x5FD601F3, 0x5B8695AB)
    assert native.iwl_metadata_bit(c.byref(meta), 32, 1) == 0  # contiguous PNVM
    assert native.iwl_metadata_bit(c.byref(meta), 48, 0) == 1  # NVM response v4
    assert native.iwl_metadata_version(c.byref(meta), 12, 2, 1) == 4
    assert native.iwl_metadata_version(c.byref(meta), 1, 0x1C, 0) == 10
