#ifndef NVX_NVIDIA_OPEN_SHIM_H
#define NVX_NVIDIA_OPEN_SHIM_H

/*
 * Shim header integrating NVIDIA open-gpu-kernel-modules official SDK definitions
 * into the NT/XP VideoPort miniport (64-bit) and Win98 VxD (32-bit) builds.
 */

#include "nvtypes.h"
#include "nvstatus.h"
#include "nverror.h"
#include "nvos.h"

/* Official SDK Command & Class definitions */
#include "class/clc57e.h"
#include "class/cla06f.h"
#include "class/clc36f.h"
#include "class/clc56f.h"
#include "class/cla06c.h"
#include "class/cl9067.h"
#include "class/clc697.h"
#include "class/clc797.h"
#include "class/cl2080_notification.h"

#include "ctrl/ctrl0073.h"
#include "ctrl/ctrl0080.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080.h"
#include "ctrl/ctrl5070/ctrl5070system.h"
#include "ctrl/ctrla06f.h"
#include "ctrl/ctrlc36f.h"
#include "ctrl/ctrl906f.h"
#include "ctrl/ctrlb06f.h"
#include "alloc/alloc_channel.h"

/* Display & Coprocessor Register definitions from swref */
#include "swref/published/disp/v03_00/dev_disp.h"
#include "swref/published/ampere/ga102/dev_gsp.h"
#include "swref/published/ampere/ga102/dev_falcon_v4.h"
#include "swref/published/ampere/ga102/dev_falcon_v4_addendum.h"
#include "swref/published/ampere/ga102/dev_fbif_v4.h"
#include "swref/published/ampere/ga102/dev_riscv_pri.h"
#include "swref/published/ampere/ga102/dev_sec_pri.h"

/* PBDMA register offsets are relative to each BAR0 offset published by the
 * graphics runlist's PBDMA_CONFIG table (see GA100 dev_runlist.h). */
#define NVX_PPBDMA_REG_BASE            0x00040000U
#define NVX_PPBDMA_GP_PUT_REL          0x00000000U
#define NVX_PPBDMA_STATUS_REL          0x00000100U
#define NVX_PPBDMA_GP_GET_REL          0x00000014U
#define NVX_PPBDMA_TOP_LEVEL_GET_REL   0x00000020U
#define NVX_PPBDMA_TOP_LEVEL_GET_HI_REL 0x00000024U
#define NVX_PPBDMA_GP_FETCH_REL        0x00000050U
#define NVX_PPBDMA_PB_FETCH_REL        0x00000054U
#define NVX_PPBDMA_PB_FETCH_HI_REL     0x00000058U
#define NVX_PPBDMA_GP_BASE_REL         0x00000048U
#define NVX_PPBDMA_GP_BASE_HI_REL      0x0000004cU
#define NVX_PPBDMA_GP_CRC_REL          0x00000074U
#define NVX_PPBDMA_PB_GET_REL          0x00000018U
#define NVX_PPBDMA_PB_GET_HI_REL       0x0000001cU
#define NVX_PPBDMA_PB_PUT_REL          0x0000005cU
#define NVX_PPBDMA_PB_PUT_HI_REL       0x00000060U
#define NVX_PPBDMA_PB_HEADER_REL       0x00000084U
#define NVX_PPBDMA_PB_COUNT_REL        0x00000088U
#define NVX_PPBDMA_PB_CRC_REL          0x00000098U
#define NVX_PPBDMA_TARGET_REL          0x000000acU
#define NVX_PPBDMA_METHOD0_REL         0x000000c0U
#define NVX_PPBDMA_DATA0_REL           0x000000c4U
#define NVX_PPBDMA_INTR_0_REL          0x00000108U
#define NVX_PPBDMA_INTR_1_REL          0x00000148U
#define NVX_GR_RUNLIST_PBDMA_CONFIG_COUNT 2U
#define NVX_GR_RUNLIST_PBDMA_CONFIG(i) (0x00000010U+(i)*4U)
#define NVX_GR_RUNLIST_PBDMA_CONFIG_VALID (1U<<31)

/* Published RAMFC_TARGET fields (Turing/GA10x compatible). PBDMA TARGET
 * mirrors these fields when a channel context is loaded. */
#define NVX_RAMFC_TARGET_OFFSET         (43U * 4U)
#define NVX_RAMFC_TARGET_ENGINE_MASK    0x0000001fU
#define NVX_RAMFC_TARGET_ENG_CTX_VALID  (1U << 16)
#define NVX_RAMFC_TARGET_CE_CTX_VALID   (1U << 17)

/* Helper macro mapping for Falcon engine apertures */
#define NVX_GSP_BASE                   0x00110000
#define NVX_SEC2_BASE                  0x00840000
#define NVX_GSP_RISCV_BASE             0x00111000

#define NVX_GSP_REG(offset)            (NVX_GSP_BASE + (offset))
#define NVX_SEC2_REG(offset)           (NVX_SEC2_BASE + (offset))
#define NVX_GSP_RISCV_REG(offset)      (NVX_GSP_RISCV_BASE + (offset))

/* Official Upstream OpenRM GSP Metadata & Layout definitions */
#include "gsp/gsp_fw_wpr_meta.h"

/* Official Upstream OpenRM Message Queue definitions */
#include "msgq/msgq.h"
#include "msgq/msgq_priv.h"

/* Upstream OpenRM VBIOS BIT (BIOS Information Table) definitions */
#define NV_VBIOS_BIT_HEADER_ID                     0xB8FF
#define NV_VBIOS_BIT_HEADER_SIGNATURE              0x00544942  /* "BIT\0" */
#define NV_VBIOS_BIT_HEADER_SIZE_OFFSET            8

#define NV_VBIOS_BIT_TOKEN_BIOSDATA                0x42
#define NV_VBIOS_BIT_TOKEN_FALCON_DATA             0x70

#define NV_VBIOS_FALCON_UCODE_ENTRY_APPID_SEC_LIC  0x05
#define NV_VBIOS_FALCON_UCODE_ENTRY_APPID_FWSEC_PR 0x85

#define NV_VBIOS_BCRT30_RSA3K_SIG_SIZE             384

/* Upstream OpenRM GPU Device Database */
#include "include/nvx_gpu_table.h"

#endif /* NVX_NVIDIA_OPEN_SHIM_H */
