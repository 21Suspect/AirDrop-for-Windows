// SPDX-License-Identifier: GPL-3.0-or-later AND ISC AND BSD-3-Clause
// Windows adaptation of the SO/GF power sequence in OpenBSD iwx.
// Original notices are preserved below; Intel's BSD license option is selected.
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

#include "iwl_hw.h"
#include <string.h>
#define CSR_HW_IF 0x000u
#define CSR_INT 0x008u
#define CSR_INT_MASK 0x00cu
#define CSR_FH_INT 0x010u
#define CSR_RESET 0x020u
#define CSR_GP 0x024u
#define CSR_HW_REV 0x028u
#define CSR_GIO 0x03cu
#define CSR_MBOX 0x088u
#define CSR_RF_ID 0x09cu
#define CSR_ICT 0x0a0u
#define CSR_CHICKEN 0x100u
#define CSR_HPET 0x240u
#define CSR_LINK 0x250u
#define CSR_MSIX_FH_MASK 0x2804u
#define CSR_MSIX_HW_MASK 0x280cu
#define NIC_READY 0x00400000u
#define NIC_PREPARE 0x08000000u
#define ENABLE_PME 0x10000000u
#define HAP_WAKE 0x00080000u
#define CLOCK_READY 0x1u
#define INIT_DONE 0x4u
#define MAC_ACCESS 0x8u
#define GOING_TO_SLEEP 0x10u
#define SW_RESET 0x80u
#define MASTER_DISABLED 0x100u
#define STOP_MASTER 0x200u
#define LINK_PM_DISABLED 0x80000000u

typedef IWL_HW_STATE HW;
static uint32_t rd(HW *hw, uint32_t reg)
{
    uint32_t value=hw->io->read32(hw->io->context,reg);
    if (value==UINT32_MAX) hw->removed=1;
    return value;
}
static void wr(HW *hw, uint32_t reg, uint32_t value)
{
    if (hw->removed) return;
    hw->io->write32(hw->io->context,reg,value);
    hw->result->register_writes++;
}
static void change(HW *hw, uint32_t reg, uint32_t set, uint32_t clear)
{
    uint32_t value=rd(hw,reg);
    wr(hw,reg,(value & ~clear) | set);
}
static void delay(HW *hw, uint32_t usec)
{
    hw->io->delay_us(hw->io->context,usec);
}
static int poll(HW *hw, uint32_t reg, uint32_t mask, uint32_t bits, uint32_t usec)
{
    uint64_t begin=hw->io->now_us(hw->io->context);
    uint32_t i;
    for(i=0;i<=usec/10u;++i) {
        uint32_t value=rd(hw,reg);
        if(hw->removed) return 0;
        if((value & mask)==bits) return 1;
        if(hw->io->now_us(hw->io->context)-begin>=usec) break;
        delay(hw,10);
    }
    return 0;
}
static void mask_interrupts(HW *hw)
{
    wr(hw,CSR_INT_MASK,0);
    wr(hw,CSR_MSIX_FH_MASK,UINT32_MAX);
    wr(hw,CSR_MSIX_HW_MASK,UINT32_MAX);
    wr(hw,CSR_ICT,0);
    wr(hw,CSR_INT,UINT32_MAX);
    wr(hw,CSR_FH_INT,UINT32_MAX);
}
static int ready(HW *hw)
{
    change(hw,CSR_HW_IF,NIC_READY,0);
    if(!poll(hw,CSR_HW_IF,NIC_READY,NIC_READY,50)) return 0;
    change(hw,CSR_MBOX,0x20u,0);
    return !hw->removed;
}
static int start(HW *hw)
{
    uint32_t attempt, elapsed;
    if(!ready(hw)) {
        change(hw,CSR_LINK,LINK_PM_DISABLED,0);
        delay(hw,1000);
        for(attempt=0;attempt<10 && !hw->removed;++attempt) {
            change(hw,CSR_HW_IF,NIC_PREPARE,0);
            for(elapsed=0;elapsed<150000;elapsed+=200) {
                if(ready(hw)) goto owned;
                if(hw->removed) return IwlHwRemoved;
                delay(hw,200);
            }
            delay(hw,25000);
        }
        return hw->removed ? IwlHwRemoved : IwlHwOwnershipTimeout;
    }
owned:
    hw->owned=1;
    mask_interrupts(hw);
    change(hw,CSR_RESET,SW_RESET,0);
    delay(hw,5000);
    mask_interrupts(hw);
    change(hw,CSR_CHICKEN,0x00800000u,0);
    change(hw,CSR_HPET,0xffff0000u,0);
    change(hw,CSR_HW_IF,HAP_WAKE,0);
    change(hw,CSR_GIO,0x2u,0);
    change(hw,CSR_GP,INIT_DONE,0);
    if(!poll(hw,CSR_GP,CLOCK_READY,CLOCK_READY,25000))
        return hw->removed ? IwlHwRemoved : IwlHwClockTimeout;
    change(hw,CSR_GP,MAC_ACCESS,0);
    delay(hw,2);
    if(!poll(hw,CSR_GP,CLOCK_READY|GOING_TO_SLEEP,CLOCK_READY,150000))
        return hw->removed ? IwlHwRemoved : IwlHwAccessTimeout;
    hw->result->gp_active=rd(hw,CSR_GP);
    change(hw,CSR_GP,0,MAC_ACCESS);
    return hw->removed ? IwlHwRemoved : IwlHwOk;
}
static int stop(HW *hw)
{
    int stopped;
    if(hw->removed) return IwlHwRemoved;
    if(!hw->owned) return IwlHwNotStarted;
    mask_interrupts(hw);
    change(hw,CSR_GP,0,MAC_ACCESS);
    change(hw,CSR_LINK,LINK_PM_DISABLED,0);
    change(hw,CSR_HW_IF,NIC_PREPARE|ENABLE_PME,0);
    delay(hw,1000);
    change(hw,CSR_LINK,0,LINK_PM_DISABLED);
    delay(hw,5000);
    change(hw,CSR_RESET,STOP_MASTER,0);
    stopped=poll(hw,CSR_RESET,MASTER_DISABLED,MASTER_DISABLED,5000);
    hw->result->reset_stopped=rd(hw,CSR_RESET);
    change(hw,CSR_GP,0,INIT_DONE);
    change(hw,CSR_RESET,SW_RESET,0);
    delay(hw,5000);
    mask_interrupts(hw);
    hw->result->gp_after=rd(hw,CSR_GP);
    return hw->removed ? IwlHwRemoved : (stopped ? IwlHwOk : IwlHwStopTimeout);
}
int iwl_hw_begin(IWL_HW_STATE *hw, const IWL_HW_IO *io, IWL_HW_RESULT *result)
{
    if(!hw || !result) return IwlHwInvalid;
    memset(hw,0,sizeof(*hw));
    memset(result,0,sizeof(*result));
    result->size=sizeof(*result);
    result->version=1;
    result->start_error=IwlHwInvalid;
    result->stop_error=IwlHwNotStarted;
    if(!io || !io->read32 || !io->write32 || !io->delay_us || !io->now_us)
        return IwlHwInvalid;
    hw->io=io;hw->result=result;
    hw->started_us=io->now_us(io->context);
    result->hw_revision=rd(hw,CSR_HW_REV);
    result->rf_id=rd(hw,CSR_RF_ID);
    if(hw->removed || result->hw_revision!=0x430u || result->rf_id!=0x2010d000u) {
        result->start_error=hw->removed ? IwlHwRemoved : IwlHwWrongDevice;
        return (int)result->start_error;
    }
    result->gp_before=rd(hw,CSR_GP);
    result->start_error=(uint32_t)start(hw);
    return (int)result->start_error;
}
int iwl_hw_end(IWL_HW_STATE *hw)
{
    if(!hw || !hw->io || !hw->result) return IwlHwInvalid;
    hw->result->stop_error=(uint32_t)stop(hw);
    hw->result->elapsed_us=(uint32_t)(hw->io->now_us(hw->io->context)-hw->started_us);
    if(hw->result->stop_error==IwlHwOk || hw->result->stop_error==IwlHwRemoved)
        hw->owned=0;
    return (int)hw->result->stop_error;
}
int iwl_hw_power_cycle(const IWL_HW_IO *io, IWL_HW_RESULT *result)
{
    IWL_HW_STATE hw;
    int begin,finish;
    begin=iwl_hw_begin(&hw,io,result);
    if(begin==IwlHwInvalid) return begin;
    finish=iwl_hw_end(&hw);
    return begin ? begin : finish;
}
