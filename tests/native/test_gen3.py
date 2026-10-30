"""Independent byte-layout and DMA-address validation for Gen3 firmware startup."""
import ctypes as c
import os
import struct
from pathlib import Path

import pytest
from test_firmware import VALID, Plan


class Region(c.Structure):
    _fields_ = [("address", c.c_uint64), ("length", c.c_uint32), ("reserved", c.c_uint32)]


class Inputs(c.Structure):
    _fields_ = [("count", c.c_uint32), ("reserved", c.c_uint32), ("regions", Region * 202)]


@pytest.fixture(scope="module")
def native():
    override = os.environ.get("IWL_FW_LIBRARY")
    root = Path(__file__).resolve().parents[2] / "build" / "native"
    candidates = [Path(override)] if override else [
        *root.rglob("iwl_fw_test.dll"), *root.rglob("libiwl_fw_test.so"),
        *root.rglob("libiwl_fw_test.dylib"),
    ]
    assert len(candidates) == 1
    dll = c.CDLL(str(candidates[0].resolve()))
    dll.iwl_fw_parse.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Plan)]
    dll.iwl_fw_parse.restype = c.c_int
    dll.iwl_gen3_build.argtypes = [c.POINTER(Plan), c.POINTER(Inputs), c.c_void_p, c.c_size_t]
    dll.iwl_gen3_build.restype = c.c_int
    dll.iwl_gen3_decode_alive.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    dll.iwl_gen3_decode_alive.restype = c.c_int
    return dll


@pytest.fixture
def setup(native):
    plan = Plan()
    assert native.iwl_fw_parse(c.create_string_buffer(VALID), len(VALID), c.byref(plan)) == 0
    inputs = Inputs()
    inputs.count = 10 + plan.section_count
    for i in range(inputs.count):
        inputs.regions[i].address = 0x1234000000 + 0x200000 * i
        inputs.regions[i].length = 0x100000
    return plan, inputs


def build(native, setup, length=9956):
    plan, inputs = setup
    output = c.create_string_buffer(b"\xA5" * 9956)
    code = native.iwl_gen3_build(c.byref(plan), c.byref(inputs), output, length)
    return code, output.raw[:9956]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def test_wire_layout_and_all_512_receive_buffers(native, setup):
    _plan, inputs = setup
    code, data = build(native, setup)
    assert code == 0
    addresses = [r.address for r in inputs.regions]
    assert data[:8] == bytes(8)
    assert u64(data, 8) == addresses[2]
    assert u64(data, 16) == addresses[5]
    assert u64(data, 24) == addresses[2] + 2048
    assert u64(data, 32) == addresses[2] + 3072
    assert u64(data, 52) == addresses[6]
    assert u64(data, 60) == addresses[4]
    assert struct.unpack_from("<HH", data, 68) == (5, 9)
    assert u64(data, 88) == addresses[1]
    assert struct.unpack_from("<I", data, 96)[0] == 1660
    scratch = data[104:1764]
    assert struct.unpack_from("<HHH", scratch) == (0x430, 0, 415)
    assert struct.unpack_from("<I", scratch, 8)[0] == 0xF0000
    assert u64(scratch, 48) == addresses[3]
    assert u64(scratch, 124) == addresses[11]  # UMAC comes before LMAC in wire format.
    assert u64(scratch, 124 + 512) == addresses[10]
    assert u64(scratch, 124 + 1024) == addresses[12]
    assert u64(scratch, 124 + 1032) == addresses[13]
    assert u64(scratch, 124 + 8) == 0
    for i in range(512):
        offset = 1764 + i * 16
        assert struct.unpack_from("<H", data, offset)[0] == i
        assert data[offset + 2:offset + 8] == bytes(6)
        assert u64(data, offset + 8) == addresses[7 + i // 256] + (i % 256) * 4096


@pytest.mark.parametrize("mutation,expected", [
    ("null_address", 3), ("unaligned", 3), ("overflow", 3),
    ("overlap", 5), ("partial_overlap", 5), ("small_context", 4),
    ("small_receive_pool", 4), ("wrong_count", 2), ("wrong_group_count", 2),
    ("bad_group", 2), ("unsupported_api", 2),
])
def test_invalid_layout_never_partially_written(native, setup, mutation, expected):
    plan, inputs = setup
    if mutation == "null_address":
        inputs.regions[4].address = 0
    elif mutation == "unaligned":
        inputs.regions[0].address += 1
    elif mutation == "overflow":
        inputs.regions[2].address = 0xFFFFFFFFFFFFF000
    elif mutation == "overlap":
        inputs.regions[3].address = inputs.regions[7].address
    elif mutation == "partial_overlap":
        inputs.regions[3].address = inputs.regions[7].address + 4096
    elif mutation == "small_context":
        inputs.regions[0].length = 103
    elif mutation == "small_receive_pool":
        inputs.regions[8].length = 4096
    elif mutation == "wrong_count":
        inputs.count -= 1
    elif mutation == "wrong_group_count":
        plan.group_count[1] += 1
    elif mutation == "bad_group":
        plan.sections[0].group = 3
    elif mutation == "unsupported_api":
        plan.version_major = 78
    code, data = build(native, setup)
    assert code == expected
    assert data == b"\xA5" * len(data)


def test_short_output_does_not_write(native, setup):
    code, data = build(native, setup, 9955)
    assert code == 1
    assert data == b"\xA5" * len(data)


@pytest.mark.parametrize("payload", [116, 128, 144])
def test_independent_alive_notifications(native, payload):
    data = bytearray(8 + payload)
    struct.pack_into("<I", data, 0, 4 + payload)
    data[4] = 1
    data[7] = 0x80
    struct.pack_into("<H", data, 8, 0xCAFE)
    struct.pack_into("<II", data, 12, 77, 0xF92B5FED)
    struct.pack_into("<II", data, 108, 77, 0xF92B5FED)
    if payload >= 128:
        struct.pack_into("<III", data, 124, 11, 22, 33)
    result = (c.c_uint32 * 11)()
    assert native.iwl_gen3_decode_alive(c.create_string_buffer(bytes(data)),
                                       len(data), result) == 0
    assert list(result)[:6] == [0xCAFE, payload, 77, 0xF92B5FED, 77, 0xF92B5FED]
    assert list(result)[6:9] == ([11, 22, 33] if payload >= 128 else [0, 0, 0])
    for size in range(len(data)):
        assert native.iwl_gen3_decode_alive(c.create_string_buffer(bytes(data)),
                                           size, result) == 2


def test_other_packet_is_not_claimed_as_alive(native):
    data = struct.pack("<I4B", 4, 0x71, 0, 0, 0x80)
    result = (c.c_uint32 * 11)()
    assert native.iwl_gen3_decode_alive(c.create_string_buffer(data), len(data), result) == 1
    assert list(result) == [0] * 11


def test_wrong_alive_size_is_rejected(native):
    data = struct.pack("<I4B", 8, 1, 0, 0, 0x80) + bytes(4)
    result = (c.c_uint32 * 11)()
    assert native.iwl_gen3_decode_alive(c.create_string_buffer(data), len(data), result) == 2


@pytest.mark.parametrize("address,words,valid", [
    (0x400000, 38, 1), (0x800000, 15, 1),
    (0x80400000, 38, 1), (0x80800000, 15, 1),
    (0x1000000 - 38 * 4, 38, 1),
    (0, 38, 0), (0x3FFFFC, 38, 0), (0x400001, 38, 0),
    (0x1000000 - 36 * 4, 38, 0), (0xFFFFFFFF, 15, 0),
    (0x400000, 0, 0), (0x400000, 39, 0),
    (0x400000, 2**32 - 1, 0), (0x10400000, 38, 0),
])
def test_firmware_error_log_reads_are_fixed_size_and_inside_sram(native, address, words, valid):
    native.iwl_gen3_error_log_range.argtypes = [c.c_uint32, c.c_uint]
    assert native.iwl_gen3_error_log_range(address, words) == valid
