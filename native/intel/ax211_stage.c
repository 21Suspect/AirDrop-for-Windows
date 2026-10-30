// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <initguid.h>
#include "ax211_ioctl.h"
#include "iwl_firmware.h"
#include "iwl_pnvm.h"
#include "iwl_hw.h"
#include "ax211_open.inc"

static int PowerCycle(const wchar_t *report)
{
    IWL_HW_RESULT result={0};
    FILE *output=NULL;
    DWORD error=0,returned=0;
    HANDLE device=INVALID_HANDLE_VALUE;
    BOOL ok=FALSE,invalid_input_rejected=FALSE,short_output_rejected=FALSE;
    UCHAR invalid=1;
    if(_wfopen_s(&output,report,L"w")) return 2;
    device=OpenAx211ForStaging(&error);
    if(device==INVALID_HANDLE_VALUE) goto done;
    invalid_input_rejected=!DeviceIoControl(device,IOCTL_AX211_POWER_CYCLE,
        &invalid,1,&result,sizeof(result),&returned,NULL);
    short_output_rejected=!DeviceIoControl(device,IOCTL_AX211_POWER_CYCLE,
        NULL,0,&result,1,&returned,NULL);
    if(!invalid_input_rejected || !short_output_rejected) {error=ERROR_INVALID_DATA;goto done;}
    if(!DeviceIoControl(device,IOCTL_AX211_POWER_CYCLE,NULL,0,
        &result,sizeof(result),&returned,NULL)) {error=GetLastError();goto done;}
    ok=returned==sizeof(result) && result.size==sizeof(result) && result.version==1 &&
        !result.start_error && !result.stop_error &&
        (result.reset_stopped & 0x100u) && result.register_writes>0;
    if(!ok) error=ERROR_INVALID_DATA;
done:
    fprintf(output,"{\"ok\":%s,\"win32_error\":%lu,\"start_error\":%u,\"stop_error\":%u,"
        "\"gp_before\":%u,\"gp_active\":%u,\"gp_after\":%u,\"reset_stopped\":%u,"
        "\"register_writes\":%u,\"elapsed_us\":%u,\"invalid_input_rejected\":%s,"
        "\"short_output_rejected\":%s,\"firmware_executed\":false,\"hardware_dma_started\":false}\n",
        ok?"true":"false",error,result.start_error,result.stop_error,
        result.gp_before,result.gp_active,result.gp_after,result.reset_stopped,
        result.register_writes,result.elapsed_us,invalid_input_rejected?"true":"false",
        short_output_rejected?"true":"false");
    if(device!=INVALID_HANDLE_VALUE) CloseHandle(device);
    fclose(output);
    return ok?0:1;
}

static void PrintBootFields(FILE *output,const AX211_BOOT_RESULT *result)
{
#define REPORT(field) fprintf(output,",\"" #field "\":%lu",result->field)
    REPORT(size);REPORT(version);REPORT(error);REPORT(kernel_status);REPORT(stage);
    REPORT(start_error);REPORT(stop_error);REPORT(allocated_buffers);REPORT(allocated_bytes);
    REPORT(dma_started);REPORT(hardware_alive);REPORT(firmware_alive);
    REPORT(interrupts);REPORT(msix_causes);REPORT(fh_causes);REPORT(boot_mirror);
    REPORT(iml_response);REPORT(rx_head);REPORT(rx_packets);REPORT(last_packet_code);
    REPORT(alive_status);REPORT(alive_payload_bytes);REPORT(lmac_major);REPORT(lmac_minor);
    REPORT(umac_major);REPORT(umac_minor);REPORT(function_reset_attempted);REPORT(function_reset_status);
    REPORT(gp_control);REPORT(reset_status);REPORT(elapsed_us);
    REPORT(irq_resources);REPORT(irq_connected);REPORT(irq_vector);REPORT(irq_hits);
    REPORT(pci_command);REPORT(pci_msix);REPORT(rx_dma_config);REPORT(rx_dma_status);REPORT(iml_before);
#undef REPORT
    fprintf(output,",\"sku\":[%lu,%lu,%lu],\"buffers_released\":%s,\"radio_tx_rx_implemented\":false",
        result->sku[0],result->sku[1],result->sku[2],result->buffers_released?"true":"false");
}

static int BootFirmware(const wchar_t *firmware, const wchar_t *report)
{
    FILE *input=NULL, *output=NULL;
    UCHAR *data=NULL;
    IWL_FW_PLAN *plan=NULL;
    AX211_BOOT_RESULT result={0}, invalid_result={0};
    HANDLE device=INVALID_HANDLE_VALUE;
    DWORD error=0,returned=0;
    long length;
    BOOL ok=FALSE, malformed_rejected=FALSE, short_output_rejected=FALSE;
    if(_wfopen_s(&output,report,L"w")) return 2;
    if(_wfopen_s(&input,firmware,L"rb")) {error=ERROR_FILE_NOT_FOUND;goto done;}
    if(fseek(input,0,SEEK_END) || (length=ftell(input))<88 ||
        (unsigned long)length>IWL_FW_MAX_BYTES || fseek(input,0,SEEK_SET)) {
        error=ERROR_INVALID_DATA;goto done;
    }
    data=(UCHAR *)malloc((size_t)length);
    plan=(IWL_FW_PLAN *)malloc(sizeof(*plan));
    if(!data || !plan) {error=ERROR_OUTOFMEMORY;goto done;}
    if(fread(data,1,(size_t)length,input)!=(size_t)length ||
        iwl_fw_parse(data,(size_t)length,plan)!=IwlFwOk) {
        error=ERROR_INVALID_DATA;goto done;
    }
    device=OpenAx211ForStaging(&error);
    if(device==INVALID_HANDLE_VALUE) goto done;
    short_output_rejected=!DeviceIoControl(device,IOCTL_AX211_BOOT_FIRMWARE,
        data,(DWORD)length,&invalid_result,1,&returned,NULL) &&
        GetLastError()==ERROR_INSUFFICIENT_BUFFER;
    if(!short_output_rejected) {error=ERROR_INVALID_DATA;goto done;}
    data[0]=1;
    malformed_rejected=DeviceIoControl(device,IOCTL_AX211_BOOT_FIRMWARE,
        data,(DWORD)length,&invalid_result,sizeof(invalid_result),&returned,NULL) &&
        returned==sizeof(invalid_result) && invalid_result.error &&
        !invalid_result.dma_started && invalid_result.buffers_released;
    data[0]=0;
    if(!malformed_rejected) {error=ERROR_INVALID_DATA;goto done;}
    if(!DeviceIoControl(device,IOCTL_AX211_BOOT_FIRMWARE,data,(DWORD)length,
        &result,sizeof(result),&returned,NULL)) {error=GetLastError();goto done;}
    ok=returned==sizeof(result) && result.size==sizeof(result) && result.version==2 &&
        !result.error && !result.kernel_status && result.firmware_alive &&
        result.alive_status==0xcafe && result.buffers_released && !result.stop_error;
    if(!ok) error=ERROR_INVALID_DATA;
done:
    fprintf(output,"{\"ok\":%s,\"win32_error\":%lu,\"malformed_image_rejected\":%s,"
        "\"short_output_rejected\":%s",ok?"true":"false",error,
        malformed_rejected?"true":"false",short_output_rejected?"true":"false");
    PrintBootFields(output,&result);
    fputs("}\n",output);
    if(device!=INVALID_HANDLE_VALUE) CloseHandle(device);
    if(input) fclose(input);
    fclose(output);free(data);free(plan);
    return ok?0:1;
}

static BOOL ReadBoundedFile(const wchar_t *path, ULONG minimum, ULONG maximum,
    UCHAR **data, ULONG *bytes, DWORD *error)
{
    FILE *file=NULL;
    long length;
    BOOL ok=FALSE;
    *data=NULL;*bytes=0;
    if(_wfopen_s(&file,path,L"rb")) {*error=ERROR_FILE_NOT_FOUND;return FALSE;}
    if(fseek(file,0,SEEK_END) || (length=ftell(file))<(long)minimum ||
        (unsigned long)length>maximum || fseek(file,0,SEEK_SET)) {
        *error=ERROR_INVALID_DATA;goto done;
    }
    *data=(UCHAR *)malloc((size_t)length);
    if(!*data) {*error=ERROR_OUTOFMEMORY;goto done;}
    if(fread(*data,1,(size_t)length,file)!=(size_t)length) {*error=ERROR_READ_FAULT;goto done;}
    *bytes=(ULONG)length;ok=TRUE;
done:
    if(!ok) {free(*data);*data=NULL;}
    fclose(file);return ok;
}
static int InitializeFirmware(const wchar_t *firmware, const wchar_t *pnvm,
    const wchar_t *report,BOOL monitor)
{
    FILE *output=NULL;
    UCHAR *fw=NULL,*nv=NULL,*request=NULL;
    ULONG fw_bytes=0,nv_bytes=0,total=0;
    IWL_FW_PLAN *plan=NULL;
    AX211_INIT_INPUT *header;
    AX211_INIT_RESULT result={0},invalid={0};
    AX211_RADIO_RESULT radio={0},invalid_radio={0};
    DWORD ioctl=monitor==2 ? IOCTL_AX211_TRANSMIT_TEST :
        (monitor ? IOCTL_AX211_MONITOR_TEST : IOCTL_AX211_INIT_FIRMWARE);
    DWORD output_bytes=monitor ? sizeof(radio) : sizeof(result);
    void *result_output=monitor ? (void *)&radio : (void *)&result;
    void *invalid_output=monitor ? (void *)&invalid_radio : (void *)&invalid;
    HANDLE device=INVALID_HANDLE_VALUE;
    DWORD error=0,returned=0;
    BOOL ok=FALSE,malformed_rejected=FALSE,short_output_rejected=FALSE;
    if(_wfopen_s(&output,report,L"w")) return 2;
    if(!ReadBoundedFile(firmware,88,IWL_FW_MAX_BYTES,&fw,&fw_bytes,&error) ||
        !ReadBoundedFile(pnvm,8,IWL_PNVM_MAX_BYTES,&nv,&nv_bytes,&error)) goto done;
    plan=(IWL_FW_PLAN *)malloc(sizeof(*plan));
    if(!plan) {error=ERROR_OUTOFMEMORY;goto done;}
    if(iwl_fw_parse(fw,fw_bytes,plan)!=IwlFwOk) {error=ERROR_INVALID_DATA;goto done;}
    total=(ULONG)sizeof(*header)+fw_bytes+nv_bytes;
    request=(UCHAR *)malloc(total);
    if(!request) {error=ERROR_OUTOFMEMORY;goto done;}
    header=(AX211_INIT_INPUT *)request;
    header->magic=AX211_INIT_MAGIC;header->version=1;
    header->firmware_bytes=fw_bytes;header->pnvm_bytes=nv_bytes;
    memcpy(request+sizeof(*header),fw,fw_bytes);
    memcpy(request+sizeof(*header)+fw_bytes,nv,nv_bytes);
    device=OpenAx211ForStaging(&error);
    if(device==INVALID_HANDLE_VALUE) goto done;
    short_output_rejected=!DeviceIoControl(device,ioctl,
        request,total,invalid_output,1,&returned,NULL) && GetLastError()==ERROR_INSUFFICIENT_BUFFER;
    if(!short_output_rejected) {error=ERROR_INVALID_DATA;goto done;}
    header->magic^=1;
    malformed_rejected=DeviceIoControl(device,ioctl,
        request,total,invalid_output,output_bytes,&returned,NULL);
    if(monitor) invalid=invalid_radio.init;
    malformed_rejected=malformed_rejected && returned==output_bytes &&
        invalid.boot.error && !invalid.boot.dma_started && invalid.boot.buffers_released;
    header->magic^=1;
    if(!malformed_rejected) {error=ERROR_INVALID_DATA;goto done;}
    if(!DeviceIoControl(device,ioctl,request,total,
        result_output,output_bytes,&returned,NULL)) {error=GetLastError();goto done;}
    if(monitor) result=radio.init;
    ok=returned==output_bytes && result.boot.size==sizeof(result.boot) &&
        result.boot.version==2 && !result.boot.error && !result.boot.kernel_status &&
        result.boot.firmware_alive && result.boot.alive_status==0xcafe &&
        result.boot.buffers_released && !result.boot.stop_error &&
        result.init_complete && result.nvm_valid && result.commands_sent>=3;
    if(monitor) ok=ok && radio.size==sizeof(radio) && radio.version==2 &&
        radio.configured && radio.valid_frames && radio.duration_us>=5000000u;
    else ok=ok && result.commands_sent==3;
    if(monitor==2) ok=ok && radio.tx_submitted==1 && radio.tx_completed==1 &&
        (radio.tx_status==1 || radio.tx_status==2);
    if(!ok) error=ERROR_INVALID_DATA;
done:
    fprintf(output,"{\"ok\":%s,\"win32_error\":%lu,\"malformed_input_rejected\":%s,"
        "\"short_output_rejected\":%s",ok?"true":"false",error,
        malformed_rejected?"true":"false",short_output_rejected?"true":"false");
    PrintBootFields(output,&result.boot);
#define REPORT(field) fprintf(output,",\"" #field "\":%lu",result.field)
    REPORT(pnvm_version);REPORT(pnvm_bytes);REPORT(pnvm_complete);REPORT(pnvm_status);
    REPORT(commands_sent);REPORT(init_complete);REPORT(nvm_valid);REPORT(nvm_version);
    REPORT(nvm_flags);REPORT(mac_flags);REPORT(tx_chains);REPORT(rx_chains);
    REPORT(lar_enabled);REPORT(channel_count);REPORT(channel6_flags);
#undef REPORT
    fprintf(output,",\"monitor_requested\":%s",monitor?"true":"false");
    if(monitor) {
#define RADIO(field) fprintf(output,",\"" #field "\":%lu",radio.field)
        RADIO(configured);RADIO(command_id);RADIO(command_status);RADIO(command_response_bytes);
        RADIO(mcc);RADIO(mcc_status);RADIO(channel);RADIO(duration_us);
        fprintf(output,",\"mcc_channel6_flags\":%lu",radio.channel6_flags);
        RADIO(rx_notifications);RADIO(valid_frames);RADIO(beacons);RADIO(action_frames);
        RADIO(data_frames);RADIO(filtered_frames);RADIO(malformed_frames);RADIO(wrong_channel_frames);
        RADIO(tx_requested);RADIO(tx_queue);RADIO(tx_index);RADIO(tx_submitted);RADIO(tx_completed);
        RADIO(tx_status);RADIO(tx_response_bytes);RADIO(tx_next_index);RADIO(tx_frame_bytes);
        RADIO(tx_reported_bytes);RADIO(tx_rate);RADIO(tx_retries);
#undef RADIO
    }
    fputs("}\n",output);
    if(device!=INVALID_HANDLE_VALUE) CloseHandle(device);
    fclose(output);free(fw);free(nv);free(request);free(plan);
    return ok?0:1;
}

#include "ax211_session_test.inc"

int wmain(int argc, wchar_t **argv)
{
    FILE *input=NULL, *output=NULL;
    UCHAR *data=NULL;
    IWL_FW_PLAN *plan=NULL;
    AX211_FIRMWARE_RESULT summary={0};
    HANDLE device=INVALID_HANDLE_VALUE;
    long size=0;
    DWORD error=0, returned=0;
    ULONG crc=0xffffffffu, i, bit;
    BOOL malformed_rejected=FALSE, ok=FALSE;
    if(argc==5 && wcscmp(argv[1],L"--session-test")==0)
        return SessionTest(argv[2],argv[3],argv[4]);
    if(argc==5 && wcscmp(argv[1],L"--transmit-frame")==0)
        return InitializeFirmware(argv[2],argv[3],argv[4],2);
    if(argc==5 && wcscmp(argv[1],L"--monitor-receive")==0)
        return InitializeFirmware(argv[2],argv[3],argv[4],TRUE);
    if(argc==5 && wcscmp(argv[1],L"--init-firmware")==0)
        return InitializeFirmware(argv[2],argv[3],argv[4],FALSE);
    if(argc==4 && wcscmp(argv[1],L"--boot-firmware")==0)
        return BootFirmware(argv[2],argv[3]);
    if (argc!=3) {
        fwprintf(stderr,L"Usage: ax211-stage <firmware> <report.json>\n");return 2;
    }
    if(wcscmp(argv[1],L"--power-cycle")==0) return PowerCycle(argv[2]);
    if (_wfopen_s(&output,argv[2],L"w")) return 2;
    if (_wfopen_s(&input,argv[1],L"rb")) { error=ERROR_FILE_NOT_FOUND; goto done; }
    if (fseek(input,0,SEEK_END) || (size=ftell(input))<88 ||
        (unsigned long)size>IWL_FW_MAX_BYTES || fseek(input,0,SEEK_SET)) {
        error=ERROR_INVALID_DATA;goto done;
    }
    data=(UCHAR *)malloc((size_t)size);
    plan=(IWL_FW_PLAN *)malloc(sizeof(*plan));
    if(!data || !plan) {error=ERROR_OUTOFMEMORY;goto done;}
    if(fread(data,1,(size_t)size,input)!=(size_t)size ||
        iwl_fw_parse(data,(size_t)size,plan)!=IwlFwOk) {
        error=ERROR_INVALID_DATA;goto done;
    }
    for(i=0;i<=plan->section_count;++i) {
        const UCHAR *p;
        ULONG count,j;
        if(i<plan->section_count) { p=data+plan->sections[i].file_offset;count=plan->sections[i].length; }
        else { p=data+plan->iml_offset;count=plan->iml_length; }
        for(j=0;j<count;++j) {
            crc^=p[j];
            for(bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
        }
    }
    crc^=0xffffffffu;
    device=OpenAx211ForStaging(&error);
    if(device==INVALID_HANDLE_VALUE) goto done;
    data[0]=1;
    malformed_rejected=!DeviceIoControl(device,IOCTL_AX211_STAGE_FIRMWARE,
        data,(DWORD)size,&summary,sizeof(summary),&returned,NULL);
    data[0]=0;
    if(!malformed_rejected) {error=ERROR_INVALID_DATA;goto done;}
    if(!DeviceIoControl(device,IOCTL_AX211_STAGE_FIRMWARE,
        data,(DWORD)size,&summary,sizeof(summary),&returned,NULL)) {
        error=GetLastError();goto done;
    }
    if(returned!=sizeof(summary) || summary.size!=sizeof(summary) ||
        summary.abi_version!=1 || summary.source_crc32!=crc || summary.staged_crc32!=crc ||
        summary.buffer_count!=plan->section_count+1 ||
        summary.staged_bytes!=plan->total_section_bytes+plan->iml_length) {
        error=ERROR_INVALID_DATA;goto done;
    }
    ok=TRUE;
done:
    fprintf(output,"{\"ok\":%s,\"win32_error\":%lu,\"buffer_count\":%lu,"
        "\"staged_bytes\":%lu,\"source_crc32\":%lu,\"staged_crc32\":%lu,"
        "\"lmac_sections\":%lu,\"umac_sections\":%lu,\"paging_sections\":%lu,"
        "\"malformed_image_rejected\":%s,\"firmware_executed\":false,"
        "\"hardware_dma_started\":false,\"buffers_released_after_test\":true}\n",
        ok?"true":"false",error,summary.buffer_count,summary.staged_bytes,
        summary.source_crc32,summary.staged_crc32,summary.lmac_sections,
        summary.umac_sections,summary.paging_sections,malformed_rejected?"true":"false");
    if(device!=INVALID_HANDLE_VALUE) CloseHandle(device);
    if(input) fclose(input);
    fclose(output);free(data);free(plan);return ok?0:1;
}
