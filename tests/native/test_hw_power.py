"""Fault-injection tests for the C controller sequence, without hardware."""
import ctypes as c
import os
from pathlib import Path

import pytest

READ = c.CFUNCTYPE(c.c_uint32, c.c_void_p, c.c_uint32)
WRITE = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_uint32)
DELAY = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32)
TIME = c.CFUNCTYPE(c.c_uint64, c.c_void_p)


class Io(c.Structure):
    _fields_ = [("context", c.c_void_p), ("read", READ), ("write", WRITE),
                ("delay", DELAY), ("time", TIME)]


class Result(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in (
        "size", "version", "start_error", "stop_error", "gp_before", "gp_active",
        "gp_after", "reset_stopped", "hw_revision", "rf_id", "register_writes", "elapsed_us"
    )]


@pytest.fixture(scope="module")
def cycle():
    override = os.environ.get("IWL_FW_LIBRARY")
    root = Path(__file__).resolve().parents[2] / "build" / "native"
    candidates = [Path(override)] if override else [
        *root.rglob("iwl_fw_test.dll"), *root.rglob("libiwl_fw_test.so"),
        *root.rglob("libiwl_fw_test.dylib"),
    ]
    assert len(candidates) == 1
    library = c.CDLL(str(candidates[0].resolve()))
    function = library.iwl_hw_power_cycle
    function.argtypes = [c.POINTER(Io), c.POINTER(Result)]
    function.restype = c.c_int

    def run(model):
        result = Result()
        return function(c.byref(model.io), c.byref(result)), result
    run.library = library
    return run


class Controller:
    def __init__(self, *, ownership=True, clock=True, access=True, stop=True, removed_after=None):
        self.registers = {0x28: 0x430, 0x9C: 0x2010D000, 0x24: 0x08000000}
        self.ownership, self.clock, self.access, self.stop = ownership, clock, access, stop
        self.writes = []
        self.after_removal_writes = []
        self.removed_after = removed_after
        self.reads = 0
        self.now = 0
        self.removed = False
        self.callbacks = (READ(self.read), WRITE(self.write), DELAY(self.delay), TIME(self.time))
        self.io = Io(None, *self.callbacks)

    def read(self, _, offset):
        self.reads += 1
        if self.removed_after is not None and self.reads > self.removed_after:
            self.removed = True
            return 0xFFFFFFFF
        return self.registers.get(offset, 0)

    def write(self, _, offset, value):
        if self.removed:
            self.after_removal_writes.append((offset, value))
        self.writes.append((offset, value))
        if offset == 0:
            if not self.ownership:
                value &= ~0x00400000
        elif offset == 0x20:
            if value & 0x80:  # Controller reset clears volatile state.
                value = 0
                self.registers[0x24] = 0x08000000
            elif value & 0x200 and self.stop:
                value |= 0x100
        elif offset == 0x24:
            if value & 4 and self.clock:
                value |= 1
            if value & 8 and not self.access:
                value |= 0x10
            else:
                value &= ~0x10
        self.registers[offset] = value

    def delay(self, _, microseconds):
        self.now += microseconds

    def time(self, _):
        return self.now


def test_start_and_stop_leave_interrupts_masked(cycle):
    model = Controller()
    code, result = cycle(model)
    assert code == result.start_error == result.stop_error == 0
    assert result.reset_stopped & 0x100
    assert result.gp_active & 0xD == 0xD
    assert result.gp_after & 0xC == 0
    assert model.registers[0x0C] == 0
    # Independent published MSIX mask addresses from if_iwxreg.h.
    assert model.registers[0x2804] == model.registers[0x280C] == 0xFFFFFFFF
    assert model.registers[0xA0] == 0  # No ICT DMA target.
    assert result.register_writes == len(model.writes)
    assert result.elapsed_us < 100000


@pytest.mark.parametrize("field,value,expected", [(0x28, 0x420, 2), (0x9C, 0, 2), (0x28, 0xFFFFFFFF, 3)])
def test_wrong_or_missing_controller_never_written(cycle, field, value, expected):
    model = Controller()
    model.registers[field] = value
    assert cycle(model)[0] == expected
    assert not model.writes


def test_failed_ownership_does_not_reset_unowned_device(cycle):
    model = Controller(ownership=False)
    code, result = cycle(model)
    assert code == result.start_error == 4
    assert result.stop_error == 8
    assert not any(offset == 0x20 for offset, _ in model.writes)
    assert model.now < 2500000


@pytest.mark.parametrize("failure,expected", [("clock", 5), ("access", 6)])
def test_start_failure_still_shuts_down_owned_device(cycle, failure, expected):
    model = Controller(**{failure: False})
    code, result = cycle(model)
    assert code == result.start_error == expected
    assert result.stop_error == 0
    assert result.reset_stopped & 0x100
    assert model.now < 200000


def test_failed_bus_master_stop_is_not_reported_as_success(cycle):
    code, result = cycle(Controller(stop=False))
    assert result.start_error == 0
    assert code == result.stop_error == 7
    assert result.reset_stopped & 0x100 == 0


def test_surprise_removal_stops_register_writes(cycle):
    model = Controller(removed_after=8)
    code, result = cycle(model)
    assert code == result.stop_error == 3
    assert not model.after_removal_writes


def test_failed_stop_can_be_retried(cycle):
    library = cycle.library

    class State(c.Structure):
        _fields_ = [("io", c.POINTER(Io)), ("result", c.POINTER(Result)),
                    ("removed", c.c_int), ("owned", c.c_int), ("started_us", c.c_uint64)]

    library.iwl_hw_begin.argtypes = [c.POINTER(State), c.POINTER(Io), c.POINTER(Result)]
    library.iwl_hw_end.argtypes = [c.POINTER(State)]
    model, state, result = Controller(stop=False), State(), Result()
    assert library.iwl_hw_begin(c.byref(state), c.byref(model.io), c.byref(result)) == 0
    assert library.iwl_hw_end(c.byref(state)) == 7
    assert state.owned == 1
    model.stop = True
    assert library.iwl_hw_end(c.byref(state)) == 0
    assert state.owned == 0
    assert result.reset_stopped & 0x100
