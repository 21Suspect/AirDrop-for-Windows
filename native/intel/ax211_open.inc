// SPDX-License-Identifier: GPL-3.0-or-later
static HANDLE OpenAx211ForStaging(DWORD *last_error)
{
    HDEVINFO devices;
    SP_DEVICE_INTERFACE_DATA interface_data={0};
    SP_DEVINFO_DATA device_data={0};
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail=NULL;
    WCHAR instance[MAX_DEVICE_ID_LEN];
    HANDLE handle=INVALID_HANDLE_VALUE;
    DWORD needed=0,error=0;
    interface_data.cbSize=sizeof(interface_data);
    device_data.cbSize=sizeof(device_data);
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
    handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
done:
    free(detail);
    if(devices!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(devices);
    *last_error=error;
    return handle;
}
