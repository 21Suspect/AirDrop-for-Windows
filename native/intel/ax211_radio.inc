// SPDX-License-Identifier: GPL-3.0-or-later
// Fixed, five-second receive-only monitor test. No frame payload leaves the driver.
#include "iwl_radio.h"
#include "iwl_packet.h"
static NTSTATUS Ax211TransmitTest(AX211_KM_CONTEXT *,AX211_RADIO_RESULT *);
static NTSTATUS Ax211RadioQueue(AX211_KM_CONTEXT *,AX211_RADIO_RESULT *);
static NTSTATUS Ax211IbssStart(AX211_KM_CONTEXT *,AX211_RADIO_RESULT *);
static NTSTATUS Ax211Monitor(AX211_KM_CONTEXT *ctx,AX211_RADIO_RESULT *radio)
{
    static const UCHAR address[6]={0x02,0xad,0x21,0,0,1};
    AX211_INIT_RESULT *init=&radio->init;
    AX211_BOOT_RESULT *boot=&init->boot;
    AX211_BOOT_WORK *work=ctx->boot;
    IWL_RADIO_COMMAND command;
    IWL_MCC_INFO mcc;
    ULONGLONG begin,deadline;
    ULONG id;
    NTSTATUS status;
    if(iwl_metadata_version(&work->metadata,1,8,0)!=4 ||
        iwl_metadata_version(&work->metadata,5,8,0)!=2 ||
        iwl_metadata_version(&work->metadata,1,0x18,0)!=12 ||
        iwl_metadata_version(&work->metadata,1,0xc8,1)!=6 ||
        !iwl_metadata_bit(&work->metadata,48,1) ||
        !iwl_metadata_bit(&work->metadata,67,1) ||
        !iwl_metadata_bit(&work->metadata,74,1) ||
        !iwl_metadata_bit(&work->metadata,44,1) ||
        iwl_metadata_bit(&work->metadata,12,1)) return STATUS_NOT_SUPPORTED;
    radio->channel=6;
    for(id=0;id<IwlRadioCommandCount;++id) {
        if(id==IwlRadioMac && ctx->ibss_requested) return Ax211IbssStart(ctx,radio);
        radio->command_id=id;radio->command_status=MAXULONG;
        boot->stage=20+id;
        if(iwl_radio_monitor_command(id,init->tx_chains,init->rx_chains,address,&command))
            return STATUS_INVALID_PARAMETER;
        if(id==IwlRadioStation && radio->tx_requested) {
            command.data[2]=0;command.data[3]=0;
        }
        status=Ax211Command(ctx,init,(UCHAR)command.group,(UCHAR)command.opcode,
            (UCHAR)command.version,command.data,command.length);
        // Preserve the firmware's REPLY_ERROR type before returning failure.
        // last_packet_code/MSI causes distinguish it from a normal response.
        radio->command_response_bytes=work->response_info.payload_bytes;
        if(work->response_info.payload_bytes>=4)
            radio->command_status=Ax211ReadLe32(work->response+8);
        if(!NT_SUCCESS(status)) return status;
        if(id==IwlRadioMcc) {
            if(iwl_radio_mcc_decode(work->response+8,work->response_info.payload_bytes,&mcc))
                return STATUS_DATA_ERROR;
            radio->mcc=mcc.mcc;radio->mcc_status=mcc.status;
            radio->channel6_flags=mcc.channels[5];
            // Listening only: VALID is required, ACTIVE/IBSS are not overridden.
            if(!(mcc.channels[5]&1u)) return STATUS_NOT_SUPPORTED;
            // An IBSS beacon can start autonomously. Check TX permission BEFORE
            // adding its template/MAC/stations, not merely at data-queue setup.
            if(ctx->ibss_requested && ((mcc.channels[5]&0xbu)!=0xbu ||
                (init->channel6_flags&0xbu)!=0xbu || (mcc.channels[5]&0x10u)))
                return STATUS_NOT_SUPPORTED;
        } else if(id==IwlRadioBinding && (work->response_info.payload_bytes!=4 ||
            radio->command_status!=0)) return STATUS_DEVICE_HARDWARE_ERROR;
        else if(id==IwlRadioStation && (work->response_info.payload_bytes!=4 ||
            (radio->command_status&0xffu)!=1)) return STATUS_DEVICE_HARDWARE_ERROR;
    }
    radio->configured=1;boot->stage=40;
    if(ctx->session_requested) return Ax211RadioQueue(ctx,radio);
    begin=Ax211HwNow(ctx);deadline=begin+5000000u;
    while(Ax211HwNow(ctx)<deadline) {
        ULONG drained;
        status=Ax211BootCauses(ctx,boot);
        if(!NT_SUCCESS(status)) return status;
        for(drained=0;drained<IWL_RX_COUNT;++drained) {
            IWL_RX_FRAME_INFO info;int decoded;
            status=Ax211BootNextResponse(ctx,boot);
            if(status==STATUS_NO_MORE_ENTRIES) break;
            if(!NT_SUCCESS(status)) return status;
            if(work->response_info.opcode==2 && work->response_info.group<=1)
                return STATUS_DEVICE_HARDWARE_ERROR;
            if(work->response_info.opcode!=0xc1 || work->response_info.group>1 ||
                !(work->response_info.queue&0x80u)) continue;
            radio->rx_notifications++;
            decoded=iwl_rx_frame_decode(work->response+8,work->response_info.payload_bytes,
                work->decoded_frame,sizeof(work->decoded_frame),&info);
            if(decoded==IwlPacketFiltered || decoded==IwlPacketUnsupported) {
                radio->filtered_frames++;continue;
            }
            if(decoded!=IwlPacketOk) {radio->malformed_frames++;continue;}
            if(info.channel!=6) {radio->wrong_channel_frames++;continue;}
            radio->valid_frames++;
            if((work->decoded_frame[0]&0xfcu)==0x80u) radio->beacons++;
            if((work->decoded_frame[0]&0xfcu)==0xd0u) radio->action_frames++;
            if((work->decoded_frame[0]&0x0cu)==8u) radio->data_frames++;
        }
        Ax211HwDelay(ctx,1000);
    }
    radio->duration_us=(ULONG)(Ax211HwNow(ctx)-begin);
    boot->stage=41;
    if(!radio->valid_frames) return STATUS_IO_TIMEOUT;
    return radio->tx_requested ? Ax211TransmitTest(ctx,radio) : STATUS_SUCCESS;
}

#include "awdl_test_frame.inc"
static NTSTATUS Ax211RadioQueue(AX211_KM_CONTEXT *ctx,AX211_RADIO_RESULT *radio)
{
    AX211_BOOT_WORK *work=ctx->boot;
    AX211_BOOT_RESULT *boot=&radio->init.boot;
    IWL_RADIO_COMMAND command;
    NTSTATUS status;
    ULONGLONG tfd_addr,count_addr;
    ULONG index,queue;
    boot->stage=42;
    // Require NVM and current MCC VALID|IBSS|ACTIVE, no radar/DFS on this channel.
    if((radio->channel6_flags&0xbu)!=0xbu ||
        (radio->init.channel6_flags&0xbu)!=0xbu ||
        (radio->channel6_flags&0x10u) ||
        iwl_metadata_version(&work->metadata,5,0x17,0)!=3 ||
        iwl_metadata_version(&work->metadata,5,0x17,1)!=2 ||
        iwl_metadata_version(&work->metadata,1,0x1c,0)!=10 ||
        iwl_metadata_version(&work->metadata,1,0x1c,1)!=7)
        return STATUS_NOT_SUPPORTED;
    status=Ax211InitBuffer(ctx,65536,&work->tx_tfd_buffer,boot);
    if(!NT_SUCCESS(status)) return status;
    status=Ax211InitBuffer(ctx,4096,&work->tx_count_buffer,boot);
    if(!NT_SUCCESS(status)) return status;
    status=Ax211InitBuffer(ctx,4096,&work->tx_frame_buffer,boot);
    if(!NT_SUCCESS(status)) return status;
    tfd_addr=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->tx_tfd_buffer).QuadPart;
    count_addr=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->tx_count_buffer).QuadPart;
    // Passive monitor injection uses the IBSS broadcast station for management
    // and its multicast station for data, as mac80211 does with an IBSS vif.
    if(ctx->ibss_requested) {
        if(iwl_radio_station_queue(tfd_addr,count_addr,3,15,&command))
            return STATUS_INVALID_PARAMETER;
    } else if(iwl_radio_queue_command(tfd_addr,count_addr,&command))
        return STATUS_INVALID_PARAMETER;
    status=Ax211Command(ctx,&radio->init,(UCHAR)command.group,(UCHAR)command.opcode,
        (UCHAR)command.version,command.data,command.length);
    if(!NT_SUCCESS(status)) return status;
    radio->tx_response_bytes=work->response_info.payload_bytes;
    if(work->response_info.payload_bytes!=8) return STATUS_DATA_ERROR;
    queue=(ULONG)work->response[8]|(ULONG)work->response[9]<<8;
    index=(ULONG)work->response[12]|(ULONG)work->response[13]<<8;
    radio->tx_queue=queue;radio->tx_index=index;
    if(!queue || queue>31 || index>65535) return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}
static NTSTATUS Ax211TransmitTest(AX211_KM_CONTEXT *ctx,AX211_RADIO_RESULT *radio)
{
    AX211_BOOT_WORK *work=ctx->boot;
    AX211_BOOT_RESULT *boot=&radio->init.boot;
    IWL_TX_FRAME_REQUEST request;
    IWL_TX_FRAME_INFO info;
    NTSTATUS status=Ax211RadioQueue(ctx,radio);
    ULONGLONG frame_addr,deadline;
    PUCHAR descriptors,counts,frame;
    ULONG index=radio->tx_index,queue=radio->tx_queue;
    if(!NT_SUCCESS(status)) return status;
    frame_addr=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->tx_frame_buffer).QuadPart;
    RtlZeroMemory(&request,sizeof(request));
    request.queue=queue;request.index=index&255u;request.rate_half_mbps=12;
    request.antenna=(radio->init.tx_chains&1u)?1u:2u;
    request.no_ack=1;request.command_version=10;request.notification_version=7;
    descriptors=(PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(work->tx_tfd_buffer);
    counts=(PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(work->tx_count_buffer);
    frame=(PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(work->tx_frame_buffer);
    if(iwl_tx_frame_build(&request,Ax211TestDiscoveryFrame,sizeof(Ax211TestDiscoveryFrame),
        frame_addr,frame,4096,descriptors+(index&255u)*256u,256,&info)!=IwlPacketOk)
        return STATUS_INVALID_PARAMETER;
    // Gen3 byte count table: length + 64-byte TFD fetch count (one chunk -> zero).
    counts[(index&255u)*2]=(UCHAR)info.byte_count;counts[(index&255u)*2+1]=(UCHAR)(info.byte_count>>8);
    KeMemoryBarrier();
    radio->tx_frame_bytes=info.frame_bytes;radio->tx_submitted=1;boot->stage=43;
    Ax211HwWrite(ctx,0x460,(queue<<16)|((index+1u)&65535u));
    deadline=Ax211HwNow(ctx)+2000000u;
    while(Ax211HwNow(ctx)<deadline) {
        ULONG drained;
        status=Ax211BootCauses(ctx,boot);
        if(!NT_SUCCESS(status)) return status;
        for(drained=0;drained<IWL_RX_COUNT;++drained) {
            IWL_TX_COMPLETION_INFO completion;
            status=Ax211BootNextResponse(ctx,boot);
            if(status==STATUS_NO_MORE_ENTRIES) break;
            if(!NT_SUCCESS(status)) return status;
            if(work->response_info.opcode==2 && work->response_info.group<=1)
                return STATUS_DEVICE_HARDWARE_ERROR;
            if(work->response_info.opcode!=0x1c || work->response_info.group>1 ||
                (work->response_info.queue&0x7fu)!=queue) continue;
            radio->tx_response_bytes=work->response_info.payload_bytes;
            if(iwl_radio_tx_completion(work->response+8,work->response_info.payload_bytes,
                queue,index,&completion)) return STATUS_DATA_ERROR;
            radio->tx_completed=1;radio->tx_status=completion.status;
            radio->tx_next_index=completion.next_index;radio->tx_rate=completion.rate;
            radio->tx_reported_bytes=completion.byte_count;radio->tx_retries=completion.retries;
            boot->stage=44;
            return completion.success ? STATUS_SUCCESS : STATUS_DEVICE_HARDWARE_ERROR;
        }
        Ax211HwDelay(ctx,1000);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS Ax211IbssDataQueue(AX211_KM_CONTEXT *ctx,AX211_RADIO_RESULT *radio)
{
    AX211_BOOT_WORK *work=ctx->boot;
    AX211_BOOT_RESULT *boot=&radio->init.boot;
    IWL_RADIO_COMMAND command;
    ULONGLONG tfd,count;
    NTSTATUS status;
    ULONG queue,index;
    status=Ax211InitBuffer(ctx,65536,&work->ibss_data_tfd,boot);
    if(!NT_SUCCESS(status)) return status;
    status=Ax211InitBuffer(ctx,4096,&work->ibss_data_count,boot);
    if(!NT_SUCCESS(status)) return status;
    tfd=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->ibss_data_tfd).QuadPart;
    count=(ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(work->ibss_data_count).QuadPart;
    if(iwl_radio_station_queue(tfd,count,4,0,&command)) return STATUS_INVALID_PARAMETER;
    status=Ax211Command(ctx,&radio->init,(UCHAR)command.group,(UCHAR)command.opcode,
        (UCHAR)command.version,command.data,command.length);
    if(!NT_SUCCESS(status)) return status;
    if(work->response_info.payload_bytes!=8) return STATUS_DATA_ERROR;
    queue=(ULONG)work->response[8]|(ULONG)work->response[9]<<8;
    index=(ULONG)work->response[12]|(ULONG)work->response[13]<<8;
    if(!queue || queue>31) return STATUS_NOT_SUPPORTED;
    work->ibss_data_queue=queue;work->ibss_data_index=index;
    return STATUS_SUCCESS;
}
static NTSTATUS Ax211IbssStart(AX211_KM_CONTEXT *ctx,AX211_RADIO_RESULT *radio)
{
    static const UCHAR address[6]={2,0xad,0x21,0,0,1};
    AX211_BOOT_WORK *work=ctx->boot;
    IWL_RADIO_COMMAND command;
    NTSTATUS status;
    static const unsigned order[IwlIbssCommandCount]={
        IwlIbssMac,IwlIbssBeacon,IwlIbssBinding,IwlIbssMulticast,IwlIbssBroadcast
    };
    ULONG step,id;
    if(iwl_metadata_version(&work->metadata,1,0x91,0)!=13 ||
        iwl_metadata_version(&work->metadata,1,0x28,0)!=5 ||
        !iwl_metadata_bit(&work->metadata,30,0) ||
        !iwl_metadata_bit(&work->metadata,34,0) ||
        !iwl_metadata_bit(&work->metadata,46,0) ||
        !iwl_metadata_bit(&work->metadata,70,1) ||
        !iwl_metadata_bit(&work->metadata,71,1)) return STATUS_NOT_SUPPORTED;
    // Beacon v13 names a link. Create its MAC context before the template.
    for(step=0;step<IwlIbssCommandCount;++step) {
        id=order[step];
        radio->command_id=100+id;radio->init.boot.stage=60+id;
        radio->command_status=MAXULONG;
        if(iwl_radio_ibss_command(id,address,&command)) return STATUS_INVALID_PARAMETER;
        status=Ax211Command(ctx,&radio->init,(UCHAR)command.group,(UCHAR)command.opcode,
            (UCHAR)command.version,command.data,command.length);
        // Preserve the firmware's REPLY_ERROR type before returning failure.
        // last_packet_code/MSI causes distinguish it from a normal response.
        radio->command_response_bytes=work->response_info.payload_bytes;
        if(work->response_info.payload_bytes>=4)
            radio->command_status=Ax211ReadLe32(work->response+8);
        if(!NT_SUCCESS(status)) return status;
        if(id==IwlIbssBinding && (work->response_info.payload_bytes!=4 ||
            radio->command_status!=0)) return STATUS_DEVICE_HARDWARE_ERROR;
        if(id>=IwlIbssMulticast && (work->response_info.payload_bytes!=4 ||
            (radio->command_status&0xffu)!=1)) return STATUS_DEVICE_HARDWARE_ERROR;
        if(id==IwlIbssMulticast) {
            radio->init.boot.stage=70+id;
            status=Ax211IbssDataQueue(ctx,radio);
            if(!NT_SUCCESS(status)) return status;
        }
    }
    radio->configured=1;
    return Ax211RadioQueue(ctx,radio);
}
