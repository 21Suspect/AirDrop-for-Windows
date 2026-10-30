// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded SO/GF firmware startup. Addresses come only from Windows DMA buffers.
#include "iwl_gen3.h"
#include "iwl_metadata.h"
#include "iwl_response.h"

typedef struct AX211_BOOT_WORK {
    IWL_FW_PLAN plan;
    IWL_METADATA metadata;
    WDFCOMMONBUFFER pnvm_buffer, command_buffer, tx_tfd_buffer, tx_count_buffer, tx_frame_buffer;
    WDFCOMMONBUFFER ibss_data_tfd, ibss_data_count;
    ULONG ibss_data_queue, ibss_data_index;
    ULONG pending_tx_queue, pending_tx_index;
    ULONG rx_cursor, tx_cursor, lmac_error_address, umac_error_address;
    BOOLEAN init_complete, pnvm_complete;
    ULONG pnvm_status;
    UCHAR response[4096], decoded_frame[4096];
    IWL_RESPONSE_INFO response_info;
    IWL_GEN3_INPUT map;
    IWL_GEN3_LAYOUT layout;
    WDFCOMMONBUFFER buffers[IWL_GEN3_REGION_COUNT];
    ULONG allocated;
    BOOLEAN armed;
    IWL_HW_IO io;
    IWL_HW_STATE hardware;
    IWL_HW_RESULT power;
} AX211_BOOT_WORK;

static PVOID Ax211BootMemory(AX211_BOOT_WORK *work, ULONG index)
{
    return WdfCommonBufferGetAlignedVirtualAddress(work->buffers[index]);
}
static BOOLEAN Ax211NicAccess(AX211_KM_CONTEXT *ctx)
{
    ULONG gp=Ax211HwRead(ctx,0x24),i;
    if(gp==MAXULONG) return FALSE;
    Ax211HwWrite(ctx,0x24,gp|8u);
    KeStallExecutionProcessor(2);
    for(i=0;i<15000;++i) {
        gp=Ax211HwRead(ctx,0x24);
        if(gp==MAXULONG) return FALSE;
        if((gp&0x11u)==1u) return TRUE;
        KeStallExecutionProcessor(10);
    }
    Ax211HwWrite(ctx,0x24,gp&~8u);
    return FALSE;
}
static void Ax211NicRelease(AX211_KM_CONTEXT *ctx)
{
    ULONG gp=Ax211HwRead(ctx,0x24);
    if(gp!=MAXULONG) Ax211HwWrite(ctx,0x24,gp&~8u);
}
static void Ax211PrphWriteLocked(AX211_KM_CONTEXT *ctx, ULONG address, ULONG value)
{
    Ax211HwWrite(ctx,0x444,(address&0x00ffffffu)|0x03000000u);
    KeMemoryBarrier();
    Ax211HwWrite(ctx,0x44c,value);
}
static ULONG Ax211PrphReadLocked(AX211_KM_CONTEXT *ctx, ULONG address)
{
    Ax211HwWrite(ctx,0x448,(address&0x00ffffffu)|0x03000000u);
    KeMemoryBarrier();
    return Ax211HwRead(ctx,0x450);
}
static void Ax211BootFree(AX211_KM_CONTEXT *ctx)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONG i;
    if(!work || work->armed) return;
    for(i=0;i<work->allocated;++i) WdfObjectDelete(work->buffers[i]);
    if(work->pnvm_buffer) WdfObjectDelete(work->pnvm_buffer);
    if(work->command_buffer) WdfObjectDelete(work->command_buffer);
    if(work->tx_tfd_buffer) WdfObjectDelete(work->tx_tfd_buffer);
    if(work->tx_count_buffer) WdfObjectDelete(work->tx_count_buffer);
    if(work->tx_frame_buffer) WdfObjectDelete(work->tx_frame_buffer);
    if(work->ibss_data_tfd) WdfObjectDelete(work->ibss_data_tfd);
    if(work->ibss_data_count) WdfObjectDelete(work->ibss_data_count);
    ExFreePoolWithTag(work,'bWdO');
    ctx->boot=NULL;
}
static NTSTATUS Ax211BootQuiesce(AX211_KM_CONTEXT *ctx, AX211_BOOT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONG i;
    int stopped;
    if(!work) return STATUS_SUCCESS;
    Ax211DisarmInterrupts(ctx);
    if(!work->hardware.owned && !work->armed) return STATUS_SUCCESS;
    if(work->armed && Ax211NicAccess(ctx)) {
        // SO/GF UMAC peripheral window is offset by 0x300000.
        Ax211PrphWriteLocked(ctx,0x00d07880u,0); // RFH_RXF_DMA_CFG_GEN3
        for(i=0;i<1000;++i) {
            if(Ax211PrphReadLocked(ctx,0x00d07824u)&0x80000000u) break;
            KeStallExecutionProcessor(10);
        }
        Ax211NicRelease(ctx);
    }
    stopped=iwl_hw_end(&work->hardware);
    if(result) result->stop_error=(ULONG)stopped;
    if(stopped==IwlHwOk || stopped==IwlHwRemoved) {
        work->armed=FALSE;
        return STATUS_SUCCESS;
    }
    if(work->armed && ctx->reset_interface.DeviceReset &&
        (ctx->reset_interface.SupportedResetTypes & (1u<<FunctionLevelDeviceReset))) {
        NTSTATUS reset_status;
        if(result) result->function_reset_attempted=1;
        // Supported, device-local Windows reset. Never request a platform reset.
        reset_status=ctx->reset_interface.DeviceReset(ctx->reset_interface.Context,
            FunctionLevelDeviceReset,0,NULL);
        if(result) result->function_reset_status=(ULONG)reset_status;
        if(reset_status==STATUS_SUCCESS) {
            work->armed=FALSE;
            work->hardware.owned=0;
            return STATUS_SUCCESS;
        }
    }
    // Keep all common buffers alive if the device's DMA stop was not confirmed.
    return STATUS_DEVICE_HARDWARE_ERROR;
}
static NTSTATUS Ax211BootAllocate(AX211_KM_CONTEXT *ctx, const UCHAR *image,
    SIZE_T length, AX211_BOOT_RESULT *result)
{
    static const ULONG sizes[10]={4096,4096,4096,8192,16384,4096,65536,
        1048576,1048576,0};
    AX211_BOOT_WORK *work;
    WDF_COMMON_BUFFER_CONFIG config;
    NTSTATUS status;
    ULONG i;
    int layout_status;
    work=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*work),'bWdO');
    if(!work) return STATUS_INSUFFICIENT_RESOURCES;
    ctx->boot=work;
    RtlZeroMemory(&ctx->fault,sizeof(ctx->fault));
    ctx->fault.size=sizeof(ctx->fault);ctx->fault.version=1;
    if(iwl_fw_parse(image,length,&work->plan)!=IwlFwOk) return STATUS_INVALID_IMAGE_FORMAT;
    if(iwl_metadata_parse(image,length,&work->metadata)!=0) return STATUS_INVALID_IMAGE_FORMAT;
    work->map.count=IwlGen3Firmware+work->plan.section_count;
    WDF_COMMON_BUFFER_CONFIG_INIT(&config,PAGE_SIZE-1u);
    for(i=0;i<work->map.count;++i) {
        ULONG bytes=i<IwlGen3Iml ? sizes[i] :
            (i==IwlGen3Iml ? work->plan.iml_length :
            work->plan.sections[i-IwlGen3Firmware].length);
        status=WdfCommonBufferCreateWithConfig(ctx->dma,bytes,&config,
            WDF_NO_OBJECT_ATTRIBUTES,&work->buffers[i]);
        if(!NT_SUCCESS(status)) return status;
        work->allocated++;
        RtlZeroMemory(Ax211BootMemory(work,i),bytes);
        work->map.regions[i].address=(ULONGLONG)
            WdfCommonBufferGetAlignedLogicalAddress(work->buffers[i]).QuadPart;
        work->map.regions[i].length=bytes;
        result->allocated_bytes+=bytes;
    }
    result->allocated_buffers=work->allocated;
    layout_status=iwl_gen3_build(&work->plan,&work->map,&work->layout,sizeof(work->layout));
    if(layout_status!=IwlGen3Ok) return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlCopyMemory(Ax211BootMemory(work,IwlGen3Context),
        &work->layout.context,sizeof(work->layout.context));
    RtlCopyMemory(Ax211BootMemory(work,IwlGen3Scratch),
        &work->layout.scratch,sizeof(work->layout.scratch));
    RtlCopyMemory(Ax211BootMemory(work,IwlGen3RxFree),
        work->layout.rx,sizeof(work->layout.rx));
    RtlCopyMemory(Ax211BootMemory(work,IwlGen3Iml),
        image+work->plan.iml_offset,work->plan.iml_length);
    for(i=0;i<work->plan.section_count;++i)
        RtlCopyMemory(Ax211BootMemory(work,IwlGen3Firmware+i),
            image+work->plan.sections[i].file_offset,work->plan.sections[i].length);
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

static void Ax211BootWriteAddress(AX211_KM_CONTEXT *ctx, ULONG offset, ULONGLONG value)
{
    Ax211HwWrite(ctx,offset,(ULONG)value);
    Ax211HwWrite(ctx,offset+4,(ULONG)(value>>32));
}
static NTSTATUS Ax211BootStart(AX211_KM_CONTEXT *ctx, AX211_BOOT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONG value;
    int status;
    result->irq_resources=ctx->interrupt_count;
    result->irq_connected=(ULONG)InterlockedCompareExchange(&ctx->interrupt_connected,0,0);
    if(!NT_SUCCESS((NTSTATUS)ctx->irq_diagnostics.setup_status) ||
        !result->irq_resources || !result->irq_connected)
        return STATUS_DEVICE_NOT_READY;
    work->io.context=ctx;work->io.read32=Ax211HwRead;work->io.write32=Ax211HwWrite;
    work->io.delay_us=Ax211HwDelay;work->io.now_us=Ax211HwNow;
    status=iwl_hw_begin(&work->hardware,&work->io,&work->power);
    result->start_error=(ULONG)status;
    if(status!=IwlHwOk) return STATUS_DEVICE_NOT_READY;
    result->stage=4;
    if(!Ax211NicAccess(ctx)) return STATUS_DEVICE_NOT_READY;
    // Configure device MSI-X mode. Windows has already connected the handlers.
    // Sources stay masked until the internal cause map is installed below.
    Ax211PrphWriteLocked(ctx,0x00d05c00u,1u<<25);
    Ax211NicRelease(ctx);
    Ax211HwWrite(ctx,0x2804,MAXULONG);
    Ax211HwWrite(ctx,0x280c,MAXULONG);
    Ax211HwWrite(ctx,0x2800,MAXULONG);
    Ax211HwWrite(ctx,0x2808,MAXULONG);
    Ax211HwWrite(ctx,0x5c,2); // Clear software RF-kill handshake.
    Ax211HwWrite(ctx,0x5c,4); // Clear command-blocked handshake.
    WRITE_REGISTER_UCHAR(ctx->registers+4,0x40);
    value=Ax211HwRead(ctx,0xa8);
    if(value==MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    Ax211HwWrite(ctx,0xa8,value|0x800fffffu);
    {
        NTSTATUS irq_status=Ax211ArmInterrupts(ctx,result);
        if(!NT_SUCCESS(irq_status)) return irq_status;
    }
    result->iml_before=Ax211HwRead(ctx,0x12c);
    if(!Ax211NicAccess(ctx)) return STATUS_DEVICE_NOT_READY;
    work->armed=TRUE; // Retain every buffer until confirmed stopped.
    result->dma_started=1;
    KeMemoryBarrier();
    Ax211BootWriteAddress(ctx,0x118,work->map.regions[IwlGen3Context].address);
    Ax211BootWriteAddress(ctx,0x120,work->map.regions[IwlGen3Iml].address);
    Ax211HwWrite(ctx,0x128,work->plan.iml_length);
    value=Ax211HwRead(ctx,0);
    if(value==MAXULONG) { Ax211NicRelease(ctx); return STATUS_DEVICE_NOT_CONNECTED; }
    Ax211HwWrite(ctx,0,value|2u);
    Ax211PrphWriteLocked(ctx,0x00d05c44u,1); // UREG_CPU_INIT_RUN, SO/GF UMAC.
    Ax211NicRelease(ctx);
    result->stage=5;
    return STATUS_SUCCESS;
}
static void Ax211CaptureFault(AX211_KM_CONTEXT *ctx,AX211_BOOT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    AX211_FAULT_DIAGNOSTICS *fault=&ctx->fault;
    ULONG i;
    if(!work || !result->firmware_alive || fault->observed) return;
    fault->observed=1;
    fault->lmac_address=work->lmac_error_address;
    fault->umac_address=work->umac_error_address;
    if(!Ax211NicAccess(ctx)) return;
    // Device-local SRAM reads, bounded to two published error tables. No host
    // addresses, table pointers from a caller, or arbitrary read IOCTL exists.
    if(iwl_gen3_error_log_range(fault->lmac_address,38)) {
        Ax211HwWrite(ctx,0x40c,fault->lmac_address);
        for(i=0;i<38;++i) fault->lmac[i]=Ax211HwRead(ctx,0x41c);
        if(fault->lmac[0]!=MAXULONG) fault->read_mask|=1;
    }
    if(iwl_gen3_error_log_range(fault->umac_address,15)) {
        Ax211HwWrite(ctx,0x40c,fault->umac_address);
        for(i=0;i<15;++i) fault->umac[i]=Ax211HwRead(ctx,0x41c);
        if(fault->umac[0]!=MAXULONG) fault->read_mask|=2;
    }
    Ax211NicRelease(ctx);
}
static NTSTATUS Ax211BootCauses(AX211_KM_CONTEXT *ctx, AX211_BOOT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONG interrupt=0;
    ULONG msix=(ULONG)InterlockedExchange(&ctx->interrupt_hw,0);
    ULONG fh=(ULONG)InterlockedExchange(&ctx->interrupt_fh,0);
    volatile ULONG *mirror=(volatile ULONG *)Ax211BootMemory(work,IwlGen3Info);
    if(InterlockedCompareExchange(&ctx->interrupt_removed,0,0))
        return STATUS_DEVICE_NOT_CONNECTED;
    result->irq_hits=(ULONG)InterlockedCompareExchange(&ctx->interrupt_hits,0,0);
    result->interrupts|=interrupt;result->msix_causes|=msix;result->fh_causes|=fh;
    if((interrupt&0x22000000u) || (msix&0x22000028u) || (fh&(1u<<21))) {
        Ax211CaptureFault(ctx,result);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    if(((interrupt|msix)&1u) && !result->hardware_alive) {
        result->hardware_alive=1;
        RtlCopyMemory(Ax211BootMemory(work,IwlGen3RxFree),work->layout.rx,sizeof(work->layout.rx));
        KeMemoryBarrier();
        Ax211HwWrite(ctx,0x1c80,8);
        result->stage=6;
    }
    KeMemoryBarrier();
    result->boot_mirror=*mirror;
    result->iml_response=Ax211HwRead(ctx,0x12c);
    return STATUS_SUCCESS;
}
static NTSTATUS Ax211BootNextResponse(AX211_KM_CONTEXT *ctx, AX211_BOOT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    volatile USHORT *head=(volatile USHORT *)Ax211BootMemory(work,IwlGen3RxStatus);
    ULONG producer,current=work->rx_cursor;
    UCHAR *packet;
    KeMemoryBarrier();
    producer=*head & (IWL_RX_COUNT-1u);result->rx_head=producer;
    if(current==producer) return STATUS_NO_MORE_ENTRIES;
    packet=(UCHAR *)Ax211BootMemory(work,current<256 ? IwlGen3RxDataA : IwlGen3RxDataB)
        +(current%256)*IWL_RX_BYTES;
    KeMemoryBarrier();
    if(iwl_response_parse(packet,IWL_RX_BYTES,&work->response_info)!=0)
        return STATUS_DATA_ERROR;
    RtlCopyMemory(work->response,packet,work->response_info.packet_bytes);
    result->last_packet_code=work->response_info.opcode | (work->response_info.group<<8);
    result->rx_packets++;
    // AX210-family devices publish one packet per buffer. Copy before returning
    // ownership, then clear stale contents before the next producer wrap.
    RtlZeroMemory(packet,IWL_RX_BYTES);
    work->rx_cursor=(current+1u)&(IWL_RX_COUNT-1u);
    KeMemoryBarrier();
    if(result->hardware_alive)
        Ax211HwWrite(ctx,0x1c80,((work->rx_cursor-1u)&(IWL_RX_COUNT-1u))&~7u);
    return STATUS_SUCCESS;
}
static NTSTATUS Ax211BootWaitAlive(AX211_KM_CONTEXT *ctx, AX211_BOOT_RESULT *result)
{
    AX211_BOOT_WORK *work=ctx->boot;
    ULONGLONG deadline=Ax211HwNow(ctx)+5000000u;
    while(Ax211HwNow(ctx)<deadline) {
        NTSTATUS status=Ax211BootCauses(ctx,result);
        if(!NT_SUCCESS(status)) return status;
        ULONG drained;
        for(drained=0;drained<IWL_RX_COUNT;++drained) {
            IWL_ALIVE_INFO alive;
            int parsed;
            status=Ax211BootNextResponse(ctx,result);
            if(status==STATUS_NO_MORE_ENTRIES) break;
            if(!NT_SUCCESS(status)) return status;
            parsed=iwl_gen3_decode_alive(work->response,work->response_info.packet_bytes,&alive);
            if(parsed==2) return STATUS_DATA_ERROR;
            if(parsed==0) {
                result->alive_status=alive.status;result->alive_payload_bytes=alive.payload_bytes;
                result->lmac_major=alive.lmac_major;result->lmac_minor=alive.lmac_minor;
                result->umac_major=alive.umac_major;result->umac_minor=alive.umac_minor;
                RtlCopyMemory(result->sku,alive.sku,sizeof(result->sku));
                if(alive.status!=0xcafeu) return STATUS_DEVICE_HARDWARE_ERROR;
                work->lmac_error_address=alive.lmac_error;
                work->umac_error_address=alive.umac_error;
                result->firmware_alive=1;result->stage=7;
                return STATUS_SUCCESS;
            }
        }
        Ax211HwDelay(ctx,1000);
    }
    if(Ax211NicAccess(ctx)) {
        result->rx_dma_config=Ax211PrphReadLocked(ctx,0x00d07880u);
        result->rx_dma_status=Ax211PrphReadLocked(ctx,0x00d07824u);
        Ax211NicRelease(ctx);
    }
    return STATUS_IO_TIMEOUT;
}
static void Ax211BootFirmware(AX211_KM_CONTEXT *ctx, const UCHAR *image,
    SIZE_T length, AX211_BOOT_RESULT *result)
{
    NTSTATUS status=STATUS_SUCCESS,stop_status=STATUS_SUCCESS;
    ULONGLONG begin=Ax211HwNow(ctx);
    RtlZeroMemory(result,sizeof(*result));
    result->size=sizeof(*result);result->version=2;
    if(ctx->boot) { result->kernel_status=(ULONG)STATUS_DEVICE_BUSY;result->error=1;return; }
    if(length<88 || length>IWL_FW_MAX_BYTES) {
        result->kernel_status=(ULONG)STATUS_INVALID_BUFFER_SIZE;result->error=2;return;
    }
    result->stage=1;
    status=Ax211BootAllocate(ctx,image,length,result);
    if(!NT_SUCCESS(status)) goto done;
    result->stage=3;
    status=Ax211BootStart(ctx,result);
    if(!NT_SUCCESS(status)) goto done;
    status=Ax211BootWaitAlive(ctx,result);
done:
    if(ctx->boot) {
        stop_status=Ax211BootQuiesce(ctx,result);
        Ax211BootFree(ctx);
    }
    result->buffers_released=ctx->boot==NULL;
    result->gp_control=Ax211HwRead(ctx,0x24);
    result->reset_status=Ax211HwRead(ctx,0x20);
    if(!NT_SUCCESS(stop_status)) status=stop_status;
    result->kernel_status=(ULONG)status;
    result->error=NT_SUCCESS(status)?0:result->stage;
    result->elapsed_us=(ULONG)(Ax211HwNow(ctx)-begin);
}
