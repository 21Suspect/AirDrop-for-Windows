// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"
#define IWL_PNVM_MAX_BYTES (1024u*1024u)
#define IWL_PNVM_MAX_SECTIONS 64u
typedef struct IWL_PNVM_SECTION { uint32_t file_offset, length; } IWL_PNVM_SECTION;
typedef struct IWL_PNVM_PLAN {
    uint32_t version, data_bytes, section_count, sku[3];
    IWL_PNVM_SECTION sections[IWL_PNVM_MAX_SECTIONS];
} IWL_PNVM_PLAN;
enum IWL_PNVM_ERROR {
    IwlPnvmOk=0, IwlPnvmInvalid=1, IwlPnvmTruncated=2, IwlPnvmLimit=3,
    IwlPnvmNoMatch=4, IwlPnvmAmbiguous=5
};
// Selects published PNVM TLV payloads, unchanged, for firmware-provided SKU.
// Does not write persistent device memory or modify calibration data.
IWL_FW_EXPORT int iwl_pnvm_parse(const uint8_t *, size_t, const uint32_t sku[3],
    uint16_t mac_type, uint16_t rf_type, IWL_PNVM_PLAN *);
