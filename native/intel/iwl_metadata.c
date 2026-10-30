// SPDX-License-Identifier: GPL-3.0-or-later
// Published TLV metadata; no instruction decoding or firmware modifications.
#include "iwl_metadata.h"
#include <string.h>
static uint32_t m32(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
int iwl_metadata_parse(const uint8_t *data, size_t length, IWL_METADATA *out)
{
    IWL_METADATA value;
    uint32_t api_seen=0,capa_seen=0,records=0;
    int versions_seen=0,fw_version_seen=0;
    size_t at=88;
    if(!out) return 1;
    memset(out,0,sizeof(*out));memset(&value,0,sizeof(value));
    if(!data || length<88 || length>IWL_FW_MAX_BYTES ||
        m32(data)!=0 || m32(data+4)!=0x0a4c5749u) return 1;
    while(at<length) {
        uint32_t type,bytes;
        size_t payload,padded;
        const uint8_t *p;
        if(++records>4096 || length-at<8) return 1;
        type=m32(data+at);bytes=m32(data+at+4);payload=at+8;
        if(bytes>length-payload) return 1;
        padded=(size_t)bytes+((4u-(bytes&3u))&3u);
        if(padded>length-payload) return 1;
        p=data+payload;
        if(type==36) {
            if(fw_version_seen || bytes!=12 || m32(p)!=IWL_FW_API_VERSION) return 1;
            fw_version_seen=1;
        } else if(type==48) {
            if(versions_seen || (bytes&3u) || bytes/4>IWL_METADATA_COMMANDS) return 1;
            versions_seen=1;value.command_count=bytes/4;
            memcpy(value.commands,p,bytes);
        } else if(type==29 || type==30) {
            uint32_t index,*seen,*words;
            if(bytes!=8 || (index=m32(p))>=IWL_METADATA_WORDS) return 1;
            seen=type==29 ? &api_seen : &capa_seen;
            words=type==29 ? value.api : value.capabilities;
            if(*seen & (1u<<index)) return 1;
            *seen|=1u<<index;words[index]=m32(p+4);
        } else if(type==22) {
            if(bytes!=12 || m32(p)>3) return 1;
            if(m32(p)==0) {
                if(value.calibration_present) return 1;
                value.calibration_present=1;
                value.calibration_flow=m32(p+4);value.calibration_event=m32(p+8);
            }
        }
        at=payload+padded;
    }
    if(!fw_version_seen) return 1;
    *out=value;return 0;
}
unsigned iwl_metadata_version(const IWL_METADATA *meta, unsigned group,
    unsigned opcode, int notification)
{
    uint32_t i;
    if(!meta || meta->command_count>IWL_METADATA_COMMANDS || group>255 || opcode>255) return 99;
    for(i=0;i<meta->command_count;++i) {
        const IWL_COMMAND_VERSION *v=&meta->commands[i];
        if(v->opcode==opcode && v->group==group)
            return notification ? v->notification : v->command;
    }
    return 99;
}
int iwl_metadata_bit(const IWL_METADATA *meta,unsigned bit,int capability)
{
    if(!meta || bit>=IWL_METADATA_WORDS*32) return 0;
    return ((capability ? meta->capabilities : meta->api)[bit/32] & (1u<<(bit%32)))!=0;
}
