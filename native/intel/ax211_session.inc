// SPDX-License-Identifier: GPL-3.0-or-later
// Exclusive channel-6 transport. Device callback lock serializes all DMA access.
// The ISR never accesses session data. One TX slot stays immutable until ACKed
// by a firmware completion (which is distinct from a peer's wireless ACK).
static const UCHAR Ax211SessionAddress[6]={2,0xad,0x21,0,0,1};

static void Ax211SessionStatus(AX211_KM_CONTEXT *ctx)
{
    AX211_SESSION_STATUS *s=&ctx->session_status;
    ULONGLONG ms;
    s->size=sizeof(*s);s->version=AX211_SESSION_VERSION;
    s->lease_ms=AX211_SESSION_LEASE_MS;s->buffers_released=ctx->boot==NULL;
    if(s->active) {
        ms=(Ax211HwNow(ctx)-ctx->session_started)/1000u;
        s->uptime_ms=ms>MAXULONG ? MAXULONG : (ULONG)ms;
    }
}
static NTSTATUS Ax211SessionStop(AX211_KM_CONTEXT *ctx,ULONG reason)
{
    NTSTATUS status;
    AX211_SESSION_STATUS *s=&ctx->session_status;
    Ax211SessionStatus(ctx);
    if(s->active) s->stop_reason=reason;
    s->active=0;
    // All Stop calls hold the device lock. Never wait for a timer callback
    // that needs this same lock. A queued callback sees active==0 and exits.
    if(ctx->session_timer) WdfTimerStop(ctx->session_timer,FALSE);
    status=Ax211BootQuiesce(ctx,&ctx->session_radio.init.boot);
    if(NT_SUCCESS(status)) Ax211BootFree(ctx);
    else s->kernel_status=(ULONG)status; // Preserve DMA buffers on failure.
    s->tx_pending=ctx->boot && ctx->boot->armed ? s->tx_pending : 0;
    Ax211SessionStatus(ctx);
    return status;
}
static void Ax211SessionRefresh(AX211_KM_CONTEXT *ctx)
{
    ctx->session_deadline=Ax211HwNow(ctx)+(ULONGLONG)AX211_SESSION_LEASE_MS*1000u;
    WdfTimerStart(ctx->session_timer,WDF_REL_TIMEOUT_IN_MS(AX211_SESSION_LEASE_MS));
}
static void Ax211SessionBegin(AX211_KM_CONTEXT *ctx,const AX211_RADIO_RESULT *radio)
{
    ctx->session_radio=*radio;
    RtlZeroMemory(&ctx->session_status,sizeof(ctx->session_status));
    ctx->session_status.active=1;
    ctx->session_status.ibss_configured=ctx->ibss_requested;
    ctx->session_status.next_tx_index=radio->tx_index;
    ctx->session_started=Ax211HwNow(ctx);
    Ax211SessionStatus(ctx);
    Ax211SessionRefresh(ctx);
}
VOID Ax211SessionTimer(WDFTIMER timer)
{
    AX211_KM_CONTEXT *ctx=Ax211KmContext(WdfTimerGetParentObject(timer));
    ULONGLONG now;
    if(!ctx->session_status.active) return;
    now=Ax211HwNow(ctx);
    if(now>=ctx->session_deadline) {
        (void)Ax211SessionStop(ctx,Ax211SessionLeaseExpired);
    } else {
        // A previous expiry can have been queued before an exchange renewed it.
        LONGLONG remaining=(LONGLONG)(ctx->session_deadline-now);
        WdfTimerStart(timer,-remaining*10);
    }
}
VOID Ax211FileCleanup(WDFFILEOBJECT file)
{
    WDFDEVICE device=WdfFileObjectGetDevice(file);
    AX211_KM_CONTEXT *ctx=Ax211KmContext(device);
    WdfObjectAcquireLock(device);
    (void)Ax211SessionStop(ctx,Ax211SessionFileClosed);
    WdfObjectReleaseLock(device);
}
static NTSTATUS Ax211SessionDrain(AX211_KM_CONTEXT *ctx,AX211_SESSION_OUTPUT *out)
{
    AX211_SESSION_STATUS *s=&ctx->session_status;
    AX211_BOOT_WORK *work=ctx->boot;
    AX211_BOOT_RESULT *boot=&ctx->session_radio.init.boot;
    NTSTATUS status=Ax211BootCauses(ctx,boot);
    ULONG n;
    if(!NT_SUCCESS(status)) return status;
    for(n=0;n<128 && out->rx_count<AX211_SESSION_RX_COUNT;++n) {
        IWL_RESPONSE_INFO *response=&work->response_info;
        status=Ax211BootNextResponse(ctx,boot);
        if(status==STATUS_NO_MORE_ENTRIES) break;
        if(!NT_SUCCESS(status)) return status;
        if(response->opcode==2 && response->group<=1)
            return STATUS_DEVICE_HARDWARE_ERROR;
        if(s->ibss_configured && response->opcode==0x90 && response->group<=1 &&
            (response->queue&0x80u)) {
            // API bit 46: short beacon notification v6 (status, TSF, manager, GP2).
            if(response->payload_bytes<20) return STATUS_DATA_ERROR;
            s->beacons_completed++;s->last_beacon_status=Ax211ReadLe32(work->response+8)&0xffu;
        } else if(response->opcode==0x1c && response->group<=1) {
            IWL_TX_COMPLETION_INFO completion;
            if(!s->tx_pending || (response->queue&0x7fu)!=work->pending_tx_queue ||
                response->index!=(work->pending_tx_index&255u) ||
                iwl_radio_tx_completion(work->response+8,response->payload_bytes,
                    work->pending_tx_queue,work->pending_tx_index,&completion))
                return STATUS_DATA_ERROR;
            s->tx_pending=0;s->tx_completed++;s->last_tx_status=completion.status;
            if(!completion.success) s->tx_failed++;
            if(work->pending_tx_queue==ctx->session_radio.tx_queue)
                ctx->session_radio.tx_index=completion.next_index;
            else if(s->ibss_configured && work->pending_tx_queue==work->ibss_data_queue)
                work->ibss_data_index=completion.next_index;
            else return STATUS_DATA_ERROR;
            s->next_tx_index=completion.next_index;
        } else if(response->opcode==0xc1 && response->group<=1 &&
            (response->queue&0x80u)) {
            IWL_RX_FRAME_INFO info;
            AX211_SESSION_FRAME *frame;
            int decoded=iwl_rx_frame_decode(work->response+8,response->payload_bytes,
                work->decoded_frame,sizeof(work->decoded_frame),&info);
            if(decoded!=IwlPacketOk || info.channel!=6) continue;
            s->rx_valid++;
            if(iwl_awdl_frame_validate(work->decoded_frame,info.frame_bytes,
                Ax211SessionAddress,0)!=IwlPacketOk) continue;
            s->rx_awdl++;
            frame=&out->received[out->rx_count++];
            frame->bytes=info.frame_bytes;frame->rssi_dbm=info.rssi_dbm;
            RtlCopyMemory(frame->frame,work->decoded_frame,info.frame_bytes);
        }
    }
    if(s->tx_pending && Ax211HwNow(ctx)>=ctx->tx_deadline) return STATUS_IO_TIMEOUT;
    return STATUS_SUCCESS;
}
static NTSTATUS Ax211SessionSubmit(AX211_KM_CONTEXT *ctx,AX211_SESSION_OUTPUT *out)
{
    AX211_SESSION_INPUT *input=&ctx->session_input;
    AX211_SESSION_STATUS *s=&ctx->session_status;
    AX211_BOOT_WORK *work=ctx->boot;
    AX211_RADIO_RESULT *radio=&ctx->session_radio;
    IWL_TX_FRAME_REQUEST request;
    IWL_TX_FRAME_INFO info;
    PUCHAR descriptors,counts,slot;
    ULONGLONG address;
    ULONG queue,index;
    BOOLEAN data_queue;
    if(!input->tx_bytes || s->tx_pending) return STATUS_SUCCESS;
    RtlZeroMemory(&request,sizeof(request));
    data_queue=s->ibss_configured && (input->frame[0]&0x0cu)==8u;
    queue=data_queue ? work->ibss_data_queue : radio->tx_queue;
    index=data_queue ? work->ibss_data_index : radio->tx_index;
    if(!queue || queue>31 || index>65535u ||
        (data_queue && (!work->ibss_data_tfd || !work->ibss_data_count)))
        return STATUS_INVALID_DEVICE_STATE;
    request.queue=queue;request.index=index&255u;
    request.rate_half_mbps=input->rate_half_mbps;request.no_ack=input->no_ack;
    request.antenna=(radio->init.tx_chains&1u)?1u:2u;
    request.command_version=10;request.notification_version=7;
    descriptors=(PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(
        data_queue ? work->ibss_data_tfd : work->tx_tfd_buffer);
    counts=(PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(
        data_queue ? work->ibss_data_count : work->tx_count_buffer);
    slot=(PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(work->tx_frame_buffer);
    address=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->tx_frame_buffer).QuadPart;
    if(iwl_tx_frame_build(&request,input->frame,input->tx_bytes,address,
        slot,4096,descriptors+(index&255u)*256u,256,&info)!=IwlPacketOk)
        return STATUS_INVALID_PARAMETER;
    counts[(index&255u)*2]=(UCHAR)info.byte_count;
    counts[(index&255u)*2+1]=(UCHAR)(info.byte_count>>8);
    KeMemoryBarrier();
    work->pending_tx_queue=queue;work->pending_tx_index=index;
    s->next_tx_index=index;
    s->tx_pending=1;s->tx_submitted++;out->tx_accepted=1;
    ctx->tx_deadline=Ax211HwNow(ctx)+2000000u;
    Ax211HwWrite(ctx,0x460,(queue<<16)|((index+1u)&65535u));
    return STATUS_SUCCESS;
}
static void Ax211SessionControl(AX211_KM_CONTEXT *ctx,WDFREQUEST request,
    SIZE_T in_size,ULONG code)
{
    NTSTATUS status;
    if(code==IOCTL_AX211_SESSION_STOP) {
        AX211_SESSION_STATUS *out;
        if(in_size) {WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*out),(PVOID *)&out,NULL);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        (void)Ax211SessionStop(ctx,Ax211SessionExplicit);
        *out=ctx->session_status;
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*out));return;
    } else {
        const AX211_SESSION_INPUT *input;
        AX211_SESSION_OUTPUT *out;
        ULONG rate;
        if(in_size!=sizeof(*input)) {WdfRequestComplete(request,STATUS_INVALID_BUFFER_SIZE);return;}
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*input),(PVOID *)&input,NULL);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*out),(PVOID *)&out,NULL);
        if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
        rate=input->rate_half_mbps;
        if(input->size!=sizeof(*input) || input->version!=AX211_SESSION_VERSION ||
            input->tx_bytes>AX211_SESSION_TX_MAX || input->no_ack>1 ||
            (input->tx_bytes && rate!=12 && rate!=18 && rate!=24 && rate!=36 &&
                rate!=48 && rate!=72 && rate!=96 && rate!=108) ||
            (input->tx_bytes && iwl_awdl_frame_validate(input->frame,input->tx_bytes,
                Ax211SessionAddress,1)!=IwlPacketOk)) {
            WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;
        }
        // METHOD_BUFFERED input/output can alias. Save the small input in the
        // device context before clearing output; neither large struct is stacked.
        RtlCopyMemory(&ctx->session_input,input,sizeof(*input));
        RtlZeroMemory(out,sizeof(*out));
        if(ctx->session_status.active && Ax211HwNow(ctx)>=ctx->session_deadline)
            (void)Ax211SessionStop(ctx,Ax211SessionLeaseExpired);
        if(ctx->session_status.active) {
            status=Ax211SessionDrain(ctx,out);
            if(NT_SUCCESS(status)) status=Ax211SessionSubmit(ctx,out);
            if(!NT_SUCCESS(status)) {
                ctx->session_status.kernel_status=(ULONG)status;
                (void)Ax211SessionStop(ctx,Ax211SessionFault);
            } else {
                ctx->session_status.exchanges++;
                Ax211SessionRefresh(ctx);
            }
        }
        Ax211SessionStatus(ctx);out->status=ctx->session_status;
        WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*out));
    }
}
