// SPDX-License-Identifier: GPL-3.0-or-later AND ISC AND BSD-3-Clause
// Packet layouts adapted from OpenBSD iwx under its BSD license option.
// Original notices retained below.
/*
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 *   Author: Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*-
 * Based on BSD-licensed source modules in the Linux iwlwifi driver,
 * which were used as the reference documentation for this implementation.
 *
 ******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************
 */

/*-
 * Copyright (c) 2007-2010 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Additional Linux iwlwifi BSD-3-Clause references:
 * Copyright (C) 2012-2014, 2018-2022 Intel Corporation
 * Copyright (C) 2013-2015 Intel Mobile Communications GmbH
 * Copyright (C) 2015-2017 Intel Deutschland GmbH
 * The BSD-3-Clause terms retained above apply to these references.
 */

#include "iwl_packet.h"
#include <string.h>
static uint16_t p16(const uint8_t *p)
{
    return (uint16_t)(p[0]|(uint16_t)p[1]<<8);
}
static uint32_t p32(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void w16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p,uint32_t v)
{
    unsigned i;for(i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8));
}
static void w64(uint8_t *p,uint64_t v)
{
    unsigned i;for(i=0;i<8;++i) p[i]=(uint8_t)(v>>(i*8));
}
/* Three-address, unencrypted management/data frames used by AWDL and IBSS.
 * Owly owns its own sequence numbers. Do not change them or invent an ACK
 * policy for non-QoS frames. Control and DS traffic belong to another path. */
static int header_size(const uint8_t *frame,size_t bytes,uint32_t *header)
{
    uint16_t fc;
    unsigned type,subtype;
    if(bytes<2) return IwlPacketMalformed;
    fc=p16(frame);type=fc&0xcu;subtype=fc&0xf0u;
    if(fc&3u) return IwlPacketMalformed;
    if(type!=0 && type!=8) return IwlPacketFiltered;
    if(bytes<24) return IwlPacketMalformed;
    if((fc&0xc700u) || (p16(frame+22)&15u)) return IwlPacketFiltered;
    if(type==8 && subtype!=0 && subtype!=0x80u) return IwlPacketFiltered;
    *header=type==8 && subtype==0x80u ? 26u : 24u;
    if(bytes<*header) return IwlPacketMalformed;
    return IwlPacketOk;
}
int iwl_rx_frame_decode(const uint8_t *payload,size_t bytes,
    uint8_t *frame,size_t capacity,IWL_RX_FRAME_INFO *info)
{
    const uint8_t *mac;
    uint32_t length,header,pad,trailer,status,normalized;
    int result,a,b;
    if(!info) return IwlPacketMalformed;
    memset(info,0,sizeof(*info));
    if(!payload || !frame || bytes<IWL_RX_MPDU_DESC_BYTES || bytes>4088u)
        return IwlPacketMalformed;
    length=p16(payload);status=p32(payload+12);
    if(length>bytes-IWL_RX_MPDU_DESC_BYTES || length>IWL_RX_FRAME_MAX)
        return IwlPacketMalformed;
    if((status&3u)!=3u || (status&0x700u) || (status&(1u<<29)))
        return IwlPacketFiltered; /* bad CRC/overrun, encryption, RF kill */
    mac=payload+IWL_RX_MPDU_DESC_BYTES;
    result=header_size(mac,length,&header);
    if(result!=IwlPacketOk) return result;
    pad=(payload[3]&0x20u) ? 2u : 0;
    trailer=(payload[2]&0xf0u)>>3; /* MIC+CRC length in 2-byte units */
    /* Protected frames were filtered; only a four-byte FCS can remain. */
    if(trailer!=0 && trailer!=4) return IwlPacketUnsupported;
    if(length<header+pad+trailer) return IwlPacketMalformed;
    if((payload[3]&0x40u) && header!=26u) return IwlPacketMalformed;
    normalized=length-pad-trailer;
    if(capacity<normalized) return IwlPacketCapacity;
    memcpy(frame,mac,header);
    memcpy(frame+header,mac+header+pad,normalized-header);
    if(payload[3]&0x40u) frame[24]&=0x7fu;
    /* Only 9000-family firmware reverses A-MSDU addresses. AX211 does not. */
    a=payload[40] ? -(int)payload[40] : -256;
    b=payload[41] ? -(int)payload[41] : -256;
    info->frame_bytes=normalized;info->header_bytes=header;
    info->channel=payload[42];info->rate_n_flags=p32(payload+36);
    info->rssi_dbm=a>b ? a : b;
    info->phy_info=p16(payload+5);info->device_time=p32(payload+44);
    info->status=status;info->deaggregated=(payload[3]&0x40u)!=0;
    return IwlPacketOk;
}
int iwl_tx_frame_build(const IWL_TX_FRAME_REQUEST *request,
    const uint8_t *frame,size_t bytes,uint64_t dma_address,
    void *slot,size_t slot_capacity,void *tfd,size_t tfd_capacity,
    IWL_TX_FRAME_INFO *info)
{
    static const uint8_t rates[8]={12,18,24,36,48,72,96,108};
    uint8_t *command=(uint8_t *)slot,*desc=(uint8_t *)tfd;
    uint32_t header,input_header,wire_bytes,pad,head_bytes,total,rate,offload,i,body_bytes;
    int result;
    if(!info) return IwlPacketMalformed;
    memset(info,0,sizeof(*info));
    if(!request || !frame || !slot || !tfd || bytes>IWL_TX_FRAME_MAX ||
        !request->queue || request->queue>31 || request->index>255 ||
        (request->antenna!=1 && request->antenna!=2) || request->no_ack>1 ||
        !dma_address || (dma_address&4095u) || dma_address>UINT64_MAX-4096u)
        return IwlPacketMalformed;
    if(request->command_version!=10 || request->notification_version!=7)
        return IwlPacketUnsupported;
    result=header_size(frame,bytes,&header);
    if(result!=IwlPacketOk) return result;
    input_header=header;wire_bytes=(uint32_t)bytes;
    if(request->no_ack && !(frame[4]&1u) && header!=26) {
        if((p16(frame)&0xfcu)!=8u) return IwlPacketUnsupported;
        /* A non-QoS unicast has no No Ack policy field. Add QoS control,
         * TID 0 / No Ack, keeping addresses, sequence and payload intact.
         * AWDL's data encoding supports both DATA and QOS_DATA. */
        if(bytes>IWL_TX_FRAME_MAX-2u) return IwlPacketCapacity;
        header=26;wire_bytes+=2;
    }
    for(i=0;i<8;++i) if(request->rate_half_mbps==rates[i]) break;
    if(i==8) return IwlPacketUnsupported;
    /* TX notification v7 uses rate_n_flags v2 (indices, not old PLCP codes). */
    rate=(request->antenna<<14)|0x100u|i;
    pad=(4u-(header&3u))&3u;
    head_bytes=32u+header+pad; /* short command header + Gen3 TX + MAC */
    total=32u+wire_bytes+pad;
    body_bytes=(uint32_t)bytes-input_header;
    if(slot_capacity<IWL_TX_SLOT_BYTES || tfd_capacity<IWL_TX_DESCRIPTOR_BYTES)
        return IwlPacketCapacity;
    offload=(header/2u)<<8;
    if(pad) offload|=1u<<13;
    if(input_header==26 && (frame[24]&0x80u)) offload|=1u<<14;
    memset(command,0,IWL_TX_SLOT_BYTES);
    memset(desc,0,IWL_TX_DESCRIPTOR_BYTES);
    command[0]=0x1c;command[2]=(uint8_t)request->index;
    command[3]=(uint8_t)request->queue;
    w16(command+4,(uint16_t)wire_bytes);w16(command+6,3); /* CMD_RATE | ENCRYPT_DIS */
    w32(command+8,offload);w32(command+20,rate);
    memcpy(command+32,frame,input_header);
    if(header!=input_header) command[32]|=0x80u;
    if(request->no_ack && header==26)
        command[56]=(uint8_t)((command[56]&~0x60u)|0x20u);
    if(body_bytes) memcpy(command+head_bytes,frame+input_header,body_bytes);
    w16(desc,body_bytes ? 3 : 2);
    w16(desc+2,20);w64(desc+4,dma_address);
    w16(desc+12,(uint16_t)(head_bytes-20u));w64(desc+14,dma_address+20u);
    if(body_bytes) {
        w16(desc+22,(uint16_t)body_bytes);w64(desc+24,dma_address+head_bytes);
    }
    info->slot_bytes=total;info->frame_bytes=wire_bytes;info->header_bytes=header;
    /* Three TBs occupy 32 bytes: one 64-byte fetch, chunk field is zero. */
    info->byte_count=wire_bytes;info->rate_n_flags=rate;
    return IwlPacketOk;
}

int iwl_awdl_frame_validate(const uint8_t *frame,size_t bytes,
    const uint8_t local[6],unsigned transmit)
{
    static const uint8_t bssid[6]={0,0x25,0,0xff,0x94,0x73};
    static const uint8_t action[6]={0x7f,0,0x17,0xf2,8,0x10};
    uint32_t header;int result;
    if(!frame || !local || transmit>1 || bytes>IWL_RX_FRAME_MAX ||
        (transmit && bytes>IWL_TX_FRAME_MAX)) return IwlPacketMalformed;
    result=header_size(frame,bytes,&header);
    if(result!=IwlPacketOk) return result;
    if(memcmp(frame+16,bssid,6) || (frame[10]&1u)) return IwlPacketFiltered;
    if(transmit) {
        if(memcmp(frame+10,local,6)) return IwlPacketFiltered;
    } else if(!memcmp(frame+10,local,6) ||
        (!(frame[4]&1u) && memcmp(frame+4,local,6))) return IwlPacketFiltered;
    if((frame[0]&0x0cu)==0) {
        if((frame[0]&0xfcu)!=0xd0u || bytes<30 ||
            memcmp(frame+24,action,sizeof(action))) return IwlPacketFiltered;
    }
    return IwlPacketOk;
}
