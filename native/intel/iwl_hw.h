// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#if defined(_WIN32) && defined(IWL_FW_TEST_DLL)
#define IWL_HW_API __declspec(dllexport)
#else
#define IWL_HW_API
#endif

// Fixed SO/GF controller sequence; callers cannot select register addresses.
typedef struct IWL_HW_IO {
    void *context;
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    void (*delay_us)(void *, uint32_t);
    uint64_t (*now_us)(void *);
} IWL_HW_IO;

enum IWL_HW_ERROR {
    IwlHwOk=0, IwlHwInvalid=1, IwlHwWrongDevice=2, IwlHwRemoved=3,
    IwlHwOwnershipTimeout=4, IwlHwClockTimeout=5, IwlHwAccessTimeout=6,
    IwlHwStopTimeout=7, IwlHwNotStarted=8
};
typedef struct IWL_HW_RESULT {
    uint32_t size, version, start_error, stop_error;
    uint32_t gp_before, gp_active, gp_after, reset_stopped;
    uint32_t hw_revision, rf_id, register_writes, elapsed_us;
} IWL_HW_RESULT;
typedef struct IWL_HW_STATE {
    const IWL_HW_IO *io;
    IWL_HW_RESULT *result;
    int removed, owned;
    uint64_t started_us;
} IWL_HW_STATE;
IWL_HW_API int iwl_hw_begin(IWL_HW_STATE *, const IWL_HW_IO *, IWL_HW_RESULT *);
IWL_HW_API int iwl_hw_end(IWL_HW_STATE *);
IWL_HW_API int iwl_hw_power_cycle(const IWL_HW_IO *io, IWL_HW_RESULT *result);
