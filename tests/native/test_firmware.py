"""Exercise the actual portable C parser used by the Intel driver."""
import ctypes
import os
import random
import struct
from pathlib import Path

import pytest


class Section(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ("file_offset", "length", "device_offset", "group")]


class Plan(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in (
        "version_major", "version_minor", "version_api", "phy_config",
        "cpu_count", "paging_bytes", "iml_offset", "iml_length", "section_count"
    )] + [
        ("group_count", ctypes.c_uint32 * 3),
        ("total_section_bytes", ctypes.c_uint32),
        ("record_count", ctypes.c_uint32),
        ("sections", Section * 192),
    ]


@pytest.fixture(scope="module")
def parse():
    override = os.environ.get("IWL_FW_LIBRARY")
    root = Path(__file__).resolve().parents[2] / "build" / "native"
    candidates = [Path(override)] if override else [
        *root.rglob("iwl_fw_test.dll"), *root.rglob("libiwl_fw_test.so"),
        *root.rglob("libiwl_fw_test.dylib"),
    ]
    if len(candidates) != 1 or not candidates[0].is_file():
        pytest.fail("Build native/intel with CMake or set IWL_FW_LIBRARY to its test library")
    dll = ctypes.CDLL(str(candidates[0].resolve()))
    dll.iwl_fw_parse.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Plan))
    dll.iwl_fw_parse.restype = ctypes.c_int

    def run(data, length=None):
        buffer = ctypes.create_string_buffer(data)
        result = Plan()
        code = dll.iwl_fw_parse(buffer, len(data) if length is None else length,
                               ctypes.byref(result))
        return code, result
    return run


def tlv(kind, data):
    return struct.pack("<II", kind, len(data)) + data + b"\0" * (-len(data) % 4)


def section(offset, data):
    return tlv(19, struct.pack("<I", offset) + data)


HEADER = struct.pack("<II64sIIQ", 0, 0x0A4C5749, b"independent test image", 77, 0, 0)
META = (
    tlv(36, struct.pack("<III", 77, 0x1234ABCD, 0))
    + tlv(23, struct.pack("<I", 0x330018))
    + tlv(27, struct.pack("<I", 2))
    + tlv(32, struct.pack("<I", 4096))
    + tlv(52, b"image-loader")
)
LMAC = section(0x440000, b"lmac-code")
CPU_MARKER = section(0xFFFFCCCC, b"\0" * 4)
UMAC = section(0x80440000, b"umac-code")
PAGE_MARKER = section(0xAAAABBBB, b"\0" * 4)
PAGING = section(0, b"css-signature") + section(0x1000000, b"x" * 4096)
VALID = HEADER + META + LMAC + CPU_MARKER + UMAC + PAGE_MARKER + PAGING


def test_independent_container(parse):
    code, result = parse(VALID)
    assert code == 0
    assert (result.version_major, result.version_minor) == (77, 0x1234ABCD)
    assert list(result.group_count) == [1, 1, 2]
    assert result.paging_bytes == 4096
    assert VALID[result.iml_offset:result.iml_offset + result.iml_length] == b"image-loader"
    payloads = [VALID[s.file_offset:s.file_offset+s.length]
                for s in result.sections[:result.section_count]]
    assert payloads == [b"lmac-code", b"umac-code", b"css-signature", b"x"*4096]


def test_every_truncated_prefix(parse):
    for length in range(len(VALID)):
        assert parse(VALID, length)[0] != 0, length


@pytest.mark.parametrize("data", [
    b"", b"\0"*87, b"\1"+VALID[1:], VALID[:4]+b"bad!"+VALID[8:],
    VALID+b"x", VALID+struct.pack("<II", 100, 0xFFFFFFFF),
    HEADER+META+CPU_MARKER+LMAC+UMAC+PAGE_MARKER+PAGING,
    HEADER+META+LMAC+CPU_MARKER+CPU_MARKER+UMAC+PAGE_MARKER+PAGING,
    HEADER+META+LMAC+PAGE_MARKER+UMAC+CPU_MARKER+PAGING,
    HEADER+META+LMAC+CPU_MARKER+UMAC+PAGING,
    HEADER+META+LMAC+CPU_MARKER+UMAC+PAGE_MARKER,
    HEADER+META+section(0, b"x"*32769)+CPU_MARKER+UMAC+PAGE_MARKER+PAGING,
    HEADER+META+section(0xFFFFFFFD, b"abcdef")+CPU_MARKER+UMAC+PAGE_MARKER+PAGING,
    VALID+tlv(36, struct.pack("<III", 77, 0, 0)),
    VALID+tlv(52, b"duplicate image loader"),
    HEADER+tlv(36, struct.pack("<III", 89, 0, 0))+META[20:]+LMAC+CPU_MARKER+UMAC+PAGE_MARKER+PAGING,
])
def test_malformed_containers(parse, data):
    assert parse(data)[0] != 0


@pytest.mark.parametrize("kind,bad_body", [
    (36, b"\0"*8), (23, b""), (27, struct.pack("<I", 1)),
    (32, struct.pack("<I", 4095)), (32, struct.pack("<I", 8192)),
    (52, b""), (19, b"\0"*3),
])
def test_bad_record_sizes_and_values(parse, kind, bad_body):
    assert parse(VALID+tlv(kind, bad_body))[0] != 0


def test_group_limit(parse):
    data = HEADER+META+LMAC*65+CPU_MARKER+UMAC+PAGE_MARKER+PAGING
    assert parse(data)[0] == 3


def test_unknown_tlv_and_alignment(parse):
    code, result = parse(HEADER+tlv(0xFEED, b"123")+VALID[88:])
    assert code == 0
    assert result.record_count == 12


def test_random_mutations_are_bounded(parse):
    rng = random.Random(20261030)
    for _ in range(1000):
        data = bytearray(VALID)
        for _ in range(rng.randint(1, 5)):
            index = rng.randrange(len(data))
            data[index] = rng.randrange(256)
        code, result = parse(bytes(data))
        if code == 0:
            assert result.section_count <= 192
            assert all(count <= 64 for count in result.group_count)
            assert result.iml_offset + result.iml_length <= len(data)
            for section_info in result.sections[:result.section_count]:
                assert 0 < section_info.length <= 32768
                assert section_info.file_offset + section_info.length <= len(data)
                assert section_info.group in (0, 1, 2)


def test_real_firmware_when_supplied(parse):
    name = os.environ.get("IWL_FW_IMAGE")
    if not name:
        pytest.skip("External Intel firmware is not bundled with the source")
    data = Path(name).read_bytes()
    code, result = parse(data)
    assert code == 0
    assert result.version_major == 77
    assert result.cpu_count == 2
    assert list(result.group_count) == [15, 16, 24]
    assert result.iml_length == 13944
    assert result.paging_bytes == 749568
