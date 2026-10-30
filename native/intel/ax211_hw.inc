// SPDX-License-Identifier: GPL-3.0-or-later
#include "iwl_hw.h"

static uint32_t Ax211HwRead(void *context, uint32_t offset)
{
    AX211_KM_CONTEXT *ctx=(AX211_KM_CONTEXT *)context;
    if (!ctx->registers || ctx->length<4u || (offset & 3u) || offset>ctx->length-4u)
        return UINT32_MAX;
    return READ_REGISTER_ULONG((PULONG)(ctx->registers+offset));
}
static void Ax211HwWrite(void *context, uint32_t offset, uint32_t value)
{
    AX211_KM_CONTEXT *ctx=(AX211_KM_CONTEXT *)context;
    if (!ctx->registers || ctx->length<4u || (offset & 3u) || offset>ctx->length-4u) return;
    WRITE_REGISTER_ULONG((PULONG)(ctx->registers+offset),value);
}
static void Ax211HwDelay(void *context, uint32_t usec)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(context);
    if(usec<=50) { KeStallExecutionProcessor(usec); return; }
    interval.QuadPart=-(LONGLONG)usec*10;
    KeDelayExecutionThread(KernelMode,FALSE,&interval);
}
static uint64_t Ax211HwNow(void *context)
{
    UNREFERENCED_PARAMETER(context);
    return KeQueryInterruptTime()/10u;
}
static void Ax211HwPowerCycle(AX211_KM_CONTEXT *ctx, IWL_HW_RESULT *result)
{
    IWL_HW_IO io;
    io.context=ctx;io.read32=Ax211HwRead;io.write32=Ax211HwWrite;
    io.delay_us=Ax211HwDelay;io.now_us=Ax211HwNow;
    (void)iwl_hw_power_cycle(&io,result);
}
