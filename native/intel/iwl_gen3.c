// SPDX-License-Identifier: GPL-3.0-or-later AND ISC AND BSD-3-Clause
// Gen3 wire structures adapted from OpenBSD iwx under its BSD license option.
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

#include "iwl_gen3.h"
#include <string.h>
_Static_assert(sizeof(IWL_GEN3_CONTEXT)==104,"Gen3 context wire size");
_Static_assert(offsetof(IWL_GEN3_CONTEXT,mtr)==52,"Gen3 packed command address");
_Static_assert(offsetof(IWL_GEN3_CONTEXT,scratch)==88,"Gen3 scratch pointer");
_Static_assert(sizeof(IWL_GEN3_SCRATCH)==1660,"Gen3 scratch wire size");
_Static_assert(offsetof(IWL_GEN3_SCRATCH,umac)==124,"Gen3 DRAM table offset");
_Static_assert(sizeof(IWL_RX_TRANSFER)==16,"Gen3 RX descriptor size");
_Static_assert(sizeof(IWL_GEN3_LAYOUT)==9956,"Output layout size");

int iwl_gen3_build(const IWL_FW_PLAN *plan, const IWL_GEN3_INPUT *input,
    void *output, size_t output_length)
{
    static const uint32_t minimum[10]={104,1660,4096,8192,16384,4096,65536,
        1048576,1048576,0};
    IWL_GEN3_LAYOUT *layout=(IWL_GEN3_LAYOUT *)output;
    uint32_t i,j,counts[3]={0,0,0};
    if(!plan || !input || !output || output_length<sizeof(*layout))
        return IwlGen3BadArgument;
    if(plan->version_major!=77 || plan->cpu_count!=2 ||
        !plan->section_count || plan->section_count>IWL_FW_MAX_SECTIONS ||
        !plan->iml_length || plan->iml_length>65536 ||
        input->count!=IwlGen3Firmware+plan->section_count)
        return IwlGen3BadPlan;
    for(i=0;i<plan->section_count;++i) {
        const IWL_FW_SECTION *section=&plan->sections[i];
        if(section->group>2 || !section->length || section->length>IWL_FW_SECTION_BYTES)
            return IwlGen3BadPlan;
        if(i && section->group<plan->sections[i-1].group) return IwlGen3BadPlan;
        if(++counts[section->group]>64) return IwlGen3BadPlan;
    }
    for(i=0;i<3;++i)
        if(!counts[i] || counts[i]!=plan->group_count[i]) return IwlGen3BadPlan;
    for(i=0;i<input->count;++i) {
        const IWL_DMA_REGION *region=&input->regions[i];
        uint32_t needed=i<IwlGen3Iml ? minimum[i] :
            (i==IwlGen3Iml ? plan->iml_length : plan->sections[i-IwlGen3Firmware].length);
        if(!region->address || (region->address & 4095u) ||
            region->address>UINT64_MAX-region->length) return IwlGen3BadAddress;
        if(region->length<needed) return IwlGen3TooSmall;
        for(j=0;j<i;++j) {
            const IWL_DMA_REGION *previous=&input->regions[j];
            if(region->address<previous->address+previous->length &&
                previous->address<region->address+region->length) return IwlGen3Overlap;
        }
    }
    memset(layout,0,sizeof(*layout));
    layout->scratch.mac_id=0x430;
    layout->scratch.size_dwords=sizeof(layout->scratch)/4;
    layout->scratch.control=0x000f0000u; // 4K RX, legacy responses, 256-byte TFD.
    layout->scratch.rx_free=input->regions[IwlGen3RxFree].address;
    memset(counts,0,sizeof(counts));
    for(i=0;i<plan->section_count;++i) {
        uint32_t group=plan->sections[i].group;
        uint64_t address=input->regions[IwlGen3Firmware+i].address;
        uint32_t index=counts[group]++;
        if(group==0) layout->scratch.lmac[index]=address;
        else if(group==1) layout->scratch.umac[index]=address;
        else layout->scratch.paging[index]=address;
    }
    layout->context.prph_info=input->regions[IwlGen3Info].address;
    layout->context.cr_head=input->regions[IwlGen3RxStatus].address;
    layout->context.tr_tail=input->regions[IwlGen3Info].address+2048;
    layout->context.cr_tail=input->regions[IwlGen3Info].address+3072;
    layout->context.mtr=input->regions[IwlGen3TxCmd].address;
    layout->context.mcr=input->regions[IwlGen3RxUsed].address;
    layout->context.mtr_size=5; // log2(256)-3
    layout->context.mcr_size=9; // log2(512)
    layout->context.scratch=input->regions[IwlGen3Scratch].address;
    layout->context.scratch_bytes=sizeof(layout->scratch);
    for(i=0;i<IWL_RX_COUNT;++i) {
        layout->rx[i].id=(uint16_t)i;
        layout->rx[i].address=input->regions[i<256 ? IwlGen3RxDataA : IwlGen3RxDataB].address+
            (uint64_t)(i%256)*IWL_RX_BYTES;
    }
    return IwlGen3Ok;
}

static uint32_t gen3_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int iwl_gen3_decode_alive(const uint8_t *data, size_t length, IWL_ALIVE_INFO *alive)
{
    uint32_t bytes,payload;
    if(!alive) return 2;
    memset(alive,0,sizeof(*alive));
    if(!data || length<8) return 2;
    bytes=gen3_le32(data);
    if(bytes==0x55550000u) return 2;
    bytes &= 0x3fffu;
    if(bytes<4 || bytes>length-4) return 2;
    if(data[4]!=1 || data[5]!=0) return 1;
    payload=bytes-4;
    if(payload!=116 && payload!=128 && payload!=144) return 2;
    alive->status=(uint32_t)data[8] | (uint32_t)data[9]<<8;
    alive->payload_bytes=payload;
    alive->lmac_major=gen3_le32(data+12);
    alive->lmac_minor=gen3_le32(data+16);
    alive->lmac_error=gen3_le32(data+28);
    alive->umac_major=gen3_le32(data+108);
    alive->umac_minor=gen3_le32(data+112);
    alive->umac_error=gen3_le32(data+116);
    if(payload>=128) {
        alive->sku[0]=gen3_le32(data+124);
        alive->sku[1]=gen3_le32(data+128);
        alive->sku[2]=gen3_le32(data+132);
    }
    return 0;
}

int iwl_gen3_error_log_range(uint32_t address, unsigned dwords)
{
    uint32_t offset=address&0x7fffffffu;
    return (dwords==38 || dwords==15) && !(address&3u) &&
        offset>=0x400000u && offset<=0x1000000u-dwords*4u;
}
