// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"
#define IWL_METADATA_WORDS 16u
#define IWL_METADATA_COMMANDS 512u
typedef struct IWL_COMMAND_VERSION { uint8_t opcode, group, command, notification; } IWL_COMMAND_VERSION;
typedef struct IWL_METADATA {
    uint32_t command_count, api[16], capabilities[16];
    uint32_t calibration_flow, calibration_event, calibration_present;
    IWL_COMMAND_VERSION commands[512];
} IWL_METADATA;
IWL_FW_EXPORT int iwl_metadata_parse(const uint8_t *, size_t, IWL_METADATA *);
// 99 is the published unknown-version sentinel. First matching entry wins,
// as in the API-77 reference driver; firmware can list command and notification
// variants separately.
IWL_FW_EXPORT unsigned iwl_metadata_version(const IWL_METADATA *, unsigned group,
    unsigned opcode, int notification);
IWL_FW_EXPORT int iwl_metadata_bit(const IWL_METADATA *, unsigned bit, int capability);
