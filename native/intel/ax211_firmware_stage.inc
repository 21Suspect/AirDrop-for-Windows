// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware preparation uses Windows DMA buffers but does not start the device.
#include "iwl_firmware.h"

typedef struct AX211_FW_WORK {
    IWL_FW_PLAN plan;
    WDFCOMMONBUFFER buffers[IWL_FW_MAX_SECTIONS+1];
} AX211_FW_WORK;

static ULONG Ax211Crc32(ULONG crc, const UCHAR *data, SIZE_T bytes)
{
    SIZE_T i;
    ULONG bit;
    for (i=0; i<bytes; ++i) {
        crc ^= data[i];
        for (bit=0; bit<8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u-(crc & 1u)));
    }
    return crc;
}
static NTSTATUS Ax211StageFirmware(
    AX211_KM_CONTEXT *ctx, const UCHAR *image, SIZE_T bytes,
    AX211_FIRMWARE_RESULT *result)
{
    AX211_FW_WORK *work;
    WDF_COMMON_BUFFER_CONFIG config;
    NTSTATUS status=STATUS_SUCCESS;
    ULONG index, allocated=0, source_crc=0xffffffffu, copy_crc=0xffffffffu;
    int parse_result;
    if (bytes > IWL_FW_MAX_BYTES || bytes < 88) return STATUS_INVALID_BUFFER_SIZE;
    work=ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*work), 'fWdO');
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;
    parse_result=iwl_fw_parse(image, bytes, &work->plan);
    if (parse_result != IwlFwOk) { status=STATUS_INVALID_IMAGE_FORMAT; goto done; }
    WDF_COMMON_BUFFER_CONFIG_INIT(&config, PAGE_SIZE-1u);
    RtlZeroMemory(result,sizeof(*result));
    result->size=sizeof(*result);
    result->abi_version=1;
    result->firmware_major=work->plan.version_major;
    result->firmware_minor=work->plan.version_minor;
    result->lmac_sections=work->plan.group_count[0];
    result->umac_sections=work->plan.group_count[1];
    result->paging_sections=work->plan.group_count[2];
    result->image_loader_bytes=work->plan.iml_length;
    for (index=0; index<=work->plan.section_count; ++index) {
        SIZE_T size;
        const UCHAR *source;
        PVOID target;
        if (index<work->plan.section_count) {
            size=work->plan.sections[index].length;
            source=image+work->plan.sections[index].file_offset;
        } else {
            size=work->plan.iml_length;
            source=image+work->plan.iml_offset;
        }
        status=WdfCommonBufferCreateWithConfig(ctx->dma,size,&config,
            WDF_NO_OBJECT_ATTRIBUTES,&work->buffers[index]);
        if (!NT_SUCCESS(status)) goto done;
        allocated++;
        target=WdfCommonBufferGetAlignedVirtualAddress(work->buffers[index]);
        RtlCopyMemory(target,source,size);
        KeMemoryBarrier();
        source_crc=Ax211Crc32(source_crc,source,size);
        copy_crc=Ax211Crc32(copy_crc,(const UCHAR *)target,size);
        result->staged_bytes+=(ULONG)size;
    }
    result->buffer_count=allocated;
    result->source_crc32=source_crc ^ 0xffffffffu;
    result->staged_crc32=copy_crc ^ 0xffffffffu;
    if (result->source_crc32!=result->staged_crc32) status=STATUS_DATA_ERROR;
done:
    // No device address was programmed, so all buffers can be released here.
    for (index=0; index<allocated; ++index) WdfObjectDelete(work->buffers[index]);
    ExFreePoolWithTag(work,'fWdO');
    return status;
}
