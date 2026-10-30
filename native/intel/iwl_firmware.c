// SPDX-License-Identifier: GPL-3.0-or-later
// Pure bounded parser for the published Intel TLV container format.
// No firmware execution, modification, device access, allocation or filesystem I/O.
// Format reference: OpenBSD if_iwxreg.h/if_iwx.c (see PROVENANCE.md).
#include "iwl_firmware.h"
#include <string.h>
#define TLV_SEC_RT 19u
#define TLV_PHY_SKU 23u
#define TLV_CPU_COUNT 27u
#define TLV_PAGING 32u
#define TLV_VERSION 36u
#define TLV_IML 52u
#define CPU_SEPARATOR 0xffffccccu
#define PAGING_SEPARATOR 0xaaaabbbbu
#define FW_MAGIC 0x0a4c5749u
#define SEEN_VERSION 1u
#define SEEN_PHY 2u
#define SEEN_CPU 4u
#define SEEN_PAGING 8u
#define SEEN_IML 16u
#define SEEN_ALL 31u

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int iwl_fw_parse(const uint8_t *data, size_t length, IWL_FW_PLAN *plan)
{
    size_t cursor = 88;
    uint32_t seen = 0, group = 0, page_data_bytes = 0;
    if (!plan) return IwlFwInvalidHeader;
    memset(plan, 0, sizeof(*plan));
    if (!data || length < 88) return IwlFwInvalidHeader;
    if (length > IWL_FW_MAX_BYTES) return IwlFwLimit;
    if (u32(data) != 0 || u32(data+4) != FW_MAGIC) return IwlFwInvalidHeader;
    while (cursor < length) {
        uint32_t type, size, flag = 0;
        size_t payload, padded;
        const uint8_t *body;
        if (++plan->record_count > 4096) return IwlFwLimit;
        if (length-cursor < 8) return IwlFwTruncated;
        type = u32(data+cursor);
        size = u32(data+cursor+4);
        payload = cursor+8;
        if ((size_t)size > length-payload) return IwlFwTruncated;
        padded = (size_t)size + ((4u-(size & 3u)) & 3u);
        if (padded > length-payload) return IwlFwTruncated;
        body = data+payload;
        switch (type) {
        case TLV_VERSION:
            flag = SEEN_VERSION;
            if (size != 12) return IwlFwInvalidRecord;
            plan->version_major = u32(body);
            plan->version_minor = u32(body+4);
            plan->version_api = u32(body+8);
            if (plan->version_major != IWL_FW_API_VERSION) return IwlFwUnsupportedVersion;
            break;
        case TLV_PHY_SKU:
            flag = SEEN_PHY;
            if (size != 4) return IwlFwInvalidRecord;
            plan->phy_config = u32(body);
            break;
        case TLV_CPU_COUNT:
            flag = SEEN_CPU;
            if (size != 4 || u32(body) != 2) return IwlFwInvalidRecord;
            plan->cpu_count = u32(body);
            break;
        case TLV_PAGING:
            flag = SEEN_PAGING;
            if (size != 4 || u32(body) > 4u*1024u*1024u ||
                (u32(body) & 4095u)) return IwlFwInvalidRecord;
            plan->paging_bytes = u32(body);
            break;
        case TLV_IML:
            flag = SEEN_IML;
            if (!size || size > 65536u) return IwlFwInvalidRecord;
            plan->iml_offset = (uint32_t)payload;
            plan->iml_length = size;
            break;
        case TLV_SEC_RT: {
            uint32_t device_offset;
            IWL_FW_SECTION *section;
            if (size < 4) return IwlFwInvalidRecord;
            device_offset = u32(body);
            if (device_offset == CPU_SEPARATOR || device_offset == PAGING_SEPARATOR) {
                if (size != 4 && size != 8) return IwlFwInvalidRecord;
                if (device_offset == CPU_SEPARATOR) {
                    if (group != 0 || !plan->group_count[0]) return IwlFwInvalidSectionOrder;
                    group = 1;
                } else {
                    if (group != 1 || !plan->group_count[1]) return IwlFwInvalidSectionOrder;
                    group = 2;
                }
                break;
            }
            if (size == 4 || size-4 > IWL_FW_SECTION_BYTES) return IwlFwInvalidRecord;
            if (plan->section_count == IWL_FW_MAX_SECTIONS ||
                plan->group_count[group] == IWL_FW_MAX_GROUP_SECTIONS) return IwlFwLimit;
            if (device_offset > UINT32_MAX-(size-4)) return IwlFwInvalidRecord;
            section = &plan->sections[plan->section_count++];
            section->file_offset = (uint32_t)payload+4;
            section->length = size-4;
            section->device_offset = device_offset;
            section->group = group;
            // The first paging section is a CSS/signature block, not paged data.
            if (group == 2 && plan->group_count[2]) page_data_bytes += section->length;
            plan->group_count[group]++;
            plan->total_section_bytes += section->length;
            break;
        }
        default:
            // Debug/metadata TLVs are skipped, never interpreted as hardware writes.
            break;
        }
        if (flag && (seen & flag)) return IwlFwInvalidRecord;
        seen |= flag;
        cursor = payload+padded;
    }
    if (seen != SEEN_ALL || group != 2 || !plan->group_count[2])
        return IwlFwMissingRecord;
    if (plan->paging_bytes != page_data_bytes) return IwlFwInvalidRecord;
    return IwlFwOk;
}
const char *iwl_fw_error(int code)
{
    switch (code) {
    case IwlFwOk: return "ok";
    case IwlFwInvalidHeader: return "invalid firmware header";
    case IwlFwTruncated: return "truncated firmware record";
    case IwlFwLimit: return "firmware limit exceeded";
    case IwlFwInvalidRecord: return "invalid or duplicate firmware record";
    case IwlFwUnsupportedVersion: return "unsupported firmware API version";
    case IwlFwMissingRecord: return "required firmware record missing";
    case IwlFwInvalidSectionOrder: return "invalid firmware section order";
    default: return "unknown parse result";
    }
}
