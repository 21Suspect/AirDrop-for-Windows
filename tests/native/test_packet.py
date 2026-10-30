"""Device-controlled MPDU boundaries and API-77 raw-transmit wire layouts."""
import ctypes as c
import os
import random
import struct
from pathlib import Path

import pytest

OK, MALFORMED, FILTERED, UNSUPPORTED, CAPACITY = range(5)


class Rx(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in ("length", "header", "channel", "rate")]
    _fields_ += [("rssi", c.c_int32)]
    _fields_ += [(n, c.c_uint32) for n in ("phy", "time", "status", "deaggregated")]


class Request(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in
                ("queue", "index", "rate", "antenna", "no_ack", "command", "notification")]


class Tx(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in ("length", "frame", "header", "count", "rate")]


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
    dll.iwl_rx_frame_decode.argtypes = [
        c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t, c.POINTER(Rx)]
    dll.iwl_tx_frame_build.argtypes = [
        c.POINTER(Request), c.c_void_p, c.c_size_t, c.c_uint64, c.c_void_p,
        c.c_size_t, c.c_void_p, c.c_size_t, c.POINTER(Tx)]
    return dll


def frame(qos=False, multicast=False):
    fc = 0x88 if qos else 0xD0
    dest = b"\x33\x33\x00\x00\x00\xfb" if multicast else b"\x02\x11\x22\x33\x44\x55"
    raw = struct.pack("<HH", fc, 0) + dest + b"\x02\xaa\xbb\xcc\xdd\xee"
    raw += bytes.fromhex("00 25 00 ff 94 73") + struct.pack("<H", 0x7AB0)
    return raw + (b"\x05\x00" if qos else b"") + bytes(range(64))


def mpdu(raw, padded=False, fcs=False, deaggregated=False):
    header = 26 if raw[0] == 0x88 else 24
    body = raw[:header] + (b"\xfe\xed" if padded else b"") + raw[header:]
    body += b"\xde\xad\xbe\xef" if fcs else b""
    desc = bytearray(64)
    struct.pack_into("<H", desc, 0, len(body))
    desc[2] = 0x20 if fcs else 0
    desc[3] = (0x20 if padded else 0) | (0x40 if deaggregated else 0)
    struct.pack_into("<H", desc, 5, 0x20)
    struct.pack_into("<II", desc, 12, 3, 0x7F000000)
    struct.pack_into("<I", desc, 36, 0x4102)
    desc[40:43] = bytes([52, 47, 6])
    struct.pack_into("<I", desc, 44, 0x1234ABCD)
    return bytes(desc) + body


def decode(native, data, capacity=4024):
    target = c.create_string_buffer(b"\xA5" * 4024 + b"GUARD")
    info = Rx()
    c.memset(c.byref(info), 0xCC, c.sizeof(info))
    code = native.iwl_rx_frame_decode(
        c.create_string_buffer(data), len(data), target, capacity, c.byref(info))
    assert target.raw[4024:4029] == b"GUARD"
    if code != OK:
        assert bytes(info) == bytes(c.sizeof(info))
        assert target.raw[:4024] == b"\xA5" * 4024
    else:
        assert target.raw[info.length:4024] == b"\xA5" * (4024 - info.length)
    return code, target.raw[:info.length], info


@pytest.mark.parametrize("qos,padded,fcs", [
    (False, False, False), (False, False, True),
    (True, True, False), (True, True, True), (True, False, False),
])
def test_received_frame_and_metadata_survive_padding_and_fcs_removal(native, qos, padded, fcs):
    raw = frame(qos)
    code, output, info = decode(native, mpdu(raw, padded, fcs))
    assert code == OK and output == raw
    assert (info.header, info.channel, info.rate, info.rssi) == (26 if qos else 24, 6, 0x4102, -47)
    assert (info.phy, info.time, info.status) == (0x20, 0x1234ABCD, 3)


def test_deaggregation_clears_only_amsdu_bit_without_reversing_ax211_addresses(native):
    raw = bytearray(frame(qos=True))
    raw[24] |= 0x80
    code, output, info = decode(native, mpdu(raw, padded=True, deaggregated=True))
    raw[24] &= ~0x80
    assert code == OK and output == raw and info.deaggregated == 1
    # Firmware may also leave a complete, unmodified aggregate: preserve its bit.
    raw[24] |= 0x80
    code, output, info = decode(native, mpdu(raw))
    assert code == OK and output == raw and info.deaggregated == 0


def test_every_truncated_packet_is_rejected_without_partial_output(native):
    packet = mpdu(frame(qos=True), padded=True, fcs=True)
    for end in range(len(packet)):
        assert decode(native, packet[:end])[0] != OK
    assert decode(native, packet, capacity=len(frame(qos=True)) - 1)[0] == CAPACITY


@pytest.mark.parametrize("status", [0, 1, 2, 0x103, 0x203, 0x703, 0x20000003])
def test_damaged_encrypted_and_rf_killed_frames_are_filtered(native, status):
    packet = bytearray(mpdu(frame()))
    struct.pack_into("<I", packet, 12, status)
    assert decode(native, bytes(packet))[0] == FILTERED


@pytest.mark.parametrize("offset,value,code", [
    (0, 0xFF, MALFORMED), (2, 0x10, UNSUPPORTED), (3, 0x40, MALFORMED),
    (64, 0xD1, MALFORMED), (64, 0xD4, FILTERED), (65, 0x40, FILTERED),
    (65, 0x04, FILTERED), (65, 0x03, FILTERED), (65, 0x80, FILTERED),
    (86, 1, FILTERED),
])
def test_unsupported_headers_and_inconsistent_descriptor_fields(native, offset, value, code):
    packet = bytearray(mpdu(frame()))
    packet[offset] = value
    assert decode(native, bytes(packet))[0] == code


@pytest.mark.parametrize("energies,expected", [((0, 0), -256), ((0, 61), -61), ((45, 0), -45)])
def test_absent_antenna_energy_is_not_a_zero_dbm_signal(native, energies, expected):
    packet = bytearray(mpdu(frame()))
    packet[40:42] = bytes(energies)
    assert decode(native, bytes(packet))[2].rssi == expected


def build(native, raw=None, address=0x123456000, slot_capacity=4096, tfd_capacity=256, **fields):
    request = Request(3, 255, 24, 1, 0, 10, 7)
    for name, value in fields.items():
        setattr(request, name, value)
    raw = frame() if raw is None else raw
    slot = c.create_string_buffer(b"\xA5" * 4096 + b"GUARD")
    tfd = c.create_string_buffer(b"\x5A" * 256 + b"GUARD")
    info = Tx()
    c.memset(c.byref(info), 0xCC, c.sizeof(info))
    code = native.iwl_tx_frame_build(
        c.byref(request), c.create_string_buffer(bytes(raw)), len(raw), address,
        slot, slot_capacity, tfd, tfd_capacity, c.byref(info))
    assert slot.raw[4096:4101] == tfd.raw[256:261] == b"GUARD"
    if code != OK:
        assert bytes(info) == bytes(c.sizeof(info))
        assert slot.raw[:4096] == b"\xA5" * 4096
        assert tfd.raw[:256] == b"\x5A" * 256
    return code, slot.raw[:4096], tfd.raw[:256], info


@pytest.mark.parametrize("qos", [False, True])
def test_tx_header_rate_dma_segments_and_byte_count_match_gen3_abi(native, qos):
    raw = frame(qos)
    code, slot, tfd, info = build(native, raw)
    assert code == OK
    assert slot[:4] == bytes.fromhex("1c 00 ff 03")  # short header, queue 3
    assert struct.unpack_from("<HHI", slot, 4) == (
        len(raw), 3, 0x2D00 if qos else 0x0C00)
    assert slot[12:20] == bytes(8) and slot[24:32] == bytes(8)
    assert struct.unpack_from("<I", slot, 20)[0] == 0x4102  # 12 Mbit/s, antenna A
    assert info.rate == 0x4102 and info.count == len(raw) and info.frame == len(raw)
    header = 26 if qos else 24
    payload_offset = 60 if qos else 56
    assert slot[32:32 + header] == raw[:header]
    assert slot[32 + header:payload_offset] == bytes(payload_offset - 32 - header)
    assert slot[payload_offset:info.length] == raw[header:]
    assert slot[info.length:] == bytes(4096 - info.length)
    assert struct.unpack_from("<H", tfd)[0] == 3
    assert [struct.unpack_from("<HQ", tfd, off) for off in (2, 12, 22)] == [
        (20, 0x123456000), (payload_offset - 20, 0x123456014),
        (len(raw) - header, 0x123456000 + payload_offset)]
    assert tfd[32:] == bytes(224)


def test_qos_noack_is_explicit_and_does_not_destroy_tid_or_amsdu(native):
    raw = bytearray(frame(qos=True))
    raw[24] = 0xC5
    code, slot, _, _ = build(native, raw, no_ack=1)
    assert code == OK and slot[56] == 0xA5
    assert struct.unpack_from("<I", slot, 8)[0] == 0x6D00  # MH + pad + A-MSDU
    assert build(native, frame(), no_ack=1)[0] == UNSUPPORTED
    assert build(native, frame(multicast=True), no_ack=1)[0] == OK


@pytest.mark.parametrize("rate,index", [(12, 0), (18, 1), (24, 2), (36, 3), (48, 4),
                                       (72, 5), (96, 6), (108, 7)])
def test_explicit_ofdm_rates_use_new_firmware_indices(native, rate, index):
    code, _, _, info = build(native, rate=rate, antenna=2)
    assert code == OK and info.rate == 0x8100 + index


@pytest.mark.parametrize("fields,code", [
    ({"queue": 0}, MALFORMED), ({"queue": 32}, MALFORMED), ({"index": 256}, MALFORMED),
    ({"antenna": 0}, MALFORMED), ({"antenna": 3}, MALFORMED), ({"no_ack": 2}, MALFORMED),
    ({"command": 8}, UNSUPPORTED), ({"notification": 6}, UNSUPPORTED),
    ({"rate": 0}, UNSUPPORTED), ({"rate": 11}, UNSUPPORTED),
    ({"address": 0}, MALFORMED), ({"address": 0x123400001}, MALFORMED),
    ({"address": 0xFFFFFFFFFFFFF000}, MALFORMED),
    ({"slot_capacity": 4095}, CAPACITY), ({"tfd_capacity": 255}, CAPACITY),
])
def test_tx_rejects_unsupported_dma_versions_rates_and_short_buffers(native, fields, code):
    assert build(native, **fields)[0] == code


def test_transmit_length_boundaries_and_no_payload(native):
    raw = frame(qos=True)
    for length in range(26):
        assert build(native, raw[:length])[0] != OK
    code, _, tfd, info = build(native, raw[:26])
    assert code == OK and info.frame == 26 and struct.unpack_from("<H", tfd)[0] == 2
    assert build(native, raw + bytes(2304 - len(raw)))[0] == OK
    assert build(native, raw + bytes(2305 - len(raw)))[0] == MALFORMED


def test_random_device_descriptors_are_bounded_and_preserve_canaries(native):
    rng = random.Random(0xA211)
    for _ in range(2000):
        raw = frame(qos=bool(rng.getrandbits(1)))
        data = bytearray(mpdu(raw, padded=True, fcs=True))
        for _ in range(rng.randrange(1, 9)):
            data[rng.randrange(len(data))] = rng.randrange(256)
        # Include plausible lengths, not only obviously corrupt descriptors.
        if rng.getrandbits(1):
            struct.pack_into("<H", data, 0, len(data) - 64)
        code, output, info = decode(native, bytes(data), rng.randrange(4025))
        if code == OK:
            assert 24 <= len(output) <= len(data) - 64
            assert info.header <= info.length


def test_plain_data_noack_conversion_preserves_addresses_sequence_and_payload(native):
    raw = bytearray(frame())
    raw[0] = 0x08
    code, slot, tfd, info = build(native, raw, no_ack=1)
    assert code == OK and slot[32] == 0x88 and slot[33:56] == raw[1:24]
    assert slot[56:58] == b"\x20\x00" and slot[58:60] == bytes(2)
    assert slot[60:info.length] == raw[24:]
    assert info.header == 26 and info.frame == len(raw) + 2 and info.count == len(raw) + 2
    assert struct.unpack_from("<H", slot, 4)[0] == len(raw) + 2
    assert struct.unpack_from("<HQ", tfd, 22) == (len(raw) - 24, 0x12345603C)
    assert build(native, raw + bytes(2302 - len(raw)), no_ack=1)[0] == OK
    assert build(native, raw + bytes(2303 - len(raw)), no_ack=1)[0] == CAPACITY


LOCAL = bytes.fromhex("02ad21000001")
PEER = bytes.fromhex("02ad21000002")
AWDL_BSSID = bytes.fromhex("002500ff9473")


def awdl_frame(*, transmit=True, qos=False, action=False):
    kind = 0xD0 if action else (0x88 if qos else 8)
    dest = b"\xff" * 6 if action else (PEER if transmit else LOCAL)
    source = LOCAL if transmit else PEER
    body = bytes.fromhex("7f0017f20810") if action else b"\xaa\xaa\x03\0\0\0\x86\xdd"
    return bytearray(bytes([kind, 0, 0, 0]) + dest + source + AWDL_BSSID +
                     b"\0\0" + (b"\0\0" if qos else b"") + body)


def validate_awdl(native, value, transmit):
    native.iwl_awdl_frame_validate.argtypes = [
        c.c_void_p, c.c_size_t, c.c_void_p, c.c_uint]
    return native.iwl_awdl_frame_validate(
        c.create_string_buffer(bytes(value)), len(value),
        c.create_string_buffer(LOCAL), transmit)


@pytest.mark.parametrize("transmit", [False, True])
@pytest.mark.parametrize("qos,action", [(False, False), (True, False), (False, True)])
def test_awdl_session_accepts_matching_address_and_frame_types(native, transmit, qos, action):
    value = awdl_frame(transmit=transmit, qos=qos, action=action)
    assert validate_awdl(native, value, transmit) == OK


@pytest.mark.parametrize("offset", [10, 16, 17, 18, 19, 20, 21, 24, 25, 26, 27, 28, 29])
def test_awdl_session_rejects_foreign_source_bssid_or_action_vendor(native, offset):
    value = awdl_frame(action=True)
    value[offset] ^= 2
    assert validate_awdl(native, value, True) != OK


@pytest.mark.parametrize("end", range(30))
def test_awdl_action_prefix_truncation_is_rejected(native, end):
    assert validate_awdl(native, awdl_frame(action=True)[:end], True) != OK


@pytest.mark.parametrize("kind,flags", [(0xC0, 0), (0xA0, 0), (0xB4, 0),
                                       (8, 1), (8, 2), (8, 0x40)])
def test_awdl_session_rejects_disconnect_control_ds_and_protected_traffic(native, kind, flags):
    value = awdl_frame()
    value[0:2] = bytes([kind, flags])
    assert validate_awdl(native, value, True) != OK


def test_awdl_session_filters_other_peers_unicast_and_own_echo(native):
    value = awdl_frame(transmit=False)
    value[4] ^= 2
    assert validate_awdl(native, value, False) == FILTERED
    value[4:10] = b"\xff" * 6
    assert validate_awdl(native, value, False) == OK
    value[10:16] = LOCAL
    assert validate_awdl(native, value, False) == FILTERED
    assert validate_awdl(native, awdl_frame() + bytes(2304), True) == MALFORMED
