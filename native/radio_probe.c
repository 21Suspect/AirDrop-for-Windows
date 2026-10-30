/* SPDX-License-Identifier: GPL-3.0-or-later
 * Query Windows Native Wi-Fi; optional reversible monitor-mode test.
 * No driver installation, IHV commands, register access or packet injection.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wlanapi.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

static void json_string(FILE *out, const wchar_t *value) {
    char utf8[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), NULL, NULL);
    if (!n) { fputs("null", out); return; }
    fputc('"', out);
    for (int i = 0; i < n - 1; ++i) {
        unsigned char c = (unsigned char)utf8[i];
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c < 32) fprintf(out, "\\u%04x", (unsigned int)c);
        else fputc(c, out);
    }
    fputc('"', out);
}
static BOOL is_admin(void) {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = NULL;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0,&group)) {
        CheckTokenMembership(NULL, group, &member);
        FreeSid(group);
    }
    return member;
}
static DWORD get_mode(HANDLE handle, const GUID *guid, ULONG *mode) {
    DWORD size = 0;
    PVOID value = NULL;
    WLAN_OPCODE_VALUE_TYPE source;
    DWORD result = WlanQueryInterface(handle, guid,
        wlan_intf_opcode_current_operation_mode, NULL, &size, &value, &source);
    if (!result) {
        if (size < sizeof(ULONG) || !value) result = ERROR_INVALID_DATA;
        else *mode = *(ULONG *)value;
    }
    if (value) WlanFreeMemory(value);
    return result;
}
int wmain(int argc, wchar_t **argv) {
    BOOL probe = FALSE;
    const wchar_t *output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--probe-monitor")) probe = TRUE;
        else if (!wcscmp(argv[i], L"--output") && i+1 < argc) output = argv[++i];
        else {
            fputs("Usage: radio-probe [--probe-monitor] [--output PATH]\n"
                  "Monitor test only targets a disconnected Intel AX211.\n", stderr);
            return 2;
        }
    }
    FILE *out = output ? _wfopen(output, L"wb") : stdout;
    if (!out) { fputs("Cannot open output file.\n", stderr); return 2; }
    HANDLE handle = NULL;
    DWORD negotiated = 0;
    DWORD result = WlanOpenHandle(2, NULL, &negotiated, &handle);
    fprintf(out, "{\"schema\":1,\"elevated\":%s,\"open_error\":%lu,\"interfaces\":[",
        is_admin() ? "true" : "false", result);
    PWLAN_INTERFACE_INFO_LIST list = NULL;
    int exit_code = 0;
    DWORD enumeration_error = result;
    if (!result) enumeration_error = WlanEnumInterfaces(handle, NULL, &list);
    if (!enumeration_error && list) {
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            WLAN_INTERFACE_INFO *item = &list->InterfaceInfo[i];
            ULONG before = 0, after = 0;
            DWORD query_error = get_mode(handle, &item->InterfaceGuid, &before);
            DWORD set_error = ERROR_SUCCESS, restore_error = ERROR_SUCCESS;
            const char *status = "not_requested";
            if (probe) {
                if (!wcsstr(item->strInterfaceDescription, L"AX211"))
                    status = "not_target";
                else if (item->isState != wlan_interface_state_disconnected)
                    status = "skipped_not_disconnected";
                else if (query_error)
                    status = "skipped_query_failed";
                else if (before == 0x80000000UL)
                    status = "already_monitor";
                else {
                    ULONG monitor = 0x80000000UL; /* windot11.h */
                    set_error = WlanSetInterface(handle, &item->InterfaceGuid,
                        wlan_intf_opcode_current_operation_mode,
                        sizeof(monitor), &monitor, NULL);
                    status = set_error ? "monitor_rejected" : "monitor_accepted";
                    if (!set_error) {
                        restore_error = WlanSetInterface(handle, &item->InterfaceGuid,
                            wlan_intf_opcode_current_operation_mode,
                            sizeof(before), &before, NULL);
                        if (restore_error) { status = "RESTORE_FAILED"; exit_code = 3; }
                    }
                }
            }
            DWORD after_error = get_mode(handle, &item->InterfaceGuid, &after);
            if (!strcmp(status, "monitor_accepted") && (after_error || after != before)) {
                status = "RESTORE_UNVERIFIED";
                exit_code = 3;
            }
            if (i) fputc(',', out);
            fputs("{\"description\":", out);
            json_string(out, item->strInterfaceDescription);
            fprintf(out, ",\"interface_state\":%d,\"query_error\":%lu,"
                "\"mode_before\":%lu,\"probe_status\":\"%s\","
                "\"set_monitor_error\":%lu,\"restore_error\":%lu,"
                "\"after_query_error\":%lu,\"mode_after\":%lu}",
                (int)item->isState, query_error, before, status,
                set_error, restore_error, after_error, after);
        }
    }
    fprintf(out, "],\"enumeration_error\":%lu}\n", enumeration_error);
    if (list) WlanFreeMemory(list);
    if (handle) WlanCloseHandle(handle, NULL);
    if (output) fclose(out);
    return enumeration_error ? 2 : exit_code;
}
