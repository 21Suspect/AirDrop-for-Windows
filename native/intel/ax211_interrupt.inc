// SPDX-License-Identifier: GPL-3.0-or-later
// Windows owns PCI configuration and the MSI-X table. This driver only reads
// PCI configuration; it routes device-internal causes to Windows message 0.
// The ISR never dereferences boot/DMA buffers and never queues unbounded work.
EVT_WDF_INTERRUPT_ISR Ax211InterruptIsr;
EVT_WDF_INTERRUPT_ENABLE Ax211InterruptEnable;
EVT_WDF_INTERRUPT_DISABLE Ax211InterruptDisable;

#define AX211_IRQ_HW_MASK 0x220000e9u /* ALIVE, fatal/SW errors, RF/CT kill */
#define AX211_IRQ_FH_MASK 0x00200001u /* RX queue 0, FH error */

static void Ax211MaskInterrupts(AX211_KM_CONTEXT *ctx)
{
    Ax211HwWrite(ctx,0x0c,0);
    Ax211HwWrite(ctx,0x2804,MAXULONG);
    Ax211HwWrite(ctx,0x280c,MAXULONG);
}
BOOLEAN Ax211InterruptIsr(WDFINTERRUPT interrupt,ULONG message)
{
    AX211_KM_CONTEXT *ctx=Ax211KmContext(WdfInterruptGetDevice(interrupt));
    ULONG hw,fh;
    UNREFERENCED_PARAMETER(message);
    if(!ctx->registers) return FALSE;
    hw=Ax211HwRead(ctx,0x2808);fh=Ax211HwRead(ctx,0x2800);
    if(hw==MAXULONG || fh==MAXULONG) {
        InterlockedExchange(&ctx->interrupt_removed,1);
        return FALSE;
    }
    Ax211HwWrite(ctx,0x2808,hw);Ax211HwWrite(ctx,0x2800,fh);
    if(InterlockedCompareExchange(&ctx->interrupt_armed,0,0)) {
        InterlockedOr(&ctx->interrupt_hw,(LONG)(hw&AX211_IRQ_HW_MASK));
        InterlockedOr(&ctx->interrupt_fh,(LONG)(fh&AX211_IRQ_FH_MASK));
        InterlockedIncrement(&ctx->interrupt_hits);
    }
    // Hardware automasks a vector until this W1C acknowledgment.
    Ax211HwWrite(ctx,0x2810,1);
    return TRUE;
}
NTSTATUS Ax211InterruptEnable(WDFINTERRUPT interrupt,WDFDEVICE device)
{
    AX211_KM_CONTEXT *ctx=Ax211KmContext(device);
    UNREFERENCED_PARAMETER(interrupt);
    Ax211MaskInterrupts(ctx); // sources only enabled by an explicit boot IOCTL
    InterlockedIncrement(&ctx->interrupt_connected);
    return STATUS_SUCCESS;
}
NTSTATUS Ax211InterruptDisable(WDFINTERRUPT interrupt,WDFDEVICE device)
{
    AX211_KM_CONTEXT *ctx=Ax211KmContext(device);
    UNREFERENCED_PARAMETER(interrupt);
    Ax211MaskInterrupts(ctx);
    InterlockedExchange(&ctx->interrupt_armed,0);
    InterlockedDecrement(&ctx->interrupt_connected);
    return STATUS_SUCCESS;
}
static NTSTATUS Ax211CreateInterrupts(AX211_KM_CONTEXT *ctx,
    WDFCMRESLIST raw,WDFCMRESLIST translated)
{
    ULONG i,count=0,previous=ctx->interrupt_count;
    AX211_IRQ_DIAGNOSTICS *diagnostics=&ctx->irq_diagnostics;
    NTSTATUS validated=STATUS_SUCCESS;
    RtlZeroMemory(diagnostics,sizeof(*diagnostics));
    diagnostics->size=sizeof(*diagnostics);diagnostics->version=1;
    diagnostics->raw_count=WdfCmResourceListGetCount(raw);
    diagnostics->translated_count=WdfCmResourceListGetCount(translated);
    if(diagnostics->raw_count!=diagnostics->translated_count)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    // Validate the whole list before creating any interrupt objects.
    for(i=0;i<diagnostics->translated_count;++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(translated,i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR original=WdfCmResourceListGetDescriptor(raw,i);
        AX211_IRQ_RESOURCE *entry;
        if(!r || !original) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(r->Type!=CmResourceTypeInterrupt) continue;
        if(count>=RTL_NUMBER_OF(diagnostics->resources)) return STATUS_NOT_SUPPORTED;
        entry=&diagnostics->resources[count++];
        entry->index=i;entry->raw_type=original->Type;
        entry->raw_flags=original->Flags;entry->translated_flags=r->Flags;
        if(original->Type==CmResourceTypeInterrupt &&
            (original->Flags&CM_RESOURCE_INTERRUPT_MESSAGE))
            entry->message_count=original->u.MessageInterrupt.Raw.MessageCount;
        entry->vector=r->u.Interrupt.Vector;
        if(original->Type!=CmResourceTypeInterrupt ||
            !(original->Flags&CM_RESOURCE_INTERRUPT_MESSAGE) ||
            !(r->Flags&CM_RESOURCE_INTERRUPT_MESSAGE) || entry->message_count!=1)
            validated=STATUS_NOT_SUPPORTED;
    }
    diagnostics->resource_count=count;
    if(!count) return STATUS_NOT_SUPPORTED;
    if(!NT_SUCCESS(validated)) return validated;
    for(i=0;i<count;++i) {
        WDF_INTERRUPT_CONFIG config;
        NTSTATUS status;
        ULONG index=diagnostics->resources[i].index;
        if(i<previous) continue;
        WDF_INTERRUPT_CONFIG_INIT(&config,Ax211InterruptIsr,NULL);
        config.AutomaticSerialization=FALSE;
        config.SpinLock=ctx->interrupt_lock;
        config.InterruptRaw=WdfCmResourceListGetDescriptor(raw,index);
        config.InterruptTranslated=WdfCmResourceListGetDescriptor(translated,index);
        config.EvtInterruptEnable=Ax211InterruptEnable;
        config.EvtInterruptDisable=Ax211InterruptDisable;
        status=WdfInterruptCreate(ctx->device,&config,WDF_NO_OBJECT_ATTRIBUTES,&ctx->interrupts[i]);
        if(!NT_SUCCESS(status)) return status;
        ctx->interrupt_count=i+1;
    }
    return count && (!previous || count==previous) ?
        STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}
static NTSTATUS Ax211ReadPciState(AX211_KM_CONTEXT *ctx)
{
    BUS_INTERFACE_STANDARD bus;
    UCHAR config[256];
    ULONG bytes,offset,visited;
    NTSTATUS status;
    RtlZeroMemory(&bus,sizeof(bus));RtlZeroMemory(config,sizeof(config));
    status=WdfFdoQueryForInterface(ctx->device,&GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&bus,sizeof(bus),1,NULL);
    if(!NT_SUCCESS(status)) return status;
    bytes=bus.GetBusData ? bus.GetBusData(bus.Context,PCI_WHICHSPACE_CONFIG,
        config,0,sizeof(config)) : 0;
    if(bus.InterfaceDereference) bus.InterfaceDereference(bus.Context);
    if(bytes!=sizeof(config)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ctx->pci_command=(ULONG)config[4]|(ULONG)config[5]<<8;
    ctx->pci_msix=0;
    if(!(config[6]&0x10u)) return STATUS_NOT_SUPPORTED;
    offset=config[0x34]&0xfcu;
    for(visited=0;offset && visited<48;++visited) {
        if(offset<0x40u || offset>0xfcu) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(config[offset]==0x11u) {
            ctx->pci_msix=(ULONG)config[offset+2]|(ULONG)config[offset+3]<<8;
            return STATUS_SUCCESS;
        }
        offset=config[offset+1]&0xfcu;
    }
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS Ax211ArmInterrupts(AX211_KM_CONTEXT *ctx,AX211_BOOT_RESULT *result)
{
    // Device internal cause map from OpenBSD iwx_conf_msix_hw; only message 0.
    static const UCHAR causes[]={0x05,0x10,0x13,0x15,0x16,0x17,0x29,0x2d};
    ULONG i;
    BOOLEAN message_zero=FALSE;
    NTSTATUS status=Ax211ReadPciState(ctx);
    result->pci_command=ctx->pci_command;result->pci_msix=ctx->pci_msix;
    result->irq_resources=ctx->interrupt_count;
    result->irq_connected=(ULONG)InterlockedCompareExchange(&ctx->interrupt_connected,0,0);
    if(!NT_SUCCESS(status)) return status;
    // Require bus mastering and OS-enabled, unmasked MSI-X. Never write PCI headers.
    if((ctx->pci_command&6u)!=6u || (ctx->pci_msix&0xc000u)!=0x8000u)
        return STATUS_DEVICE_NOT_READY;
    for(i=0;i<ctx->interrupt_count;++i) {
        WDF_INTERRUPT_INFO info;
        WDF_INTERRUPT_INFO_INIT(&info);
        WdfInterruptGetInfo(ctx->interrupts[i],&info);
        if(info.MessageSignaled && info.MessageNumber==0 && info.Vector) {
            message_zero=TRUE;ctx->interrupt_vector=info.Vector;
        }
    }
    result->irq_vector=ctx->interrupt_vector;
    if(!message_zero || !result->irq_connected) return STATUS_DEVICE_NOT_READY;
    WdfInterruptAcquireLock(ctx->interrupts[0]);
    Ax211MaskInterrupts(ctx);
    InterlockedExchange(&ctx->interrupt_hits,0);
    InterlockedExchange(&ctx->interrupt_hw,0);
    InterlockedExchange(&ctx->interrupt_fh,0);
    InterlockedExchange(&ctx->interrupt_removed,0);
    Ax211HwWrite(ctx,8,MAXULONG);
    Ax211HwWrite(ctx,0x2800,MAXULONG);Ax211HwWrite(ctx,0x2808,MAXULONG);
    WRITE_REGISTER_UCHAR(ctx->registers+0x2880,0x80);
    for(i=0;i<sizeof(causes);++i)
        WRITE_REGISTER_UCHAR(ctx->registers+0x2890+causes[i],0x80);
    Ax211HwWrite(ctx,0x2810,1);
    InterlockedExchange(&ctx->interrupt_armed,1);
    Ax211HwWrite(ctx,0x2804,~AX211_IRQ_FH_MASK);
    Ax211HwWrite(ctx,0x280c,~AX211_IRQ_HW_MASK);
    WdfInterruptReleaseLock(ctx->interrupts[0]);
    return STATUS_SUCCESS;
}
static void Ax211DisarmInterrupts(AX211_KM_CONTEXT *ctx)
{
    BOOLEAN locked=ctx->interrupt_count &&
        InterlockedCompareExchange(&ctx->interrupt_connected,0,0)>0;
    if(locked) WdfInterruptAcquireLock(ctx->interrupts[0]);
    Ax211MaskInterrupts(ctx);
    InterlockedExchange(&ctx->interrupt_armed,0);
    if(locked) WdfInterruptReleaseLock(ctx->interrupts[0]);
}
