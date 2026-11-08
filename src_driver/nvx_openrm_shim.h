/*
 * nvx_openrm_shim.h - Hardware Abstraction Layer shim mapping VideoPort / VMM
 * primitives to NVIDIA OpenRM conventions.
 *
 * Designed for portable compilation on Windows XP x64 (64-bit),
 * Windows 2000 / XP (32-bit XDDM), and Windows 98 SE (32-bit VxD).
 */

#ifndef NVX_OPENRM_SHIM_H
#define NVX_OPENRM_SHIM_H

#include "nvtypes.h"
#include "nvstatus.h"
#include "include/nvx_nvidia_open_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register read/write abstraction */
NvU32 nvx_reg_read32(const void *base, NvU32 offset);
void  nvx_reg_write32(void *base, NvU32 offset, NvU32 val);
NvU8  nvx_reg_read08(const void *base, NvU32 offset);
void  nvx_reg_write08(void *base, NvU32 offset, NvU8 val);

/* Convenience macros matching OpenRM conventions */
#define GPU_REG_RD32(base, offset)        nvx_reg_read32((base), (offset))
#define GPU_REG_WR32(base, offset, val)   nvx_reg_write32((base), (offset), (val))
#define GPU_REG_RD08(base, offset)        nvx_reg_read08((base), (offset))
#define GPU_REG_WR08(base, offset, val)   nvx_reg_write08((base), (offset), (val))

/* Delays and timing */
void  nvx_stall_us(NvU32 microseconds);
void  nvx_delay_ms(NvU32 milliseconds);

/* Memory operations */
void  nvx_mem_copy(void *dst, const void *src, NvLength bytes);
void  nvx_mem_zero(void *dst, NvLength bytes);

/* OpenRM MsgQ Doorbell Notification callback */
int   nvx_msgq_notify_remote(int isRead, void *pArg);

/* Falcon engine reset and polling helper */
NvBool nvx_falcon_wait_idle(const void *bar0, NvU32 falconBase, NvU32 timeoutUs);

#ifdef __cplusplus
}
#endif

#endif /* NVX_OPENRM_SHIM_H */
