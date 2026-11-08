/*
 * nvx_openrm_shim.c - Hardware Abstraction Layer shim mapping VideoPort / VMM
 * primitives to NVIDIA OpenRM conventions.
 *
 * Implements unified register access, timing, memory manipulation, and
 * OpenRM MsgQ remote notification hooks for Windows XP/2000 and Win98.
 */

#include "miniport.h"
#include "nvx_openrm_shim.h"
#include "nvx_gsp.h"

NvU32 nvx_reg_read32(const void *base, NvU32 offset)
{
    if (!base) return 0xFFFFFFFFU;
    return VideoPortReadRegisterUlong((PULONG)((const UCHAR *)base + offset));
}

void nvx_reg_write32(void *base, NvU32 offset, NvU32 val)
{
    if (!base) return;
    VideoPortWriteRegisterUlong((PULONG)((UCHAR *)base + offset), val);
}

NvU8 nvx_reg_read08(const void *base, NvU32 offset)
{
    if (!base) return 0xFFU;
    return VideoPortReadRegisterUchar((PUCHAR)((const UCHAR *)base + offset));
}

void nvx_reg_write08(void *base, NvU32 offset, NvU8 val)
{
    if (!base) return;
    VideoPortWriteRegisterUchar((PUCHAR)((UCHAR *)base + offset), val);
}

void nvx_stall_us(NvU32 microseconds)
{
    VideoPortStallExecution(microseconds);
}

void nvx_delay_ms(NvU32 milliseconds)
{
    NvU32 i;
    for (i = 0; i < milliseconds; i++) {
        VideoPortStallExecution(1000);
    }
}

void nvx_mem_copy(void *dst, const void *src, NvLength bytes)
{
    if (!dst || !src || !bytes) return;
    VideoPortMoveMemory(dst, (PVOID)src, (ULONG)bytes);
}

void nvx_mem_zero(void *dst, NvLength bytes)
{
    if (!dst || !bytes) return;
    VideoPortZeroMemory(dst, (ULONG)bytes);
}

int nvx_msgq_notify_remote(int isRead, void *pArg)
{
    if (isRead == FCN_FLAG_NOTIFY_MSG_WRITE && pArg != NULL) {
        PNVX_DEVICE_EXTENSION dev = (PNVX_DEVICE_EXTENSION)pArg;
        if (dev->Bar0MmioVirtual != NULL) {
            VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual + 0x110c00), 0);
        }
    }
    return 0;
}

NvBool nvx_falcon_wait_idle(const void *bar0, NvU32 falconBase, NvU32 timeoutUs)
{
    NvU32 elapsed = 0;
    if (!bar0) return NV_FALSE;

    while (elapsed < timeoutUs) {
        /* NV_PFALCON_FALCON_DMACTL (0x10c) bit 1 (DMEM scrubbing) and bit 2 (IMEM scrubbing) */
        NvU32 dmactl = nvx_reg_read32(bar0, falconBase + 0x10c);
        if ((dmactl & 0x6) == 0) {
            return NV_TRUE;
        }
        nvx_stall_us(10);
        elapsed += 10;
    }
    return NV_FALSE;
}
