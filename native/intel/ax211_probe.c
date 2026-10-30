// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <initguid.h>
#include "ax211_ioctl.h"
int wmain(int argc, wchar_t **argv)
{
    HDEVINFO devices;
    SP_DEVICE_INTERFACE_DATA interface_data = {0};
    SP_DEVINFO_DATA device_data = {0};
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = NULL;
    AX211_IDENTITY identity = {0};
    AX211_IRQ_DIAGNOSTICS irq={0};
    BOOL irq_valid=FALSE;
    DWORD irq_error=0;
    ULONG irq_index;
    WCHAR instance[MAX_DEVICE_ID_LEN];
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD needed = 0, returned = 0, error = 0;
    BOOL good, small_rejected = FALSE, input_rejected = FALSE, unknown_rejected = FALSE;
    ULONG junk = 0;
    FILE *out = stdout;
    int result = 1;
    interface_data.cbSize = sizeof(interface_data);
    device_data.cbSize = sizeof(device_data);
    BOOL require_dma = FALSE;
    int arg;
    for (arg = 1; arg < argc; ++arg) {
        if (wcscmp(argv[arg], L"--require-dma") == 0 && !require_dma) {
            require_dma = TRUE;
        } else if (wcscmp(argv[arg], L"--output") == 0 && arg + 1 < argc && out == stdout) {
            if (_wfopen_s(&out, argv[++arg], L"w") != 0) return 2;
        } else {
            fwprintf(stderr, L"Usage: ax211-probe [--require-dma] [--output report.json]\n");
            if (out != stdout) fclose(out);
            return 2;
        }
    }
    devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_OPENDROP_AX211, NULL,
        NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
    if (!SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_OPENDROP_AX211,
                                    0, &interface_data)) { error = GetLastError(); goto done; }
    {
        SP_DEVICE_INTERFACE_DATA extra = {0};
        extra.cbSize = sizeof(extra);
        if (SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_OPENDROP_AX211,
                                       1, &extra)) { error = ERROR_DUP_NAME; goto done; }
        if (GetLastError() != ERROR_NO_MORE_ITEMS) { error = GetLastError(); goto done; }
    }
    SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, NULL, 0, &needed, NULL);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed > 65536) {
        error = ERROR_INVALID_DATA; goto done;
    }
    detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
    if (!detail) { error = ERROR_OUTOFMEMORY; goto done; }
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, needed,
                                         NULL, &device_data) ||
        !SetupDiGetDeviceInstanceIdW(devices, &device_data, instance,
                                    MAX_DEVICE_ID_LEN, NULL)) {
        error = GetLastError(); goto done;
    }
    if (_wcsnicmp(instance, L"PCI\\VEN_8086&DEV_7A70&SUBSYS_00948086",
                  wcslen(L"PCI\\VEN_8086&DEV_7A70&SUBSYS_00948086")) != 0) {
        error = ERROR_NOT_SUPPORTED; goto done;
    }
    handle = CreateFileW(detail->DevicePath, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
    good = DeviceIoControl(handle, IOCTL_AX211_IDENTITY, NULL, 0,
                           &identity, sizeof(identity), &returned, NULL);
    if (!good) { error = GetLastError(); goto done; }
    if (returned != sizeof(identity) || identity.size != sizeof(identity) ||
        identity.abi_version != AX211_ABI_VERSION || !identity.hw_revision ||
        identity.hw_revision == 0xffffffff || !identity.rf_id ||
        identity.rf_id == 0xffffffff) { error = ERROR_INVALID_DATA; goto done; }
    good = DeviceIoControl(handle, IOCTL_AX211_IDENTITY, NULL, 0,
                           &junk, sizeof(junk), &returned, NULL);
    small_rejected = !good && GetLastError() == ERROR_INSUFFICIENT_BUFFER;
    good = DeviceIoControl(handle, IOCTL_AX211_IDENTITY, &junk, sizeof(junk),
                           &identity, sizeof(identity), &returned, NULL);
    input_rejected = !good && GetLastError() == ERROR_INVALID_PARAMETER;
    good = DeviceIoControl(handle,
        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_READ_ACCESS),
        NULL, 0, &junk, sizeof(junk), &returned, NULL);
    unknown_rejected = !good && GetLastError() == ERROR_INVALID_FUNCTION;
    if (!small_rejected || !input_rejected || !unknown_rejected) {
        error = ERROR_INVALID_DATA; goto done;
    }
    if (require_dma && (!(identity.reserved[0] & 1u) || !identity.reserved[1])) {
        error = ERROR_NOT_SUPPORTED; goto done;
    }
    if(identity.reserved[0] & 2u) { error=ERROR_BUSY; goto done; }
    result = 0;
    if(require_dma) {
        if(DeviceIoControl(handle,IOCTL_AX211_IRQ_DIAGNOSTICS,NULL,0,
            &irq,sizeof(irq),&returned,NULL)) {
            irq_valid=returned==sizeof(irq) && irq.size==sizeof(irq) &&
                irq.version==1 && irq.resource_count<=32;
            if(!irq_valid) { irq_error=ERROR_INVALID_DATA;result=1;error=irq_error; }
        } else {
            irq_error=GetLastError();
            result=1;error=irq_error;
        }
    }
done:
    fprintf(out, "{\"ok\":%s,\"win32_error\":%lu,\"abi\":%lu,\"bar_length\":%lu,"
        "\"hw_revision\":%lu,\"rf_id\":%lu,\"small_buffer_rejected\":%s,"
        "\"input_rejected\":%s,\"unknown_ioctl_rejected\":%s,"
        "\"windows_dma_buffer_allocated\":%s,\"dma_buffer_bytes\":%lu,"
        "\"device_reset_types\":%lu,\"firmware_dma_active\":%s,\"dma_hardware_transfer_tested\":false,\"radio_tx_rx_implemented\":false",
        result == 0 ? "true":"false", error, identity.abi_version,
        identity.bar_length, identity.hw_revision, identity.rf_id,
        small_rejected ? "true":"false", input_rejected ? "true":"false",
        unknown_rejected ? "true":"false",
        (identity.reserved[0] & 1u) ? "true":"false", identity.reserved[1], identity.reserved[2],
        (identity.reserved[0] & 2u) ? "true":"false");
    fprintf(out,",\"irq_diagnostics_valid\":%s,\"irq_diagnostics_error\":%lu",
        irq_valid?"true":"false",irq_error);
    if(irq_valid) {
        fprintf(out,",\"irq_setup_status\":%lu,\"raw_resource_count\":%lu,"
            "\"translated_resource_count\":%lu,\"irq_objects\":%lu,\"irq_connected\":%lu,"
            "\"pci_status\":%lu,\"pci_command\":%lu,\"pci_msix\":%lu,\"irq_resources\":[",
            irq.setup_status,irq.raw_count,irq.translated_count,irq.objects_created,
            irq.connected,irq.pci_status,irq.pci_command,irq.pci_msix);
        for(irq_index=0;irq_index<irq.resource_count;++irq_index) {
            const AX211_IRQ_RESOURCE *entry=&irq.resources[irq_index];
            fprintf(out,"%s{\"index\":%lu,\"raw_type\":%lu,\"raw_flags\":%lu,"
                "\"translated_flags\":%lu,\"message_count\":%lu,\"vector\":%lu}",
                irq_index?",":"",entry->index,entry->raw_type,entry->raw_flags,
                entry->translated_flags,entry->message_count,entry->vector);
        }
        fprintf(out,"]");
    }
    fprintf(out,"}\n");
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(detail);
    if (devices != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(devices);
    if (out != stdout) fclose(out);
    return result;
}
