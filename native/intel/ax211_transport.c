// SPDX-License-Identifier: GPL-3.0-or-later
// Narrow user-mode ABI shared by the Rust worker and the KMDF wire definitions.
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdlib.h>
#include <wchar.h>
#include <initguid.h>
#include "ax211_ioctl.h"
#include "iwl_firmware.h"
#include "iwl_pnvm.h"
#include "ax211_open.inc"

C_ASSERT(sizeof(AX211_SESSION_STATUS)==84);
C_ASSERT(sizeof(AX211_SESSION_INPUT)==2324);
C_ASSERT(sizeof(AX211_SESSION_OUTPUT)==32348);

DWORD od_ax211_open(HANDLE *out)
{
    DWORD error=0;
    if(!out) return ERROR_INVALID_PARAMETER;
    *out=OpenAx211ForStaging(&error);
    return *out==INVALID_HANDLE_VALUE ? error : ERROR_SUCCESS;
}
DWORD od_ax211_exchange(HANDLE device,const AX211_SESSION_INPUT *input,
    AX211_SESSION_OUTPUT *output)
{
    DWORD returned=0;
    if(!input || !output) return ERROR_INVALID_PARAMETER;
    if(!DeviceIoControl(device,IOCTL_AX211_SESSION_EXCHANGE,(void *)input,sizeof(*input),
        output,sizeof(*output),&returned,NULL)) return GetLastError();
    if(returned!=sizeof(*output) || output->status.size!=sizeof(output->status) ||
        output->status.version!=AX211_SESSION_VERSION ||
        output->rx_count>AX211_SESSION_RX_COUNT || output->tx_accepted>1)
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
DWORD od_ax211_stop(HANDLE device,AX211_SESSION_STATUS *state)
{
    DWORD returned=0;
    if(!state) return ERROR_INVALID_PARAMETER;
    if(!DeviceIoControl(device,IOCTL_AX211_SESSION_STOP,NULL,0,
        state,sizeof(*state),&returned,NULL)) return GetLastError();
    if(returned!=sizeof(*state) || state->size!=sizeof(*state) ||
        state->version!=AX211_SESSION_VERSION) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
DWORD od_ax211_start(HANDLE device,const UCHAR *firmware,ULONG fw_bytes,
    const UCHAR *pnvm,ULONG nv_bytes,ULONG ibss,AX211_SESSION_STATUS *state,ULONG startup[6])
{
    AX211_INIT_INPUT *bundle;
    AX211_RADIO_RESULT radio={0};
    AX211_SESSION_INPUT input={0};
    AX211_SESSION_OUTPUT *output;
    DWORD returned=0,error=0,total;
    if(!startup || ibss>1 || !state || !firmware || !pnvm || fw_bytes<88 || fw_bytes>IWL_FW_MAX_BYTES ||
        nv_bytes<8 || nv_bytes>IWL_PNVM_MAX_BYTES) return ERROR_INVALID_PARAMETER;
    ZeroMemory(state,sizeof(*state));ZeroMemory(startup,6*sizeof(ULONG));
    total=(DWORD)sizeof(*bundle)+fw_bytes+nv_bytes;
    bundle=(AX211_INIT_INPUT *)malloc(total);
    output=(AX211_SESSION_OUTPUT *)calloc(1,sizeof(*output));
    if(!bundle || !output) {free(bundle);free(output);return ERROR_OUTOFMEMORY;}
    bundle->magic=AX211_INIT_MAGIC;bundle->version=1;
    bundle->firmware_bytes=fw_bytes;bundle->pnvm_bytes=nv_bytes;
    memcpy(bundle+1,firmware,fw_bytes);memcpy((UCHAR *)(bundle+1)+fw_bytes,pnvm,nv_bytes);
    if(!DeviceIoControl(device,ibss?IOCTL_AX211_IBSS_START:IOCTL_AX211_SESSION_START,bundle,total,
        &radio,sizeof(radio),&returned,NULL)) {error=GetLastError();goto done;}
    state->kernel_status=radio.init.boot.kernel_status;
    startup[0]=radio.init.boot.stage;startup[1]=radio.command_id;startup[2]=radio.command_status;
    startup[3]=radio.init.boot.last_packet_code;startup[4]=radio.init.boot.msix_causes;
    startup[5]=radio.init.boot.fh_causes;
    if(returned!=sizeof(radio) || radio.size!=sizeof(radio) || radio.version!=2 ||
        radio.init.boot.error || radio.init.boot.kernel_status ||
        !radio.init.boot.firmware_alive || radio.init.boot.buffers_released ||
        !radio.configured || radio.tx_queue<1 || radio.tx_queue>31) {
        error=ERROR_GEN_FAILURE;goto done;
    }
    input.size=sizeof(input);input.version=AX211_SESSION_VERSION;
    error=od_ax211_exchange(device,&input,output);
    *state=output->status;
    if(!error && (!state->active || state->kernel_status || state->ibss_configured!=ibss))
        error=ERROR_GEN_FAILURE;
done:
    free(bundle);free(output);return error;
}

DWORD od_ax211_fault(HANDLE device,AX211_FAULT_DIAGNOSTICS *fault)
{
    DWORD returned=0;
    C_ASSERT(sizeof(AX211_FAULT_DIAGNOSTICS)==236);
    if(!fault) return ERROR_INVALID_PARAMETER;
    ZeroMemory(fault,sizeof(*fault));
    if(!DeviceIoControl(device,IOCTL_AX211_FAULT_DIAGNOSTICS,NULL,0,
        fault,sizeof(*fault),&returned,NULL)) return GetLastError();
    if(returned!=sizeof(*fault) || fault->size!=sizeof(*fault) || fault->version!=1)
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
