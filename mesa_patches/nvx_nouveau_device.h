#ifndef NVX_NOUVEAU_DEVICE_H
#define NVX_NOUVEAU_DEVICE_H

#include "nvx_winsys.h"

typedef struct NVX_NOUVEAU_DEVICE {
   NVX_WINSYS_DEVICE winsys;
   uint32_t device_id;
   uint32_t chipset;
   uint32_t class_3d;
   uint64_t vram_aperture_bytes;
   uint64_t gart_size;
} NVX_NOUVEAU_DEVICE;

int NvxNouveauDeviceInitFromWinsys(NVX_NOUVEAU_DEVICE *device,
                                   const NVX_WINSYS_DEVICE *winsys);

#ifdef _WIN32
#include <windows.h>
int NvxNouveauDeviceInitHdc(NVX_NOUVEAU_DEVICE *device, HDC hdc);
#endif

#endif
