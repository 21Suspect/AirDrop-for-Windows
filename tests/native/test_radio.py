"""Check fixed monitor command layouts and hostile MCC response lengths."""
import ctypes as c
import struct

import pytest
import test_metadata

native = test_metadata.native


class Command(c.Structure):
    _fields_ = [("group", c.c_uint32), ("opcode", c.c_uint32),
                ("version", c.c_uint32), ("length", c.c_uint32),
                ("data", c.c_uint8 * 160)]


class Mcc(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in
                ("status", "mcc", "capabilities", "source", "count")]
    _fields_.append(("channels", c.c_uint32 * 110))


@pytest.fixture(scope="module")
def radio(native):
    native.iwl_radio_monitor_command.argtypes = [
        c.c_uint, c.c_uint, c.c_uint, c.c_void_p, c.POINTER(Command)]
    native.iwl_radio_mcc_decode.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Mcc)]
    return native


ADDRESS = bytes.fromhex("02ad21000001")


@pytest.mark.parametrize("index,group,opcode,version,length", [
    (0, 1, 0x98, 0, 4), (1, 1, 0x9B, 0, 8), (2, 2, 1, 0, 8),
    (3, 4, 4, 0, 20), (4, 1, 0x77, 0, 4), (5, 1, 0xC8, 0, 28),
    (6, 1, 8, 0, 32), (7, 5, 8, 2, 32), (8, 1, 0x28, 0, 148),
    (9, 1, 0x2B, 0, 28), (10, 1, 0x18, 0, 48),
])
def test_published_monitor_payload_layout(radio, index, group, opcode, version, length):
    command = Command()
    assert radio.iwl_radio_monitor_command(
        index, 3, 3, c.create_string_buffer(ADDRESS), c.byref(command)) == 0
    assert (command.group, command.opcode, command.version, command.length) == (
        group, opcode, version, length)
    data = bytes(command.data)
    assert data[length:] == bytes(160 - length)
    if index == 1:
        assert struct.unpack_from("<II", data) == (1, 0x15)  # BT coexistence enabled
    elif index == 5:
        assert data[:3] == b"ZZ\x10"  # Query existing/NVM country, never override.
        assert data[3:] == bytes(157)
    elif index == 6:
        assert struct.unpack_from("<IIIIIIII", data) == (0, 1, 6, 1, 0, 0, 0, 0)
    elif index == 7:
        assert struct.unpack_from("<II", data) == (0, 0x1406)
    elif index == 8:
        assert struct.unpack_from("<IIII", data) == (0, 1, 2, 0)
        assert data[16:22] == ADDRESS
        assert data[24:30] == bytes([255]) * 6
        assert struct.unpack_from("<I", data, 52)[0] == 0x1847
    elif index == 9:
        assert struct.unpack_from("<7I", data) == (0, 1, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0)
    elif index == 10:
        assert data[2:4] == b"\xff\xff"  # Every TX TID disabled.
        assert data[16] == 2 and data[35] == 1
        assert data[40:44] == bytes(4)  # No transmit queue.


@pytest.mark.parametrize("index,tx,rx,address", [
    (11, 3, 3, ADDRESS), (0, 0, 3, ADDRESS), (0, 3, 4, ADDRESS),
    (0, 3, 3, bytes(6)), (0, 3, 3, bytes([3]) + bytes(5)),
])
def test_bad_command_inputs_return_no_partial_output(radio, index, tx, rx, address):
    command = Command()
    c.memset(c.byref(command), 0xAB, c.sizeof(command))
    assert radio.iwl_radio_monitor_command(
        index, tx, rx, c.create_string_buffer(address), c.byref(command)) != 0
    assert bytes(command) == bytes(c.sizeof(command))


def mcc_payload(count=110, status=0):
    return struct.pack("<IHHHHB3xI", status, 0x4445, 0, 0, 0, 0x10, count) + (
        struct.pack("<I", 1) * count)


@pytest.mark.parametrize("status", [0, 1])
def test_mcc_keeps_receive_only_channel_flags(radio, status):
    data = mcc_payload(status=status)
    result = Mcc()
    assert radio.iwl_radio_mcc_decode(
        c.create_string_buffer(data), len(data), c.byref(result)) == 0
    assert (result.status, result.mcc, result.source, result.count) == (status, 0x4445, 16, 110)
    assert result.channels[5] == 1  # Does not add ACTIVE/IBSS permission.


@pytest.mark.parametrize("data", [
    b"", bytes(19), mcc_payload()[:-1], mcc_payload() + b"\0",
    mcc_payload(5), mcc_payload(111), mcc_payload(status=2),
    mcc_payload(status=0xFFFFFFFF),
])
def test_mcc_bounds_and_failed_status(radio, data):
    result = Mcc()
    c.memset(c.byref(result), 0xAB, c.sizeof(result))
    assert radio.iwl_radio_mcc_decode(
        c.create_string_buffer(data), len(data), c.byref(result)) != 0
    assert bytes(result) == bytes(c.sizeof(result))


class Completion(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in
                ("status", "queue", "next_index", "rate", "byte_count", "retries", "success")]


@pytest.fixture(scope="module")
def transmit(radio):
    radio.iwl_radio_queue_command.argtypes = [c.c_uint64, c.c_uint64, c.POINTER(Command)]
    radio.iwl_radio_tx_completion.argtypes = [
        c.c_void_p, c.c_size_t, c.c_uint, c.c_uint, c.POINTER(Completion)]
    return radio


def test_queue_layout_uses_full_windows_dma_addresses(transmit):
    result = Command()
    assert transmit.iwl_radio_queue_command(
        0x123456780000, 0x223456780000, c.byref(result)) == 0
    assert (result.group, result.opcode, result.version, result.length) == (5, 0x17, 0, 36)
    assert struct.unpack_from("<II4sIIQQ", bytes(result.data)) == (
        0, 4, bytes([15, 0, 0, 0]), 0, 5, 0x223456780000, 0x123456780000)


@pytest.mark.parametrize("descriptors,counts", [
    (0, 0x200000), (0x100000, 0), (0x100001, 0x200000),
    (0x100000, 0x200001), (0x100000, 0x100000), (0x100000, 0x10F000),
    (0x100000, 0x0FF000 + 4096), (0xFFFFFFFFFFFFF000, 0x200000),
    (0x100000, 0xFFFFFFFFFFFFF000),
])
def test_queue_rejects_overlap_alignment_and_address_overflow(transmit, descriptors, counts):
    result = Command()
    c.memset(c.byref(result), 0xAA, c.sizeof(result))
    assert transmit.iwl_radio_queue_command(descriptors, counts, c.byref(result)) != 0
    assert bytes(result) == bytes(c.sizeof(result))


def completion_payload(status=1, index=0, queue=1):
    payload = bytearray(48)
    payload[0] = 1
    payload[3] = 2
    struct.pack_into("<I", payload, 4, 0x4100)
    struct.pack_into("<H", payload, 30, 299)
    struct.pack_into("<H", payload, 36, queue)
    struct.pack_into("<H", payload, 40, status)
    struct.pack_into("<I", payload, 44, (index + 1) & 65535)
    return bytes(payload)


@pytest.mark.parametrize("status,success", [(1, 1), (2, 1), (0x82, 0), (0x90, 0)])
@pytest.mark.parametrize("index", [0, 255, 256, 4095, 65535])
def test_tx_completion_validates_queue_and_wrap_and_preserves_failure(transmit, status, success, index):
    data = completion_payload(status, index)
    result = Completion()
    assert transmit.iwl_radio_tx_completion(
        c.create_string_buffer(data), len(data), 1, index, c.byref(result)) == 0
    assert (result.status, result.success, result.queue, result.next_index,
            result.byte_count, result.rate, result.retries) == (
        status, success, 1, (index + 1) & 65535, 299, 0x4100, 2)


@pytest.mark.parametrize("data,queue,index", [
    (completion_payload()[:-1], 1, 0), (completion_payload() + b"\\0", 1, 0),
    (bytes([2]) + completion_payload()[1:], 1, 0),
    (completion_payload(), 2, 0), (completion_payload(), 0, 0),
    (completion_payload(), 1, 1), (completion_payload(), 1, 65536),
])
def test_tx_completion_rejects_unowned_or_malformed_completions(transmit, data, queue, index):
    result = Completion()
    c.memset(c.byref(result), 0xAA, c.sizeof(result))
    assert transmit.iwl_radio_tx_completion(
        c.create_string_buffer(data), len(data), queue, index, c.byref(result)) != 0
    assert bytes(result) == bytes(c.sizeof(result))


@pytest.mark.parametrize("observed", [0, 1, 255, 257, 65536, 0x10000100])
def test_gen3_completion_does_not_confuse_storage_wrap_with_hardware_pointer(transmit, observed):
    data = bytearray(completion_payload(index=255))
    struct.pack_into("<I", data, 44, observed)
    result = Completion()
    assert transmit.iwl_radio_tx_completion(
        c.create_string_buffer(bytes(data)), len(data), 1, 255, c.byref(result)) != 0
    assert bytes(result) == bytes(c.sizeof(result))


@pytest.fixture(scope="module")
def ibss(native):
    native.iwl_radio_ibss_command.argtypes = [c.c_uint, c.c_void_p, c.POINTER(Command)]
    native.iwl_radio_station_queue.argtypes = [
        c.c_uint64, c.c_uint64, c.c_uint, c.c_uint, c.POINTER(Command)]
    return native


def ibss_command(ibss, kind):
    command = Command()
    assert ibss.iwl_radio_ibss_command(
        kind, c.create_string_buffer(ADDRESS), c.byref(command)) == 0
    assert bytes(command.data)[command.length:] == bytes(160 - command.length)
    return command, bytes(command.data)[:command.length]


def test_ht20_beacon_ie_boundaries_identity_and_capabilities(ibss):
    command, data = ibss_command(ibss, 0)
    assert (command.group, command.opcode, command.version) == (1, 0x91, 0)
    length, flags = struct.unpack_from("<HH", data)
    frame = data[32:]
    assert length == len(frame) == 126 and flags == 0  # Firmware antenna, 6 Mbps.
    assert data[4:32] == bytes(28)
    assert frame[:4] == b"\x80\0\0\0"
    assert frame[4:10] == b"\xff" * 6
    assert frame[10:16] == ADDRESS and frame[16:22] == bytes.fromhex("002500ff9473")
    assert struct.unpack_from("<HH", frame, 32) == (10000, 0x422)
    ies = {}
    at = 36
    while at < len(frame):
        kind, size = frame[at:at + 2]
        assert at + 2 + size <= len(frame) and kind not in ies
        ies[kind] = frame[at + 2:at + 2 + size]
        at += 2 + size
    assert at == len(frame)
    assert ies[0] == b"owly" and ies[3] == b"\x06" and ies[6] == b"\0\0"
    assert len(ies[45]) == 26 and len(ies[61]) == 22
    assert struct.unpack_from("<H", ies[45])[0] & 2 == 0  # No 40 MHz.
    assert ies[45][3] == 0xFF and ies[45][4:13] == bytes(9)  # One RX stream.
    assert ies[61][0:2] == b"\x06\0" and ies[61][6] == 0xFF
    assert ies[221] == bytes.fromhex("0050f202000100")


def test_ibss_context_alone_handles_passive_monitor_and_acks(ibss):
    _, mac = ibss_command(ibss, 1)
    assert struct.unpack_from("<4I", mac) == (0, 1, 4, 0)
    assert mac[16:22] == ADDRESS and mac[24:30] == bytes.fromhex("002500ff9473")
    assert struct.unpack_from("<7I", mac, 32) == (15, 0x15, 0, 0x20, 0x10, 0x1847, 3)
    assert struct.unpack_from("<I", mac, 112)[0] == 10000
    for ac in range(4):
        cwmin, cwmax, aifs, fifos, txop = struct.unpack_from("<HHBBH", mac, 60 + ac * 8)
        assert 0 < cwmin <= cwmax and aifs >= 2 and fifos == 1 << (ac + 1)
        assert txop <= 3008
    _, binding = ibss_command(ibss, 2)
    assert struct.unpack("<7I", binding) == (0, 1, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0)
    # A passive monitor does not create a second firmware MAC on an active IBSS.
    assert struct.unpack_from("<I", mac, 52)[0] & 0x1847 == 0x1847


@pytest.mark.parametrize("kind,station,mac,sta_type,address", [
    (3, 4, 0, 2, bytes.fromhex("030000000000")),
    (4, 3, 0, 1, bytes.fromhex("002500ff9473")),
])
def test_ibss_stations_have_distinct_ids_and_correct_parent(ibss, kind, station, mac, sta_type, address):
    command, data = ibss_command(ibss, kind)
    assert command.opcode == 0x18 and len(data) == 48
    assert data[2:4] == b"\xff\xff"  # No aggregation enabled.
    assert struct.unpack_from("<I", data, 4)[0] == mac
    assert data[8:14] == address and data[16] == station and data[35] == sta_type


@pytest.mark.parametrize("kind,address", [(5, ADDRESS), (99, ADDRESS), (0, b"\xff" * 6),
                                         (1, b"\0" * 6), (3, None)])
def test_ibss_builder_rejects_unknown_commands_and_nonlocal_addresses(ibss, kind, address):
    command = Command()
    c.memset(c.byref(command), 0xAA, c.sizeof(command))
    assert ibss.iwl_radio_ibss_command(
        kind, None if address is None else c.create_string_buffer(address), c.byref(command)) != 0
    assert bytes(command) == bytes(c.sizeof(command))


@pytest.mark.parametrize("station,tid", [(2, 15), (3, 15), (4, 0)])
def test_ibss_aux_queue_matches_station_and_tid(ibss, station, tid):
    command = Command()
    assert ibss.iwl_radio_station_queue(0x100000, 0x200000, station, tid, c.byref(command)) == 0
    assert struct.unpack_from("<I", bytes(command.data), 4)[0] == 1 << station
    assert command.data[8] == tid


@pytest.mark.parametrize("station,tid", [(0, 15), (1, 0), (5, 0), (3, 16), (4, 255)])
def test_ibss_aux_queue_rejects_unallocated_station_or_unsupported_tid(ibss, station, tid):
    command = Command()
    assert ibss.iwl_radio_station_queue(0x100000, 0x200000, station, tid, c.byref(command)) != 0
    assert bytes(command) == bytes(c.sizeof(command))
