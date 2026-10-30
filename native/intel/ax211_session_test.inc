// SPDX-License-Identifier: GPL-3.0-or-later
#include "awdl_test_frame.inc"

static BOOL SessionStartChecked(HANDLE device,UCHAR *bundle,ULONG length,
    AX211_RADIO_RESULT *radio,DWORD *error)
{
    DWORD returned=0;
    ZeroMemory(radio,sizeof(*radio));
    if(!DeviceIoControl(device,IOCTL_AX211_SESSION_START,bundle,length,
        radio,sizeof(*radio),&returned,NULL)) {*error=GetLastError();return FALSE;}
    if(returned!=sizeof(*radio) || radio->size!=sizeof(*radio) || radio->version!=2 ||
        radio->init.boot.error || radio->init.boot.kernel_status ||
        !radio->init.boot.firmware_alive || !radio->configured ||
        radio->init.boot.buffers_released || radio->tx_queue<1 || radio->tx_queue>31) {
        *error=ERROR_INVALID_DATA;return FALSE;
    }
    return TRUE;
}
static BOOL SessionStopChecked(HANDLE device,AX211_SESSION_STATUS *state,DWORD *error)
{
    DWORD returned=0;
    if(!DeviceIoControl(device,IOCTL_AX211_SESSION_STOP,NULL,0,
        state,sizeof(*state),&returned,NULL)) {*error=GetLastError();return FALSE;}
    if(returned!=sizeof(*state) || state->size!=sizeof(*state) ||
        state->version!=AX211_SESSION_VERSION || state->active ||
        !state->buffers_released || state->kernel_status) {
        *error=ERROR_INVALID_DATA;return FALSE;
    }
    return TRUE;
}
static int SessionTest(const wchar_t *firmware,const wchar_t *pnvm,const wchar_t *report)
{
    UCHAR *fw=NULL,*nv=NULL,*bundle=NULL;
    ULONG fw_bytes=0,nv_bytes=0,total=0;
    AX211_INIT_INPUT *header;
    AX211_RADIO_RESULT radio={0},invalid={0};
    AX211_SESSION_STATUS explicit_stop={0},closed_stop={0},lease_stop={0},final_stop={0};
    AX211_SESSION_INPUT input={0};
    AX211_SESSION_OUTPUT *exchange=NULL;
    HANDLE device=INVALID_HANDLE_VALUE;
    FILE *output=NULL;
    DWORD error=0,returned=0;
    ULONGLONG deadline,next_tx;
    BOOL ok=FALSE,bad_header=FALSE,short_output=FALSE,bad_frame=FALSE;
    BOOL explicit_ok=FALSE,close_ok=FALSE,lease_ok=FALSE;
    if(_wfopen_s(&output,report,L"w")) return 2;
    exchange=(AX211_SESSION_OUTPUT *)calloc(1,sizeof(*exchange));
    if(!exchange) {error=ERROR_OUTOFMEMORY;goto done;}
    if(!ReadBoundedFile(firmware,88,IWL_FW_MAX_BYTES,&fw,&fw_bytes,&error) ||
        !ReadBoundedFile(pnvm,8,IWL_PNVM_MAX_BYTES,&nv,&nv_bytes,&error)) goto done;
    total=(ULONG)sizeof(*header)+fw_bytes+nv_bytes;
    bundle=(UCHAR *)malloc(total);
    if(!bundle) {error=ERROR_OUTOFMEMORY;goto done;}
    header=(AX211_INIT_INPUT *)bundle;
    header->magic=AX211_INIT_MAGIC;header->version=1;
    header->firmware_bytes=fw_bytes;header->pnvm_bytes=nv_bytes;
    memcpy(bundle+sizeof(*header),fw,fw_bytes);
    memcpy(bundle+sizeof(*header)+fw_bytes,nv,nv_bytes);
    device=OpenAx211ForStaging(&error);
    if(device==INVALID_HANDLE_VALUE) goto done;
    short_output=!DeviceIoControl(device,IOCTL_AX211_SESSION_START,bundle,total,
        &invalid,1,&returned,NULL) && GetLastError()==ERROR_INSUFFICIENT_BUFFER;
    header->magic^=1;
    bad_header=DeviceIoControl(device,IOCTL_AX211_SESSION_START,bundle,total,
        &invalid,sizeof(invalid),&returned,NULL) && returned==sizeof(invalid) &&
        invalid.init.boot.error && !invalid.init.boot.dma_started &&
        invalid.init.boot.buffers_released;
    header->magic^=1;
    if(!short_output || !bad_header) {error=ERROR_INVALID_DATA;goto done;}
    if(!SessionStartChecked(device,bundle,total,&radio,&error)) goto done;
    input.size=sizeof(input);input.version=AX211_SESSION_VERSION;
    input.tx_bytes=sizeof(Ax211TestDiscoveryFrame);input.rate_half_mbps=12;input.no_ack=1;
    memcpy(input.frame,Ax211TestDiscoveryFrame,sizeof(Ax211TestDiscoveryFrame));
    input.frame[10]^=2; // Foreign source address must be rejected before submission.
    bad_frame=!DeviceIoControl(device,IOCTL_AX211_SESSION_EXCHANGE,&input,sizeof(input),
        exchange,sizeof(*exchange),&returned,NULL) && GetLastError()==ERROR_INVALID_PARAMETER;
    input.frame[10]^=2;
    if(!bad_frame) {error=ERROR_INVALID_DATA;goto done;}
    deadline=GetTickCount64()+45000u;next_tx=0;
    do {
        input.tx_bytes=(exchange->status.tx_submitted<1024 && GetTickCount64()>=next_tx) ?
            (ULONG)sizeof(Ax211TestDiscoveryFrame) : 0;
        if(!DeviceIoControl(device,IOCTL_AX211_SESSION_EXCHANGE,&input,sizeof(input),
            exchange,sizeof(*exchange),&returned,NULL)) {error=GetLastError();goto done;}
        if(returned!=sizeof(*exchange) || !exchange->status.active ||
            exchange->status.kernel_status || exchange->rx_count>AX211_SESSION_RX_COUNT) {
            error=ERROR_INVALID_DATA;goto done;
        }
        if(exchange->tx_accepted) next_tx=GetTickCount64()+20;
        if(exchange->status.tx_completed>=1024) break;
        Sleep(1);
    } while(GetTickCount64()<deadline);
    if(exchange->status.tx_completed!=1024 || exchange->status.tx_submitted!=1024 ||
        exchange->status.tx_failed || exchange->status.tx_pending ||
        exchange->status.next_tx_index!=((radio.tx_index+1024u)&65535u) || !exchange->status.rx_valid) {
        error=ERROR_INVALID_DATA;goto done;
    }
    explicit_ok=SessionStopChecked(device,&explicit_stop,&error) &&
        explicit_stop.stop_reason==Ax211SessionExplicit;
    if(!explicit_ok) goto done;
    // Start again without rebinding the driver; CloseHandle must synchronously
    // quiesce DMA. Reopening and reading status proves the cleanup callback ran.
    if(!SessionStartChecked(device,bundle,total,&radio,&error)) goto done;
    CloseHandle(device);device=INVALID_HANDLE_VALUE;
    device=OpenAx211ForStaging(&error);
    if(device==INVALID_HANDLE_VALUE) goto done;
    close_ok=SessionStopChecked(device,&closed_stop,&error) &&
        closed_stop.stop_reason==Ax211SessionFileClosed;
    if(!close_ok) goto done;
    if(!SessionStartChecked(device,bundle,total,&radio,&error)) goto done;
    Sleep(AX211_SESSION_LEASE_MS+2000);
    lease_ok=SessionStopChecked(device,&lease_stop,&error) &&
        lease_stop.stop_reason==Ax211SessionLeaseExpired;
    if(!lease_ok) goto done;
    ok=TRUE;
done:
    if(device!=INVALID_HANDLE_VALUE) {
        DWORD cleanup_error=0;
        if(!SessionStopChecked(device,&final_stop,&cleanup_error)) {
            if(!error) error=cleanup_error;
            ok=FALSE;
        }
        CloseHandle(device);
    }
    if(!ok && !error) error=ERROR_INVALID_DATA;
    fprintf(output,"{\"ok\":%s,\"win32_error\":%lu,\"buffers_released\":%s,"
        "\"short_output_rejected\":%s,\"malformed_input_rejected\":%s,"
        "\"foreign_frame_rejected\":%s,\"explicit_stop_passed\":%s,"
        "\"file_cleanup_passed\":%s,\"lease_expiry_passed\":%s",
        ok?"true":"false",error,final_stop.buffers_released?"true":"false",
        short_output?"true":"false",bad_header?"true":"false",bad_frame?"true":"false",
        explicit_ok?"true":"false",close_ok?"true":"false",lease_ok?"true":"false");
    if(exchange) {
#define FIELD(field) fprintf(output,",\"" #field "\":%lu",exchange->status.field)
        FIELD(uptime_ms);FIELD(exchanges);FIELD(tx_submitted);FIELD(tx_completed);
        FIELD(tx_failed);FIELD(last_tx_status);FIELD(next_tx_index);
        FIELD(rx_valid);FIELD(rx_awdl);FIELD(kernel_status);FIELD(stop_reason);
#undef FIELD
    }
    fprintf(output,",\"final_stop_reason\":%lu,\"final_kernel_status\":%lu,"
        "\"start_stage\":%lu,\"start_kernel_status\":%lu,\"iphone_transfer_verified\":false}\n",
        final_stop.stop_reason,final_stop.kernel_status,radio.init.boot.stage,radio.init.boot.kernel_status);
    fclose(output);free(fw);free(nv);free(bundle);free(exchange);
    return ok?0:1;
}
