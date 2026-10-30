"""Check calibration selection and real Gen3 command wire layouts."""
import ctypes as c
import hashlib
import os
import struct
from pathlib import Path

import pytest
from test_firmware import tlv


class Section(c.Structure):
    _fields_ = [("offset", c.c_uint32), ("length", c.c_uint32)]


class Pnvm(c.Structure):
    _fields_ = [("version", c.c_uint32), ("bytes", c.c_uint32), ("count", c.c_uint32),
                ("sku", c.c_uint32 * 3), ("sections", Section * 64)]


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
    dll.iwl_pnvm_parse.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(c.c_uint32),
                                  c.c_uint16, c.c_uint16, c.POINTER(Pnvm)]
    dll.iwl_command_build.argtypes = [c.c_uint8, c.c_uint8, c.c_uint8, c.c_uint8,
        c.c_uint64, c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t]
    dll.iwl_beacon_command_build.argtypes = [
        c.c_uint8, c.c_uint64, c.c_void_p, c.c_size_t,
        c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t,
    ]
    return dll


def calibration(sku=(7, 8, 9), mac=0x43, payload=b"calibration"):
    return (tlv(64, struct.pack("<III", *sku)) + tlv(58, struct.pack("<HH", mac, 0x10D))
            + tlv(62, struct.pack("<I", 0x12345678))
            + tlv(19, struct.pack("<I", 0xCAFEDEAD) + payload))


def parse(native, data, sku=(7, 8, 9)):
    plan = Pnvm()
    code = native.iwl_pnvm_parse(c.create_string_buffer(data), len(data),
                                 (c.c_uint32 * 3)(*sku), 0x43, 0x10D, c.byref(plan))
    return code, plan


def test_selects_only_matching_sku_and_radio(native):
    data = calibration(sku=(99, 8, 9)) + calibration(mac=0x37) + calibration()
    code, plan = parse(native, data)
    assert code == 0
    assert plan.version == 0x12345678
    assert plan.count == 1
    assert data[plan.sections[0].offset:plan.sections[0].offset + plan.bytes] == b"calibration"


def test_every_truncated_record_is_rejected_without_a_partial_plan(native):
    data = calibration()
    for length in range(len(data)):
        code, plan = parse(native, data[:length])
        assert code != 0
        assert bytes(plan) == bytes(c.sizeof(Pnvm))
    assert parse(native, data + b"\0")[0] == 2


def test_duplicate_matching_sections_are_ambiguous(native):
    code, plan = parse(native, calibration() * 2)
    assert code == 5
    assert plan.count == 0


@pytest.mark.parametrize("data", [
    tlv(64, bytes(8)),
    tlv(19, bytes(3)),
    tlv(64, struct.pack("<III", 7, 8, 9)) + tlv(58, b"\x43"),
    calibration() + tlv(62, bytes(4)),
])
def test_malformed_calibration_metadata_is_rejected(native, data):
    assert parse(native, data)[0] != 0


def test_unmatched_calibration_is_not_applied(native):
    assert parse(native, calibration(mac=0x37))[0] == 4


def test_authenticated_intel_pnvm_all_so_gf_variants(native):
    filename = os.environ.get("IWL_PNVM_IMAGE")
    if not filename:
        pytest.skip("External Intel PNVM is not bundled with source")
    data = Path(filename).read_bytes()
    assert hashlib.sha256(data).hexdigest() == "b37f038b3467ed568d107a9c1dfd916fa7e8cbd58b2f9bec0c2c53297ccfcab7"
    for sku, expected in [((0x610D1, 0, 0), 13668), ((0x610D1, 3, 0), 13848),
                          ((0xA10D1, 0, 0), 13592), ((0x510D1, 0, 0), 13716)]:
        code, plan = parse(native, data, sku)
        assert code == 0
        assert plan.version == 0x285B3568
        assert plan.count == 2
        assert plan.bytes == expected
        assert data[plan.sections[0].offset:plan.sections[0].offset + 4] == b"\x06\0\0\0"
        assert data[plan.sections[1].offset:plan.sections[1].offset + 8] == b"TLP**MVN"


@pytest.mark.parametrize("size", [0, 4, 12, 13, 4088])
def test_command_header_and_split_transfer_descriptors(native, size):
    payload = bytes(n & 255 for n in range(size))
    cmd, desc = c.create_string_buffer(4096), c.create_string_buffer(256)
    address = 0x1234567000
    assert native.iwl_command_build(3, 2, 0, 255, address,
        c.create_string_buffer(payload), size, cmd, 4096, desc, 256) == 0
    assert cmd.raw[:8] == struct.pack("<BBBBHBB", 3, 2, 255, 0, size, 0, 0)
    assert cmd.raw[8:8 + size] == payload
    total = size + 8
    assert struct.unpack_from("<HHQ", desc.raw) == (2 if total > 20 else 1, min(20, total), address)
    if total > 20:
        assert struct.unpack_from("<HQ", desc.raw, 12) == (total - 20, address + 20)
        assert desc.raw[22:] == bytes(234)
    else:
        assert desc.raw[12:] == bytes(244)


def test_legacy_group_is_encoded_as_long_group(native):
    cmd, desc = c.create_string_buffer(4096), c.create_string_buffer(256)
    assert native.iwl_command_build(0x98, 0, 1, 0, 0x10000, None, 0, cmd, 4096, desc, 256) == 0
    assert cmd.raw[:8] == b"\x98\x01\x00\x00\x00\x00\x00\x01"


@pytest.mark.parametrize("address,size,capacity,tfd_capacity", [
    (0, 4, 4096, 256), (0x10001, 4, 4096, 256),
    (0xFFFFFFFFFFFFF000, 4, 4096, 256), (0x10000, 4089, 4096, 256),
    (0x10000, 4, 11, 256), (0x10000, 4, 4096, 255),
])
def test_invalid_commands_do_not_write_descriptors(native, address, size, capacity, tfd_capacity):
    cmd, desc = c.create_string_buffer(b"\xA5" * 4096), c.create_string_buffer(b"\xA5" * 256)
    assert native.iwl_command_build(3, 2, 0, 0, address, c.create_string_buffer(bytes(4096)),
                                    size, cmd, capacity, desc, tfd_capacity) == 1
    assert cmd.raw[:4096] == b"\xA5" * 4096
    assert desc.raw[:256] == b"\xA5" * 256

def beacon_payload(frame_bytes=126):
    payload = bytearray(32 + frame_bytes)
    struct.pack_into("<H", payload, 0, frame_bytes)
    payload[32] = 0x80
    payload[36:42] = b"\xff" * 6
    return bytes(payload)


@pytest.mark.parametrize("frame_bytes", [36, 126, 4056])
def test_beacon_dma_fragments_reconstruct_complete_command(native, frame_bytes):
    payload = beacon_payload(frame_bytes)
    cmd, desc = c.create_string_buffer(4096), c.create_string_buffer(256)
    address = 0x1234567000
    assert native.iwl_beacon_command_build(11, address,
        c.create_string_buffer(payload), len(payload), cmd, 4096, desc, 256) == 0
    assert struct.unpack_from("<H", desc.raw)[0] == 3
    fragments = [struct.unpack_from("<HQ", desc.raw, 2 + i * 10) for i in range(3)]
    assert fragments == [(20, address), (20, address + 20), (frame_bytes, address + 40)]
    reconstructed = b"".join(
        cmd.raw[start - address:start - address + size] for size, start in fragments
    )
    assert reconstructed == struct.pack("<BBBBHBB", 0x91, 1, 11, 0, len(payload), 0, 0) + payload
    assert desc.raw[32:] == bytes(224)
    assert cmd.raw[len(reconstructed):] == bytes(4096 - len(reconstructed))


@pytest.mark.parametrize("invalid", [
    "null", "short", "oversized", "length", "subtype", "flags", "address",
    "command_capacity", "descriptor_capacity",
])
def test_invalid_beacon_does_not_write_dma(native, invalid):
    payload = bytearray(beacon_payload())
    address, capacity, desc_capacity = 0x10000, 4096, 256
    if invalid == "short":
        payload = payload[:67]
    elif invalid == "oversized":
        payload = bytearray(beacon_payload(4057))
    elif invalid == "length":
        payload[0] -= 1
    elif invalid == "subtype":
        payload[32] = 0x50
    elif invalid == "flags":
        payload[33] = 0x80
    elif invalid == "address":
        address += 1
    elif invalid == "command_capacity":
        capacity = 165
    elif invalid == "descriptor_capacity":
        desc_capacity = 255
    cmd, desc = c.create_string_buffer(b"\xa5" * 4096), c.create_string_buffer(b"\xa5" * 256)
    data = None if invalid == "null" else c.create_string_buffer(bytes(payload))
    assert native.iwl_beacon_command_build(
        0, address, data, len(payload), cmd, capacity, desc, desc_capacity,
    ) == 1
    assert cmd.raw[:4096] == b"\xa5" * 4096
    assert desc.raw[:256] == b"\xa5" * 256
