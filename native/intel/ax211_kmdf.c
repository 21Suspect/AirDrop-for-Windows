// SPDX-License-Identifier: GPL-3.0-or-later
// Development driver: bounded firmware boot, staging, and controller power tests.
// The Windows DMA adapter supplies device addresses; CPU PFNs are never used.
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include "ax211_ioctl.h"
#define AX211_CSR_HW_REV 0x028u
#define AX211_CSR_RF_ID 0x09cu
#define AX211_DMA_BYTES (1024u * 1024u)

struct AX211_BOOT_WORK;
typedef struct AX211_KM_CONTEXT {
    PUCHAR registers;
    ULONG length;
    WDFDMAENABLER dma;
    WDFDEVICE device;
    WDFSPINLOCK interrupt_lock;
    WDFINTERRUPT interrupts[32];
    ULONG interrupt_count;
    AX211_IRQ_DIAGNOSTICS irq_diagnostics;
    AX211_FAULT_DIAGNOSTICS fault;
    volatile LONG interrupt_connected, interrupt_armed;
    volatile LONG interrupt_hits, interrupt_hw, interrupt_fh;
    volatile LONG interrupt_removed;
    ULONG pci_command, pci_msix, interrupt_vector;
    WDFCOMMONBUFFER buffer;
    struct AX211_BOOT_WORK *boot;
    DEVICE_RESET_INTERFACE_STANDARD reset_interface;
    WDFTIMER session_timer;
    BOOLEAN session_requested, ibss_requested;
    ULONGLONG session_deadline, session_started, tx_deadline;
    AX211_RADIO_RESULT session_radio;
    AX211_SESSION_STATUS session_status;
    AX211_SESSION_INPUT session_input; // Buffered IOCTL alias scratch, never on kernel stack.
} AX211_KM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AX211_KM_CONTEXT, Ax211KmContext);
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Ax211KmAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Ax211KmPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Ax211KmRelease;
EVT_WDF_DEVICE_D0_EXIT Ax211KmD0Exit;
EVT_WDF_DEVICE_QUERY_REMOVE Ax211KmQueryRemove;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Ax211KmControl;
EVT_WDF_TIMER Ax211SessionTimer;
EVT_WDF_FILE_CLEANUP Ax211FileCleanup;

#include "ax211_firmware_stage.inc"
#include "ax211_hw.inc"
#include "ax211_interrupt.inc"
#include "ax211_boot.inc"
#include "ax211_init.inc"
#include "ax211_radio.inc"
#include "ax211_session.inc"

NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Ax211KmAdd);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
NTSTATUS Ax211KmAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_DMA_ENABLER_CONFIG dma_config;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_FILEOBJECT_CONFIG files;
    WDF_TIMER_CONFIG timer;
    WDFDEVICE device;
    AX211_KM_CONTEXT *ctx;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = Ax211KmPrepare;
    callbacks.EvtDeviceReleaseHardware = Ax211KmRelease;
    callbacks.EvtDeviceD0Exit = Ax211KmD0Exit;
    callbacks.EvtDeviceQueryRemove = Ax211KmQueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &callbacks);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(init, TRUE);
    WDF_FILEOBJECT_CONFIG_INIT(&files,WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK,Ax211FileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel=WdfExecutionLevelPassive;
    attributes.SynchronizationScope=WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(init,&files,&attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AX211_KM_CONTEXT);
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    ctx = Ax211KmContext(device);
    ctx->device=device;
    WDF_TIMER_CONFIG_INIT(&timer,Ax211SessionTimer);
    timer.AutomaticSerialization=TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject=device;
    attributes.ExecutionLevel=WdfExecutionLevelPassive;
    status=WdfTimerCreate(&timer,&attributes,&ctx->session_timer);
    if(!NT_SUCCESS(status)) return status;
    status=WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES,&ctx->interrupt_lock);
    if(!NT_SUCCESS(status)) return status;
    WdfDeviceSetAlignmentRequirement(device, FILE_512_BYTE_ALIGNMENT);
    WDF_DMA_ENABLER_CONFIG_INIT(&dma_config, WdfDmaProfileScatterGather64Duplex, AX211_DMA_BYTES);
    dma_config.WdmDmaVersionOverride = 3;
    status = WdfDmaEnablerCreate(device, &dma_config, WDF_NO_OBJECT_ATTRIBUTES, &ctx->dma);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_OPENDROP_AX211, NULL);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = Ax211KmControl;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
NTSTATUS Ax211KmPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    AX211_KM_CONTEXT *ctx = Ax211KmContext(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR selected = NULL;
    WDF_COMMON_BUFFER_CONFIG config;
    NTSTATUS status;
    ULONG i;
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type != CmResourceTypeMemory) continue;
        if (selected || r->u.Memory.Length < AX211_CSR_RF_ID + 4u)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        selected = r;
    }
    if (!selected || selected->u.Memory.Length<0x3000u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ctx->length = selected->u.Memory.Length;
    ctx->registers = MmMapIoSpaceEx(selected->u.Memory.Start, ctx->length, PAGE_READWRITE | PAGE_NOCACHE);
    if (!ctx->registers) return STATUS_INSUFFICIENT_RESOURCES;
    WDF_COMMON_BUFFER_CONFIG_INIT(&config, PAGE_SIZE - 1u);
    status = WdfCommonBufferCreateWithConfig(ctx->dma, AX211_DMA_BYTES, &config,
        WDF_NO_OBJECT_ATTRIBUTES, &ctx->buffer);
    if (!NT_SUCCESS(status)) {
        MmUnmapIoSpace(ctx->registers, ctx->length);
        ctx->registers = NULL;
        ctx->length = 0;
        return status;
    }
    RtlZeroMemory(WdfCommonBufferGetAlignedVirtualAddress(ctx->buffer), AX211_DMA_BYTES);
    // Read the bus driver's supported recovery interface; no reset is requested.
    RtlZeroMemory(&ctx->reset_interface,sizeof(ctx->reset_interface));
    status=WdfFdoQueryForInterface(device,&GUID_DEVICE_RESET_INTERFACE_STANDARD,
        (PINTERFACE)&ctx->reset_interface,
        (USHORT)(FIELD_OFFSET(DEVICE_RESET_INTERFACE_STANDARD,Reserved)+sizeof(PVOID)),
        1,NULL);
    if(!NT_SUCCESS(status)) RtlZeroMemory(&ctx->reset_interface,sizeof(ctx->reset_interface));
    // The framework owns interrupt object lifetime and disconnects before release.
    // Reuse objects on a same-layout PnP restart; do not manually delete them.
    status=Ax211CreateInterrupts(ctx,raw,translated);
    ctx->irq_diagnostics.setup_status=(ULONG)status;
    // An unsupported resource layout stays diagnostic-only. No firmware or DMA
    // can start without successfully connected, OS-enabled MSI-X interrupts.
    if(!NT_SUCCESS(status) && status!=STATUS_NOT_SUPPORTED) return status;
    Ax211MaskInterrupts(ctx);
    return STATUS_SUCCESS;
}
NTSTATUS Ax211KmQueryRemove(WDFDEVICE device)
{
    AX211_KM_CONTEXT *ctx=Ax211KmContext(device);
    NTSTATUS status;
    WdfObjectAcquireLock(device);
    status=ctx->boot && ctx->boot->armed ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfObjectReleaseLock(device);
    return status;
}
NTSTATUS Ax211KmD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    AX211_KM_CONTEXT *ctx=Ax211KmContext(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(target);
    WdfObjectAcquireLock(device);
    status=Ax211SessionStop(ctx,Ax211SessionPowerDown);
    WdfObjectReleaseLock(device);
    return status;
}
NTSTATUS Ax211KmRelease(WDFDEVICE device, WDFCMRESLIST translated)
{
    AX211_KM_CONTEXT *ctx = Ax211KmContext(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(translated);
    WdfObjectAcquireLock(device);
    status=Ax211SessionStop(ctx,Ax211SessionPowerDown);
    if(!NT_SUCCESS(status)) {WdfObjectReleaseLock(device);return status;}
    if (ctx->reset_interface.InterfaceDereference) {
        ctx->reset_interface.InterfaceDereference(ctx->reset_interface.Context);
        RtlZeroMemory(&ctx->reset_interface,sizeof(ctx->reset_interface));
    }
    // Firmware DMA has been confirmed stopped before releasing its adapter.
    if (ctx->buffer) { WdfObjectDelete(ctx->buffer); ctx->buffer = NULL; }
    if (ctx->registers) {
        MmUnmapIoSpace(ctx->registers, ctx->length);
        ctx->registers = NULL;
    }
    ctx->length = 0;
    WdfObjectReleaseLock(device);
    return STATUS_SUCCESS;
}
VOID Ax211KmControl(WDFQUEUE queue, WDFREQUEST request, size_t out_size,
                    size_t in_size, ULONG code)
{
    AX211_KM_CONTEXT *ctx = Ax211KmContext(WdfIoQueueGetDevice(queue));
    AX211_IDENTITY *identity;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(out_size);
    if(code==IOCTL_AX211_SESSION_STOP || code==IOCTL_AX211_SESSION_EXCHANGE) {
        Ax211SessionControl(ctx,request,in_size,code);return;
    }
    if(code==IOCTL_AX211_FAULT_DIAGNOSTICS) {
        AX211_FAULT_DIAGNOSTICS *output;
        if(in_size) { WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return; }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*output),(PVOID *)&output,NULL);
        if(!NT_SUCCESS(status)) { WdfRequestComplete(request,status);return; }
        ctx->fault.size=sizeof(ctx->fault);ctx->fault.version=1;
        RtlCopyMemory(output,&ctx->fault,sizeof(*output));
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*output));return;
    }
    if (ctx->boot && code!=IOCTL_AX211_IDENTITY) {
        WdfRequestComplete(request,STATUS_DEVICE_BUSY); return;
    }
    if(code==IOCTL_AX211_IRQ_DIAGNOSTICS) {
        AX211_IRQ_DIAGNOSTICS *output;
        if(in_size) { WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return; }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*output),(PVOID *)&output,NULL);
        if(!NT_SUCCESS(status)) { WdfRequestComplete(request,status);return; }
        ctx->irq_diagnostics.pci_status=(ULONG)Ax211ReadPciState(ctx);
        ctx->irq_diagnostics.pci_command=ctx->pci_command;
        ctx->irq_diagnostics.pci_msix=ctx->pci_msix;
        ctx->irq_diagnostics.objects_created=ctx->interrupt_count;
        ctx->irq_diagnostics.connected=(ULONG)InterlockedCompareExchange(&ctx->interrupt_connected,0,0);
        RtlCopyMemory(output,&ctx->irq_diagnostics,sizeof(*output));
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*output));return;
    }
    if(code==IOCTL_AX211_MONITOR_TEST || code==IOCTL_AX211_TRANSMIT_TEST ||
        code==IOCTL_AX211_SESSION_START || code==IOCTL_AX211_IBSS_START) {
        const UCHAR *input;
        AX211_RADIO_RESULT summary,*output;
        SIZE_T actual;
        if(!ctx->registers || ctx->length<0x3000u || !ctx->dma) {
            WdfRequestComplete(request,STATUS_DEVICE_NOT_READY);return;
        }
        if(in_size<sizeof(AX211_INIT_INPUT) ||
            in_size>sizeof(AX211_INIT_INPUT)+IWL_FW_MAX_BYTES+IWL_PNVM_MAX_BYTES) {
            WdfRequestComplete(request,STATUS_INVALID_BUFFER_SIZE);return;
        }
        status=WdfRequestRetrieveInputBuffer(request,sizeof(AX211_INIT_INPUT),(PVOID *)&input,&actual);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(summary),(PVOID *)&output,NULL);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        RtlZeroMemory(&summary,sizeof(summary));summary.size=sizeof(summary);summary.version=2;
        summary.tx_requested=code!=IOCTL_AX211_MONITOR_TEST;
        ctx->ibss_requested=code==IOCTL_AX211_IBSS_START;
        ctx->session_requested=code==IOCTL_AX211_SESSION_START || ctx->ibss_requested;
        Ax211InitializeFirmware(ctx,input,actual,&summary.init,&summary);
        if(ctx->session_requested && !summary.init.boot.error)
            Ax211SessionBegin(ctx,&summary);
        ctx->session_requested=FALSE;ctx->ibss_requested=FALSE;
        RtlCopyMemory(output,&summary,sizeof(summary));
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(summary));return;
    }
    if(code==IOCTL_AX211_INIT_FIRMWARE) {
        const UCHAR *input;
        AX211_INIT_RESULT summary,*output;
        SIZE_T actual;
        if(!ctx->registers || ctx->length<0x3000u || !ctx->dma) {
            WdfRequestComplete(request,STATUS_DEVICE_NOT_READY);return;
        }
        if(in_size<sizeof(AX211_INIT_INPUT) ||
            in_size>sizeof(AX211_INIT_INPUT)+IWL_FW_MAX_BYTES+IWL_PNVM_MAX_BYTES) {
            WdfRequestComplete(request,STATUS_INVALID_BUFFER_SIZE);return;
        }
        status=WdfRequestRetrieveInputBuffer(request,sizeof(AX211_INIT_INPUT),(PVOID *)&input,&actual);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(summary),(PVOID *)&output,NULL);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        Ax211InitializeFirmware(ctx,input,actual,&summary,NULL);
        RtlCopyMemory(output,&summary,sizeof(summary));
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(summary));return;
    }
    if (code == IOCTL_AX211_BOOT_FIRMWARE) {
        const UCHAR *input;
        AX211_BOOT_RESULT summary, *output;
        SIZE_T actual;
        if(!ctx->registers || ctx->length<0x3000u || !ctx->dma) {
            WdfRequestComplete(request,STATUS_DEVICE_NOT_READY); return;
        }
        if(in_size<88 || in_size>IWL_FW_MAX_BYTES) {
            WdfRequestComplete(request,STATUS_INVALID_BUFFER_SIZE); return;
        }
        status=WdfRequestRetrieveInputBuffer(request,88,(PVOID *)&input,&actual);
        if(!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(summary),(PVOID *)&output,NULL);
        if(!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
        Ax211BootFirmware(ctx,input,actual,&summary);
        // Buffered input/output alias; all image reads have finished.
        RtlCopyMemory(output,&summary,sizeof(summary));
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(summary)); return;
    }
    if (code == IOCTL_AX211_POWER_CYCLE) {
        IWL_HW_RESULT *result;
        if (in_size) { WdfRequestComplete(request,STATUS_INVALID_PARAMETER); return; }
        if (!ctx->registers || ctx->length<0x3000u) {
            WdfRequestComplete(request,STATUS_DEVICE_NOT_READY); return;
        }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*result),(PVOID *)&result,NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
        Ax211HwPowerCycle(ctx,result);
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*result));
        return;
    }
    if (code == IOCTL_AX211_STAGE_FIRMWARE) {
        const UCHAR *input;
        AX211_FIRMWARE_RESULT summary, *output;
        SIZE_T actual;
        if (!ctx->registers || !ctx->dma || !ctx->buffer) {
            WdfRequestComplete(request, STATUS_DEVICE_NOT_READY); return;
        }
        if (in_size > IWL_FW_MAX_BYTES || in_size < 88) {
            WdfRequestComplete(request, STATUS_INVALID_BUFFER_SIZE); return;
        }
        status=WdfRequestRetrieveInputBuffer(request,88,(PVOID *)&input,&actual);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(summary),(PVOID *)&output,NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
        status=Ax211StageFirmware(ctx,input,actual,&summary);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request,status); return; }
        // METHOD_BUFFERED input and output may alias. Input is no longer in use.
        RtlCopyMemory(output,&summary,sizeof(summary));
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(summary));
        return;
    }
    if (code != IOCTL_AX211_IDENTITY) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (in_size != 0) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*identity), (PVOID *)&identity, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    if (!ctx->registers || !ctx->buffer) {
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY); return;
    }
    RtlZeroMemory(identity, sizeof(*identity));
    identity->size = sizeof(*identity);
    identity->abi_version = AX211_ABI_VERSION;
    identity->bar_length = ctx->length;
    identity->hw_revision = READ_REGISTER_ULONG((PULONG)(ctx->registers + AX211_CSR_HW_REV));
    identity->rf_id = READ_REGISTER_ULONG((PULONG)(ctx->registers + AX211_CSR_RF_ID));
    identity->reserved[0] = 1u | ((ctx->boot && ctx->boot->armed) ? 2u : 0u);
    identity->reserved[1] = AX211_DMA_BYTES;
    identity->reserved[2] = ctx->reset_interface.DeviceReset ? ctx->reset_interface.SupportedResetTypes : 0;
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*identity));
}
