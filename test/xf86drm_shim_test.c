#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xf86drm.h"
#include "drm-uapi/nouveau_drm.h"
#include "nvif/class.h"
#include "nvif/cl0080.h"
#include "nvif/ioctl.h"

static int
mock_escape(void *opaque, uint32_t escape, uint32_t input_bytes,
            const void *input, uint32_t output_bytes, void *output)
{
   (void)opaque;
   (void)escape;
   (void)input_bytes;
   (void)input;
   (void)output_bytes;
   (void)output;
   return 0;
}

int
main(void)
{
   drmDevicePtr device = NULL;
   uint32_t handle = 123;
   int prime_fd = 123;
   NVX_WINSYS_DEVICE winsys;
   int fd;

   assert(drmGetVersion(1) == NULL);
   assert(errno == ENODEV);
   assert(drmGetDevice2(1, 0, &device) == -1);
   assert(errno == ENODEV && device == NULL);
   assert(drmCommandWriteRead(1, 0, NULL, 0) == -1);
   assert(errno == ENOSYS);
   assert(drmCommandWrite(1, 0, NULL, 0) == -1);
   assert(errno == ENOSYS);
   assert(drmIoctl(1, 0, NULL) == -1);
   assert(errno == ENOSYS);
   assert(drmCloseBufferHandle(1, 1) == -1);
   assert(errno == ENOSYS);
   assert(drmPrimeFDToHandle(1, 1, &handle) == -1);
   assert(errno == ENOSYS && handle == 0);
   assert(drmPrimeHandleToFD(1, 1, 0, &prime_fd) == -1);
   assert(errno == ENOSYS && prime_fd == -1);
   drmFreeDevice(&device);
   assert(device == NULL);

   memset(&winsys, 0, sizeof(winsys));
   winsys.escape = mock_escape;
   winsys.opaque = &winsys;
   winsys.caps.Version = NVX_3D_ABI_VERSION;
   winsys.caps.Size = sizeof(winsys.caps);
   winsys.caps.GraphicsClass = 0xc797U;
   winsys.caps.DeviceId = 0x2488U;
   winsys.caps.Chipset = 0x170U;
   winsys.caps.VramApertureBytes = 64U * 1024U * 1024U;
   winsys.caps.MaxBufferBytes = 16U * 1024U * 1024U;
   fd = NvxXf86DrmRegisterDevice(&winsys);
   assert(fd > 0 && NvxXf86DrmIsRegistered(fd));

   {
      drmVersionPtr version = drmGetVersion(fd);
      assert(version && version->version_major == 0 &&
             version->version_minor == 0 && version->version_patchlevel == 0 &&
             version->name_len == 3 && strcmp(version->name, "nvx") == 0);
      drmFreeVersion(version);
   }
   assert(drmGetDevice2(fd, 0, &device) == 0);
   assert(device && device->bustype == DRM_BUS_PLATFORM);
   assert(strcmp(device->businfo.platform->fullname,
                 "NVX HDC adapter PCI 2488 chipset 0170") == 0);
   assert(strcmp(device->deviceinfo.platform->compatible[0],
                 "nvx,nvidia-ga104-0170") == 0);
   drmFreeDevice(&device);

   {
      struct drm_nouveau_getparam getparam = {
         .param = NOUVEAU_GETPARAM_FB_SIZE,
         .value = 0
      };
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM, &getparam,
                                 sizeof(getparam)) == 0);
      assert(getparam.value == winsys.caps.VramApertureBytes);
      getparam.param = 0xffffffffU;
      errno = 0;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM, &getparam,
                                 sizeof(getparam)) == -1);
      assert(errno == ENOSYS);
   }

   {
      struct {
         struct nvif_ioctl_v0 ioctl;
         struct nvif_ioctl_new_v0 create;
         struct nv_device_v0 device;
      } create = {0};
      struct {
         struct nvif_ioctl_v0 ioctl;
         struct nvif_ioctl_mthd_v0 method;
         struct nv_device_info_v0 info;
      } query = {0};

      create.ioctl.type = NVIF_IOCTL_V0_NEW;
      create.create.route = NVIF_IOCTL_V0_ROUTE_NVIF;
      create.create.oclass = NV_DEVICE;
      create.device.device = ~0ULL;
      assert(drmCommandWrite(fd, DRM_NOUVEAU_NVIF, &create,
                             sizeof(create)) == 0);

      query.ioctl.type = NVIF_IOCTL_V0_MTHD;
      query.method.method = NV_DEVICE_V0_INFO;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_NVIF, &query,
                                 sizeof(query)) == 0);
      assert(query.info.platform == NV_DEVICE_INFO_V0_PCIE);
      assert(query.info.chipset == 0x170U);
      assert(query.info.ram_size == winsys.caps.VramApertureBytes);
      assert(strcmp(query.info.chip, "GA104") == 0);
   }

   {
      struct drm_nouveau_channel_alloc channel = {0};
      NVX_XF86DRM_STATUS status;
      uint32_t previous_unsupported;
      NvxXf86DrmGetStatus(&status);
      previous_unsupported = status.UnsupportedCount;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_CHANNEL_ALLOC, &channel,
                                 sizeof(channel)) == -1);
      assert(errno == ENOSYS);
      memset(&status, 0, sizeof(status));
      NvxXf86DrmGetStatus(&status);
      assert(status.UnsupportedCount == previous_unsupported + 1U);
      assert(status.LastUnsupportedKind ==
             NVX_XF86DRM_UNSUPPORTED_WRITE_READ);
      assert(status.LastUnsupportedOperation == DRM_NOUVEAU_CHANNEL_ALLOC);
   }

   NvxXf86DrmUnregisterDevice(fd);
   assert(!NvxXf86DrmIsRegistered(fd));
   assert(drmGetVersion(fd) == NULL && errno == ENODEV);
   return 0;
}
