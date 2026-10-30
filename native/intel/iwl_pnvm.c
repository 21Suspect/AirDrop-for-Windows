// SPDX-License-Identifier: GPL-3.0-or-later AND ISC AND BSD-3-Clause
// PNVM selection adapted from OpenBSD iwx under its BSD license option.
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

#include "iwl_pnvm.h"
#include <string.h>
static uint32_t p32(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint16_t p16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8);
}
static int finish(const IWL_PNVM_PLAN *candidate, int selected, int matched,
    int version_seen, int *found, IWL_PNVM_PLAN *best)
{
    if(!selected || !matched) return IwlPnvmOk;
    if(!version_seen || !candidate->section_count) return IwlPnvmInvalid;
    if(*found) return IwlPnvmAmbiguous;
    *best=*candidate;*found=1;
    return IwlPnvmOk;
}
int iwl_pnvm_parse(const uint8_t *data, size_t length, const uint32_t sku[3],
    uint16_t mac_type, uint16_t rf_type, IWL_PNVM_PLAN *plan)
{
    IWL_PNVM_PLAN candidate, best;
    size_t cursor=0;
    unsigned records=0;
    int in_section=0,selected=0,matched=0,version_seen=0,found=0,code;
    if(!plan) return IwlPnvmInvalid;
    memset(plan,0,sizeof(*plan));
    if(!data || !sku || !length) return IwlPnvmInvalid;
    if(length>IWL_PNVM_MAX_BYTES) return IwlPnvmLimit;
    memset(&candidate,0,sizeof(candidate));memset(&best,0,sizeof(best));
    while(cursor<length) {
        uint32_t type,bytes;
        size_t payload,padded;
        const uint8_t *p;
        if(++records>4096) return IwlPnvmLimit;
        if(length-cursor<8) return IwlPnvmTruncated;
        type=p32(data+cursor);bytes=p32(data+cursor+4);
        payload=cursor+8;
        if(bytes>length-payload) return IwlPnvmTruncated;
        padded=(size_t)bytes+((4u-(bytes&3u))&3u);
        if(padded>length-payload) return IwlPnvmTruncated;
        p=data+payload;
        if(type==64) {
            if(bytes!=12) return IwlPnvmInvalid;
            code=finish(&candidate,selected,matched,version_seen,&found,&best);
            if(code) return code;
            memset(&candidate,0,sizeof(candidate));
            candidate.sku[0]=p32(p);candidate.sku[1]=p32(p+4);candidate.sku[2]=p32(p+8);
            selected=memcmp(candidate.sku,sku,sizeof(candidate.sku))==0;
            in_section=1;matched=0;version_seen=0;
        } else if(type==58) {
            if(!in_section || bytes<4) return IwlPnvmInvalid;
            if(p16(p)==mac_type && p16(p+2)==rf_type) matched=1;
        } else if(type==62) {
            if(!in_section || bytes!=4 || version_seen) return IwlPnvmInvalid;
            candidate.version=p32(p);version_seen=1;
        } else if(type==19) {
            IWL_PNVM_SECTION *section;
            if(!in_section || bytes<4) return IwlPnvmInvalid;
            if(p32(p)!=0xddddeeeeu) {
                if(bytes==4) return IwlPnvmInvalid;
                if(candidate.section_count==IWL_PNVM_MAX_SECTIONS ||
                    bytes-4>IWL_PNVM_MAX_BYTES-candidate.data_bytes) return IwlPnvmLimit;
                section=&candidate.sections[candidate.section_count++];
                section->file_offset=(uint32_t)payload+4;
                section->length=bytes-4;
                candidate.data_bytes+=bytes-4;
            }
        }
        cursor=payload+padded;
    }
    code=finish(&candidate,selected,matched,version_seen,&found,&best);
    if(code) return code;
    if(!found) return IwlPnvmNoMatch;
    *plan=best;
    return IwlPnvmOk;
}
