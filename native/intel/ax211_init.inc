// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded firmware/PNVM initialization and capability read. No raw radio TX.
#include "iwl_pnvm.h"
#include "iwl_command.h"
static NTSTATUS Ax211Monitor(AX211_KM_CONTEXT *,AX211_RADIO_RESULT *);

static NTSTATUS Ax211InitBuffer(AX211_KM_CONTEXT *ctx, ULONG bytes,
    WDFCOMMONBUFFER *buffer, AX211_BOOT_RESULT *result)
{
    WDF_COMMON_BUFFER_CONFIG config;
    NTSTATUS status;
    WDF_COMMON_BUFFER_CONFIG_INIT(&config,PAGE_SIZE-1u);
    status=WdfCommonBufferCreateWithConfig(ctx->dma,bytes,&config,
        WDF_NO_OBJECT_ATTRIBUTES,buffer);
    if(NT_SUCCESS(status)) {
        RtlZeroMemory(WdfCommonBufferGetAlignedVirtualAddress(*buffer),bytes);
        result->allocated_buffers++;result->allocated_bytes+=bytes;
    }
    return status;
}
static ULONG Ax211ReadLe32(const UCHAR *p)
{
    return (ULONG)p[0]|(ULONG)p[1]<<8|(ULONG)p[2]<<16|(ULONG)p[3]<<24;
}
static NTSTATUS Ax211WaitResponse(AX211_KM_CONTEXT *ctx, AX211_BOOT_RESULT *result,
    ULONG group, ULONG opcode, ULONG index, ULONG timeout_us)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONGLONG deadline=Ax211HwNow(ctx)+timeout_us;
    while(Ax211HwNow(ctx)<deadline) {
        NTSTATUS status;
        ULONG drained;
        if(index==MAXULONG && ((group==12 && opcode==0xfe && work->pnvm_complete) ||
            (group==0 && opcode==4 && work->init_complete))) return STATUS_SUCCESS;
        status=Ax211BootCauses(ctx,result);
        if(!NT_SUCCESS(status)) return status;
        for(drained=0;drained<IWL_RX_COUNT;++drained) {
            IWL_RESPONSE_INFO *response=&work->response_info;
            status=Ax211BootNextResponse(ctx,result);
            if(status==STATUS_NO_MORE_ENTRIES) break;
            if(!NT_SUCCESS(status)) return status;
            if(response->opcode==2 && response->group<=1)
                return STATUS_DEVICE_HARDWARE_ERROR; // REPLY_ERROR notification.
            if(response->group==12 && response->opcode==0xfe && (response->queue&0x80u)) {
                if(response->payload_bytes!=4) return STATUS_DATA_ERROR;
                work->pnvm_status=Ax211ReadLe32(work->response+8);
                work->pnvm_complete=TRUE;
            }
            if(response->group<=1 && response->opcode==4 && (response->queue&0x80u))
                work->init_complete=TRUE;
            if(index==MAXULONG) {
                if((response->queue&0x80u) && response->opcode==opcode &&
                    (response->group==group || (group==0 && response->group==1)))
                    return STATUS_SUCCESS;
            } else if(response->queue==0 && response->index==index &&
                response->group==group && response->opcode==opcode) return STATUS_SUCCESS;
        }
        Ax211HwDelay(ctx,1000);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS Ax211Command(AX211_KM_CONTEXT *ctx, AX211_INIT_RESULT *result,
    UCHAR group, UCHAR opcode, UCHAR version, const void *data, SIZE_T bytes)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONG index=work->tx_cursor;
    UCHAR *command,*tfd;
    ULONGLONG address;
    if(!work->command_buffer || index>=IWL_TX_COUNT) return STATUS_DEVICE_BUSY;
    command=(UCHAR *)WdfCommonBufferGetAlignedVirtualAddress(work->command_buffer)+index*4096u;
    address=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->command_buffer).QuadPart
        +(ULONGLONG)index*4096u;
    tfd=(UCHAR *)Ax211BootMemory(work,IwlGen3TxCmd)+index*IWL_TFD_BYTES;
    if(group==1 && opcode==0x91) {
        if(version!=0 || iwl_beacon_command_build((UCHAR)index,address,data,bytes,
            command,4096,tfd,IWL_TFD_BYTES)) return STATUS_INVALID_PARAMETER;
    } else if(iwl_command_build(opcode,group,version,(UCHAR)index,address,data,bytes,
        command,4096,tfd,IWL_TFD_BYTES)!=0) return STATUS_INVALID_PARAMETER;
    KeMemoryBarrier();
    work->tx_cursor++;
    Ax211HwWrite(ctx,0x460,work->tx_cursor); // Command queue zero.
    result->commands_sent++;
    return Ax211WaitResponse(ctx,&result->boot,group,opcode,index,2000000u);
}
static NTSTATUS Ax211InitCommand(AX211_KM_CONTEXT *ctx,AX211_INIT_RESULT *result,
    UCHAR group,UCHAR opcode,ULONG value)
{
    return Ax211Command(ctx,result,group,opcode,0,&value,sizeof(value));
}
static NTSTATUS Ax211InitPnvm(AX211_KM_CONTEXT *ctx, const UCHAR *data,
    SIZE_T length, AX211_INIT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    IWL_PNVM_PLAN plan;
    IWL_GEN3_SCRATCH *scratch;
    NTSTATUS status;
    UCHAR *target;
    ULONG i,offset=0;
    if(!(result->boot.sku[0]|result->boot.sku[1]|result->boot.sku[2]))
        return STATUS_SUCCESS; // Published reference: no platform-NVM request.
    if(iwl_metadata_bit(&work->metadata,32,1)) return STATUS_NOT_SUPPORTED;
    if(iwl_pnvm_parse(data,length,(const uint32_t *)result->boot.sku,0x43,0x10d,&plan)!=IwlPnvmOk)
        return STATUS_INVALID_IMAGE_FORMAT;
    status=Ax211InitBuffer(ctx,plan.data_bytes,&work->pnvm_buffer,&result->boot);
    if(!NT_SUCCESS(status)) return status;
    target=(UCHAR *)WdfCommonBufferGetAlignedVirtualAddress(work->pnvm_buffer);
    for(i=0;i<plan.section_count;++i) {
        RtlCopyMemory(target+offset,data+plan.sections[i].file_offset,plan.sections[i].length);
        offset+=plan.sections[i].length;
    }
    scratch=(IWL_GEN3_SCRATCH *)Ax211BootMemory(work,IwlGen3Scratch);
    scratch->pnvm=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->pnvm_buffer).QuadPart;
    scratch->pnvm_size=plan.data_bytes;
    result->pnvm_version=plan.version;result->pnvm_bytes=plan.data_bytes;
    KeMemoryBarrier();
    if(!Ax211NicAccess(ctx)) return STATUS_DEVICE_NOT_READY;
    Ax211PrphWriteLocked(ctx,0x00d05c04u,1u<<20);
    status=Ax211WaitResponse(ctx,&result->boot,12,0xfe,MAXULONG,2000000u);
    Ax211NicRelease(ctx);
    result->pnvm_complete=work->pnvm_complete;
    result->pnvm_status=work->pnvm_status; // Opaque firmware status, retained verbatim.
    return status;
}
static void Ax211InitializeFirmware(AX211_KM_CONTEXT *ctx, const UCHAR *input,
    SIZE_T length, AX211_INIT_RESULT *result, AX211_RADIO_RESULT *radio)
{
    AX211_INIT_INPUT header;
    AX211_BOOT_RESULT *boot=&result->boot;
    IWL_NVM_INFO nvm;
    NTSTATUS status=STATUS_INVALID_PARAMETER,stop_status=STATUS_SUCCESS;
    ULONGLONG begin=Ax211HwNow(ctx);
    const UCHAR *firmware,*pnvm;
    unsigned version;
    RtlZeroMemory(result,sizeof(*result));boot->size=sizeof(*boot);boot->version=2;
    boot->stage=1;
    if(ctx->boot) { boot->error=1;boot->kernel_status=(ULONG)STATUS_DEVICE_BUSY;return; }
    if(length<sizeof(header)) goto done;
    RtlCopyMemory(&header,input,sizeof(header));
    if(header.magic!=AX211_INIT_MAGIC || header.version!=1 ||
        header.firmware_bytes<88 || header.firmware_bytes>IWL_FW_MAX_BYTES ||
        header.pnvm_bytes>IWL_PNVM_MAX_BYTES ||
        length!=sizeof(header)+(SIZE_T)header.firmware_bytes+header.pnvm_bytes) goto done;
    firmware=input+sizeof(header);pnvm=firmware+header.firmware_bytes;
    status=Ax211BootAllocate(ctx,firmware,header.firmware_bytes,boot);
    if(!NT_SUCCESS(status)) goto done;
    status=Ax211InitBuffer(ctx,1048576u,&ctx->boot->command_buffer,boot);
    if(!NT_SUCCESS(status)) goto done;
    boot->stage=3;
    status=Ax211BootStart(ctx,boot);
    if(!NT_SUCCESS(status)) goto done;
    status=Ax211BootWaitAlive(ctx,boot);
    if(!NT_SUCCESS(status)) goto done;
    boot->stage=8;
    status=Ax211InitPnvm(ctx,pnvm,header.pnvm_bytes,result);
    if(!NT_SUCCESS(status)) goto done;
    boot->stage=9;
    status=Ax211InitCommand(ctx,result,2,3,2); // INIT_EXTENDED_CFG: INIT_NVM
    if(!NT_SUCCESS(status)) goto done;
    boot->stage=10;
    status=Ax211InitCommand(ctx,result,12,0,0); // NVM_ACCESS_COMPLETE
    if(!NT_SUCCESS(status)) goto done;
    status=Ax211WaitResponse(ctx,boot,0,4,MAXULONG,2000000u);
    result->init_complete=ctx->boot->init_complete;
    if(!NT_SUCCESS(status)) goto done;
    boot->stage=11;
    status=Ax211InitCommand(ctx,result,12,2,0); // NVM_GET_INFO, read only.
    if(!NT_SUCCESS(status)) goto done;
    version=iwl_metadata_bit(&ctx->boot->metadata,48,0) ? 4 : 3;
    if(iwl_nvm_decode(ctx->boot->response+8,ctx->boot->response_info.payload_bytes,version,&nvm)!=0) {
        status=STATUS_DATA_ERROR;goto done;
    }
    result->nvm_version=nvm.version;result->nvm_flags=nvm.flags;
    result->mac_flags=nvm.mac_flags;result->tx_chains=nvm.tx_chains;result->rx_chains=nvm.rx_chains;
    result->lar_enabled=nvm.lar_enabled;result->channel_count=nvm.channel_count;
    result->channel6_flags=nvm.channel_count>=6 ? nvm.channels[5] : 0;
    result->nvm_valid=1;boot->stage=12;
    if(radio) status=Ax211Monitor(ctx,radio);
done:
    // Only the explicit continuous START operation can retain live DMA.
    if(ctx->boot && !(NT_SUCCESS(status) && ctx->session_requested)) {
        stop_status=Ax211BootQuiesce(ctx,boot);Ax211BootFree(ctx);
    }
    boot->buffers_released=ctx->boot==NULL;
    boot->gp_control=Ax211HwRead(ctx,0x24);boot->reset_status=Ax211HwRead(ctx,0x20);
    if(!NT_SUCCESS(stop_status)) status=stop_status;
    boot->kernel_status=(ULONG)status;boot->error=NT_SUCCESS(status)?0:boot->stage;
    boot->elapsed_us=(ULONG)(Ax211HwNow(ctx)-begin);
}
