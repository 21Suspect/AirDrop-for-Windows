// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#define IWL_FW_MAX_BYTES (8u * 1024u * 1024u)
#define IWL_FW_MAX_SECTIONS 192u
#define IWL_FW_MAX_GROUP_SECTIONS 64u
#define IWL_FW_SECTION_BYTES 32768u
#define IWL_FW_API_VERSION 77u
enum IWL_FW_RESULT {
    IwlFwOk=0, IwlFwInvalidHeader, IwlFwTruncated, IwlFwLimit,
    IwlFwInvalidRecord, IwlFwUnsupportedVersion, IwlFwMissingRecord,
    IwlFwInvalidSectionOrder
};
typedef struct IWL_FW_SECTION {
    uint32_t file_offset;
    uint32_t length;
    uint32_t device_offset;
    uint32_t group; // 0=LMAC, 1=UMAC, 2=paging.
} IWL_FW_SECTION;
typedef struct IWL_FW_PLAN {
    uint32_t version_major, version_minor, version_api;
    uint32_t phy_config, cpu_count, paging_bytes;
    uint32_t iml_offset, iml_length, section_count;
    uint32_t group_count[3];
    uint32_t total_section_bytes, record_count;
    IWL_FW_SECTION sections[IWL_FW_MAX_SECTIONS];
} IWL_FW_PLAN;
#if defined(IWL_FW_TEST_DLL) && defined(_WIN32)
#define IWL_FW_EXPORT __declspec(dllexport)
#else
#define IWL_FW_EXPORT
#endif
IWL_FW_EXPORT int iwl_fw_parse(const uint8_t *data, size_t length, IWL_FW_PLAN *plan);
const char *iwl_fw_error(int code);
