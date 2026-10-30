// SPDX-License-Identifier: GPL-3.0-or-later AND ISC AND BSD-3-Clause
// Fixed monitor commands adapted from OpenBSD iwx under its BSD license option.
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

// Bluetooth coexistence also follows Linux v6.1 mvm/coex.c (BSD option).
// Copyright (C) 2013-2014, 2018-2020, 2022 Intel Corporation
// Copyright (C) 2013-2015 Intel Mobile Communications GmbH

/* Additional Linux reference notices (BSD-3-Clause option, terms above):
 * Copyright (C) 2012-2015, 2018-2023 Intel Corporation
 * Copyright (C) 2013-2015 Intel Mobile Communications GmbH
 * Copyright (C) 2015-2017 Intel Deutschland GmbH
 */
#include "iwl_radio.h"
#include <string.h>
static void radio32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static uint32_t radio_read32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int iwl_radio_monitor_command(unsigned id,unsigned tx,unsigned rx,
    const uint8_t address[6],IWL_RADIO_COMMAND *out)
{
    IWL_RADIO_COMMAND c;
    if(!out) return 1;
    memset(out,0,sizeof(*out));memset(&c,0,sizeof(c));
    if(id>=IwlRadioCommandCount || !address || (address[0]&3u)!=2u ||
        !tx || (tx&~3u) || !rx || (rx&~3u)) return 1;
    c.group=1;
    switch(id) {
    case IwlRadioAntenna:
        c.opcode=0x98;c.length=4;radio32(c.data,tx);break;
    case IwlRadioCoexistence:
        c.opcode=0x9b;c.length=8;
        radio32(c.data,1); // BT_COEX_NW: let firmware arbitrate Wi-Fi/Bluetooth.
        radio32(c.data+4,0x15); // MPLUT, SYNC2SCO, HIGH_BAND_RET.
        break;
    case IwlRadioSoc:
        c.group=2;c.opcode=1;c.length=8;
        // SO integrated: low-latency XTAL. Conservative published 12000 us.
        radio32(c.data,2);radio32(c.data+4,12000);break;
    case IwlRadioTemperature:
        c.group=4;c.opcode=4;c.length=20;break; // Firmware owns CT-kill.
    case IwlRadioPower:
        c.opcode=0x77;c.length=4;break; // Keep device awake for monitor operation.
    case IwlRadioMcc:
        c.opcode=0xc8;c.length=28;
        c.data[0]='Z';c.data[1]='Z';c.data[2]=0x10; // Get current/NVM profile.
        break;
    case IwlRadioPhy:
        c.opcode=8;c.length=32;radio32(c.data+4,1); // PHY 0, ADD.
        radio32(c.data+8,6);c.data[12]=1; // UHB-layout channel 6, 2.4 GHz, 20 MHz.
        // PHY v4 reserves RX chains; separate RLC v2 supplies them.
        break;
    case IwlRadioRlc:
        c.group=5;c.opcode=8;c.version=2;c.length=32;
        radio32(c.data+4,(rx<<1)|(1u<<10)|(1u<<12));break;
    case IwlRadioMac:
        c.opcode=0x28;c.length=148;
        radio32(c.data+4,1);radio32(c.data+8,2); // MAC 0, ADD, LISTENER.
        memcpy(c.data+16,address,6);memset(c.data+24,0xff,6);
        radio32(c.data+52,0x1847); // Promiscuous control/management, group, beacon, FCS, probe.
        break;
    case IwlRadioBinding:
        c.opcode=0x2b;c.length=28;radio32(c.data+4,1); // Binding 0, MAC 0, PHY 0.
        radio32(c.data+12,0xffffffffu);radio32(c.data+16,0xffffffffu);break;
    case IwlRadioStation:
        c.opcode=0x18;c.length=48;
        c.data[2]=0xff;c.data[3]=0xff; // All TX TIDs disabled; no TX queue is allocated.
        c.data[16]=2;c.data[35]=1; // Sniffer station 2, GENERAL_PURPOSE.
        radio32(c.data+24,0x3c000000u);break; // Narrow-band, SISO flag masks.
    default:return 1;
    }
    *out=c;return 0;
}
int iwl_radio_mcc_decode(const uint8_t *data,size_t bytes,IWL_MCC_INFO *out)
{
    IWL_MCC_INFO value;uint32_t i,count;
    if(!out) return 1;
    memset(out,0,sizeof(*out));memset(&value,0,sizeof(value));
    if(!data || bytes<20) return 1;
    count=radio_read32(data+16);
    if(count<6 || count>110 || bytes!=20u+4u*count) return 1;
    value.status=radio_read32(data);
    // NEW_CHAN_PROFILE and SAME_CHAN_PROFILE are the accepted query responses.
    if(value.status>1) return 1;
    value.mcc=(uint32_t)data[4]|(uint32_t)data[5]<<8;
    value.capabilities=(uint32_t)data[6]|(uint32_t)data[7]<<8;
    value.source=data[12];value.count=count;
    for(i=0;i<count;++i) value.channels[i]=radio_read32(data+20+4*i);
    *out=value;return 0;
}

static void radio64(uint8_t *p,uint64_t value)
{
    unsigned i;for(i=0;i<8;++i) p[i]=(uint8_t)(value>>(8u*i));
}
int iwl_radio_queue_command(uint64_t tfd,uint64_t counts,IWL_RADIO_COMMAND *out)
{
    if(!out) return 1;
    memset(out,0,sizeof(*out));
    if(!tfd || !counts || (tfd&4095u) || (counts&4095u) ||
        tfd>UINT64_MAX-65536u || counts>UINT64_MAX-4096u ||
        (tfd<counts+4096u && counts<tfd+65536u)) return 1;
    out->group=5;out->opcode=0x17;out->length=36;
    radio32(out->data+4,1u<<2);out->data[8]=15; // Sniffer STA 2, management TID.
    radio32(out->data+16,5); // log2(256 descriptors)-3.
    radio64(out->data+20,counts);radio64(out->data+28,tfd);
    return 0;
}
int iwl_radio_tx_completion(const uint8_t *data,size_t bytes,unsigned queue,
    unsigned index,IWL_TX_COMPLETION_INFO *out)
{
    IWL_TX_COMPLETION_INFO value;
    if(!out) return 1;
    memset(out,0,sizeof(*out));memset(&value,0,sizeof(value));
    if(!data || bytes!=48 || data[0]!=1 || !queue || queue>31 || index>65535) return 1;
    value.queue=(uint32_t)data[36]|(uint32_t)data[37]<<8;
    // Gen3 has a 16-bit hardware pointer, independent of the 256 allocated
    // descriptor slots. Do not truncate its completion to the storage index.
    value.next_index=radio_read32(data+44);
    if(value.queue!=queue || value.next_index!=((index+1u)&65535u)) return 1;
    value.status=((uint32_t)data[40]|(uint32_t)data[41]<<8)&0xffu;
    value.rate=radio_read32(data+4);
    value.byte_count=(uint32_t)data[30]|(uint32_t)data[31]<<8;
    value.retries=data[3];
    value.success=value.status==1 || value.status==2;
    *out=value;return 0;
}

static void radio16(uint8_t *p,unsigned value)
{
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);
}
int iwl_radio_ibss_command(unsigned id,const uint8_t address[6],IWL_RADIO_COMMAND *out)
{
    static const uint8_t bssid[6]={0,0x25,0,0xff,0x94,0x73};
    static const uint8_t rates[8]={0x82,0x84,0x8b,0x96,12,18,24,36};
    static const uint8_t more_rates[4]={48,72,96,108};
    static const unsigned cwmin[4]={15,15,7,3},cwmax[4]={1023,1023,15,7};
    static const uint8_t aifs[4]={7,3,2,2};
    static const unsigned txop[4]={0,0,3008,1504};
    IWL_RADIO_COMMAND c;uint8_t *p;unsigned n,i;
    if(!out) return 1;
    memset(out,0,sizeof(*out));memset(&c,0,sizeof(c));
    if(!address || (address[0]&3u)!=2u || id>=IwlIbssCommandCount) return 1;
    c.group=1;
    switch(id) {
    case IwlIbssBeacon:
        // Template v13, zero link ID (IBSS MAC 0), 6 Mbps OFDM.
        // Firmware chooses the antenna (capability 71); TSF is offloaded.
        c.opcode=0x91;p=c.data+32;
        p[0]=0x80;memset(p+4,0xff,6);memcpy(p+10,address,6);memcpy(p+16,bssid,6);
        radio16(p+32,10000);radio16(p+34,0x0422); // 10.24 s; IBSS, short preamble/slot.
        n=36;
        p[n++]=0;p[n++]=4;memcpy(p+n,"owly",4);n+=4;
        p[n++]=1;p[n++]=8;memcpy(p+n,rates,8);n+=8;
        p[n++]=50;p[n++]=4;memcpy(p+n,more_rates,4);n+=4;
        p[n++]=3;p[n++]=1;p[n++]=6; // DS channel.
        p[n++]=6;p[n++]=2;n+=2; // IBSS ATIM window 0.
        p[n++]=45;p[n++]=26;
        radio16(p+n,0x000c);p[n+3]=0xff; // HT20, SMPS off, MCS 0..7, max A-MSDU 3839.
        n+=26;
        p[n++]=61;p[n++]=22;p[n]=6;p[n+6]=0xff;n+=22; // HT operation, no secondary channel.
        p[n++]=221;p[n++]=7; // WMM information: QoS, no U-APSD.
        p[n++]=0;p[n++]=0x50;p[n++]=0xf2;p[n++]=2;p[n++]=0;p[n++]=1;p[n++]=0;
        radio16(c.data,n);c.length=32+n;
        break;
    case IwlIbssMac:
        c.opcode=0x28;c.length=148;radio32(c.data+4,1);radio32(c.data+8,4);
        memcpy(c.data+16,address,6);memcpy(c.data+24,bssid,6);
        radio32(c.data+32,15);radio32(c.data+36,0x15); // Mandatory CCK and OFDM ACK rates.
        radio32(c.data+44,0x20);radio32(c.data+48,0x10);
        radio32(c.data+52,0x1847); // Passive monitor RX on the existing IBSS context.
        radio32(c.data+56,3); // EDCA update and HT enabled.
        for(i=0;i<4;++i) {
            p=c.data+60+i*8;
            radio16(p,cwmin[i]);radio16(p+2,cwmax[i]);
            p[4]=aifs[i];p[5]=(uint8_t)(1u<<(i+1u));radio16(p+6,txop[i]);
        }
        radio32(c.data+112,10000); // IBSS beacon interval, template/link 0.
        break;
    case IwlIbssBinding:
        c.opcode=0x2b;c.length=28;radio32(c.data+4,1);
        radio32(c.data+12,UINT32_MAX);radio32(c.data+16,UINT32_MAX); // Only IBSS MAC 0 on PHY 0.
        break;
    case IwlIbssMulticast:
    case IwlIbssBroadcast:
        c.opcode=0x18;c.length=48;radio16(c.data+2,0xffff); // Disable aggregation.
        if(id==IwlIbssMulticast) {
            c.data[8]=3;c.data[16]=4;c.data[35]=2;
        } else {
            memcpy(c.data+8,bssid,6);c.data[16]=3;c.data[35]=1;
        }
        break;
    default:return 1;
    }
    if(c.length>sizeof(c.data)) return 1;
    *out=c;return 0;
}
int iwl_radio_station_queue(uint64_t tfd,uint64_t counts,unsigned station,
    unsigned tid,IWL_RADIO_COMMAND *out)
{
    if(!out) return 1;
    memset(out,0,sizeof(*out));
    if(station<2 || station>4 || (tid!=0 && tid!=15)) return 1;
    if(iwl_radio_queue_command(tfd,counts,out)) return 1;
    radio32(out->data+4,1u<<station);out->data[8]=(uint8_t)tid;
    return 0;
}
