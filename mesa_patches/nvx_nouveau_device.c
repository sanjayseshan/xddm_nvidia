#include "nvx_nouveau_device.h"

#include <string.h>

#ifdef _WIN32
#include "nvx_winsys_win32.h"
#endif

int
NvxNouveauDeviceInitFromWinsys(NVX_NOUVEAU_DEVICE *device,
                               const NVX_WINSYS_DEVICE *winsys)
{
   uint32_t chipset;

   if (!device || !winsys || !winsys->escape || !winsys->opaque ||
       winsys->caps.Version != NVX_3D_ABI_VERSION ||
       winsys->caps.Size != sizeof(winsys->caps) ||
       winsys->caps.GraphicsClass != 0xc797U ||
       !(winsys->caps.Features & NVX_3D_FEATURE_GR_CHANNEL) ||
       !(winsys->caps.Features & NVX_3D_FEATURE_USER_ALLOC) ||
       !(winsys->caps.Features & NVX_3D_FEATURE_UPLOAD) ||
       !(winsys->caps.Features & NVX_3D_FEATURE_DOWNLOAD) ||
       !winsys->caps.VramApertureBytes)
      return 0;

   chipset = Nvx3dChipsetFromDeviceId(winsys->caps.DeviceId);
   if (!chipset || winsys->caps.Chipset != chipset)
      return 0;

   memset(device, 0, sizeof(*device));
   device->winsys = *winsys;
   device->device_id = winsys->caps.DeviceId;
   device->chipset = chipset;
   device->class_3d = winsys->caps.GraphicsClass;
   device->vram_aperture_bytes = winsys->caps.VramApertureBytes;
   device->gart_size = winsys->caps.GartBytes;
   return 1;
}

#ifdef _WIN32
int
NvxNouveauDeviceInitHdc(NVX_NOUVEAU_DEVICE *device, HDC hdc)
{
   NVX_WINSYS_DEVICE winsys;

   if (!NvxWinsysInitHdc(&winsys, hdc))
      return 0;
   return NvxNouveauDeviceInitFromWinsys(device, &winsys);
}
#endif
