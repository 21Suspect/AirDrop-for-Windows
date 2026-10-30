// SPDX-License-Identifier: GPL-3.0-or-later
// Only immutable identity CSRs. No arbitrary addresses, writes, firmware, DMA or radio.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wdf.h>
#include <initguid.h>
#include "ax211_ioctl.h"
// CSR addresses from OpenBSD if_iwxreg.h; see PROVENANCE.md.
#define AX211_CSR_HW_REV 0x028u
#define AX211_CSR_RF_ID  0x09cu
typedef struct AX211_CONTEXT { PVOID registers; ULONG length; } AX211_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AX211_CONTEXT, Ax211Context);
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Ax211Add;
EVT_WDF_DEVICE_PREPARE_HARDWARE Ax211Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Ax211Release;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Ax211Control;

NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Ax211Add);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
NTSTATUS Ax211Add(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = Ax211Prepare;
    callbacks.EvtDeviceReleaseHardware = Ax211Release;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &callbacks);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AX211_CONTEXT);
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_OPENDROP_AX211, NULL);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = Ax211Control;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
NTSTATUS Ax211Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    AX211_CONTEXT *ctx = Ax211Context(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR selected = NULL;
    ULONG i;
    UNREFERENCED_PARAMETER(raw);
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = WdfCmResourceListGetDescriptor(translated, i);
        if (!resource) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (resource->Type != CmResourceTypeMemory) continue;
        if (selected || resource->u.Memory.Length < AX211_CSR_RF_ID + 4u)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        selected = resource;
    }
    if (!selected) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ctx->length = selected->u.Memory.Length;
    return WdfDeviceMapIoSpace(device, selected->u.Memory.Start, ctx->length,
                              MmNonCached, &ctx->registers);
}
NTSTATUS Ax211Release(WDFDEVICE device, WDFCMRESLIST translated)
{
    AX211_CONTEXT *ctx = Ax211Context(device);
    UNREFERENCED_PARAMETER(translated);
    if (ctx->registers) {
        WdfDeviceUnmapIoSpace(device, ctx->registers, ctx->length);
        ctx->registers = NULL;
    }
    ctx->length = 0;
    return STATUS_SUCCESS;
}
VOID Ax211Control(WDFQUEUE queue, WDFREQUEST request, size_t output_length,
                  size_t input_length, ULONG code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    AX211_CONTEXT *ctx = Ax211Context(device);
    AX211_IDENTITY *identity = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(output_length);
    if (code != IOCTL_AX211_IDENTITY) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (input_length != 0) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
    }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*identity), (PVOID *)&identity, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    if (!ctx->registers || ctx->length < AX211_CSR_RF_ID + 4u) {
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY); return;
    }
    ZeroMemory(identity, sizeof(*identity));
    identity->size = sizeof(*identity);
    identity->abi_version = AX211_ABI_VERSION;
    identity->bar_length = ctx->length;
    identity->hw_revision = WDF_READ_REGISTER_ULONG(device,
        (PULONG)((PUCHAR)ctx->registers + AX211_CSR_HW_REV));
    identity->rf_id = WDF_READ_REGISTER_ULONG(device,
        (PULONG)((PUCHAR)ctx->registers + AX211_CSR_RF_ID));
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*identity));
}
