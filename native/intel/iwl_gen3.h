// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"
#include <stdint.h>
#include <stddef.h>
#if defined(_WIN32) && defined(IWL_FW_TEST_DLL)
#define IWL_GEN3_API __declspec(dllexport)
#else
#define IWL_GEN3_API
#endif
#define IWL_RX_COUNT 512u
#define IWL_RX_BYTES 4096u
#define IWL_TX_COUNT 256u
#define IWL_GEN3_REGION_COUNT (10u+IWL_FW_MAX_SECTIONS)
enum IWL_GEN3_REGION_ID {
    IwlGen3Context, IwlGen3Scratch, IwlGen3Info, IwlGen3RxFree,
    IwlGen3RxUsed, IwlGen3RxStatus, IwlGen3TxCmd, IwlGen3RxDataA,
    IwlGen3RxDataB, IwlGen3Iml, IwlGen3Firmware
};
typedef struct IWL_DMA_REGION { uint64_t address; uint32_t length, reserved; } IWL_DMA_REGION;
typedef struct IWL_GEN3_INPUT {
    uint32_t count, reserved;
    IWL_DMA_REGION regions[IWL_GEN3_REGION_COUNT];
} IWL_GEN3_INPUT;

#pragma pack(push,1)
typedef struct IWL_GEN3_CONTEXT {
    uint16_t version, size;
    uint32_t config;
    uint64_t prph_info, cr_head, tr_tail, cr_tail, tr_head;
    uint16_t cr_entries, tr_entries;
    uint64_t mtr, mcr;
    uint16_t mtr_size, mcr_size, mtr_doorbell, mcr_doorbell, mtr_msi, mcr_msi;
    uint8_t mtr_header, mtr_footer, mcr_header, mcr_footer;
    uint16_t ring_flags, prph_msi;
    uint64_t scratch;
    uint32_t scratch_bytes, reserved;
} IWL_GEN3_CONTEXT;
typedef struct IWL_GEN3_SCRATCH {
    uint16_t mac_id, version, size_dwords, version_reserved;
    uint32_t control, control_reserved;
    uint64_t pnvm;
    uint32_t pnvm_size, pnvm_reserved;
    uint64_t monitor;
    uint32_t monitor_size, monitor_config;
    uint64_t rx_free;
    uint32_t rx_reserved;
    uint64_t reduce_power;
    uint32_t reduce_power_size, reduce_power_reserved;
    uint32_t reserved[12];
    uint64_t umac[64], lmac[64], paging[64];
} IWL_GEN3_SCRATCH;
typedef struct IWL_RX_TRANSFER {
    uint16_t id, reserved[3];
    uint64_t address;
} IWL_RX_TRANSFER;
typedef struct IWL_GEN3_LAYOUT {
    IWL_GEN3_CONTEXT context;
    IWL_GEN3_SCRATCH scratch;
    IWL_RX_TRANSFER rx[IWL_RX_COUNT];
} IWL_GEN3_LAYOUT;
#pragma pack(pop)
enum IWL_GEN3_ERROR {
    IwlGen3Ok=0, IwlGen3BadArgument=1, IwlGen3BadPlan=2,
    IwlGen3BadAddress=3, IwlGen3TooSmall=4, IwlGen3Overlap=5
};
IWL_GEN3_API int iwl_gen3_build(const IWL_FW_PLAN *, const IWL_GEN3_INPUT *,
    void *output, size_t output_length);

typedef struct IWL_ALIVE_INFO {
    uint32_t status, payload_bytes, lmac_major, lmac_minor, umac_major, umac_minor;
    uint32_t sku[3], lmac_error, umac_error;
} IWL_ALIVE_INFO;
// 0: valid ALIVE response (inspect status); 1: another notification; 2: malformed.
IWL_GEN3_API int iwl_gen3_decode_alive(const uint8_t *, size_t, IWL_ALIVE_INFO *);

// Only the fixed LMAC/UMAC error-table sizes and documented SO/GF SRAM windows.
IWL_GEN3_API int iwl_gen3_error_log_range(uint32_t address, unsigned dwords);
