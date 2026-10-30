// SPDX-License-Identifier: GPL-3.0-or-later AND ISC AND BSD-3-Clause
// Gen3 command layout adapted from OpenBSD iwx under its BSD license option.
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

#include "iwl_command.h"
#include <string.h>
static void c16(uint8_t *p,uint16_t value)
{
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);
}
static void c64(uint8_t *p,uint64_t value)
{
    unsigned i;for(i=0;i<8;++i) p[i]=(uint8_t)(value>>(i*8));
}
int iwl_command_build(uint8_t opcode, uint8_t group, uint8_t version,
    uint8_t index, uint64_t dma_address, const void *payload, size_t payload_bytes,
    void *command, size_t command_capacity, void *tfd, size_t tfd_capacity)
{
    uint8_t *cmd=(uint8_t *)command,*desc=(uint8_t *)tfd;
    size_t total=8u+payload_bytes;
    if(!command || !tfd || (!payload && payload_bytes) ||
        payload_bytes>IWL_COMMAND_MAX_PAYLOAD || command_capacity<total ||
        tfd_capacity<IWL_TFD_BYTES || !dma_address || (dma_address&4095u) ||
        dma_address>UINT64_MAX-4096u) return 1;
    // API >= 50 uses LONG_GROUP=1 even for legacy group-0 commands.
    memset(cmd,0,total);memset(desc,0,IWL_TFD_BYTES);
    cmd[0]=opcode;cmd[1]=group ? group : 1;cmd[2]=index;cmd[3]=0; // command queue
    c16(cmd+4,(uint16_t)payload_bytes);cmd[7]=version;
    if(payload_bytes) memcpy(cmd+8,payload,payload_bytes);
    c16(desc,total>20u ? 2 : 1);
    c16(desc+2,(uint16_t)(total>20u ? 20u : total));
    c64(desc+4,dma_address);
    if(total>20u) {
        c16(desc+12,(uint16_t)(total-20u));
        c64(desc+14,dma_address+20u);
    }
    return 0;
}

int iwl_beacon_command_build(uint8_t index, uint64_t dma_address,
    const void *payload, size_t payload_bytes, void *command,
    size_t command_capacity, void *tfd, size_t tfd_capacity)
{
    const uint8_t *input=(const uint8_t *)payload;
    uint8_t *desc=(uint8_t *)tfd;
    size_t frame_bytes;
    if(!input || payload_bytes<32u+36u ||
        payload_bytes>IWL_COMMAND_MAX_PAYLOAD) return 1;
    frame_bytes=payload_bytes-32u;
    if(((size_t)input[0]|(size_t)input[1]<<8)!=frame_bytes ||
        input[32]!=0x80u || input[33]!=0) return 1;
    if(iwl_command_build(0x91,1,0,index,dma_address,payload,payload_bytes,
        command,command_capacity,tfd,tfd_capacity)) return 1;
    // Wide header (8) + fixed beacon command (32), then the frame:
    // Linux gen2_enqueue_hcmd sends TB0=20, TB1=20, TB2=frame_bytes.
    c16(desc,3);
    c16(desc+12,20);
    c64(desc+14,dma_address+20u);
    c16(desc+22,(uint16_t)frame_bytes);
    c64(desc+24,dma_address+40u);
    return 0;
}
