#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "../src_driver/nvx_gr_context.h"
#include "drm-uapi/nouveau_drm.h"
#include "nvif/class.h"
#include "nvif/cl0080.h"
#include "nvif/ioctl.h"

struct mock_buffer {
   uint64_t id;
   uint32_t bytes;
   uint8_t *data;
};

static struct mock_buffer buffers[8];
static uint64_t next_buffer_id = 1;
static uint64_t next_gpu_va = 0x10000000ULL;
static uint64_t next_gart_va = 0x11a00000ULL;

static struct mock_buffer *
find_buffer(uint64_t id)
{
   unsigned i;
   for (i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
      if (buffers[i].id == id)
         return &buffers[i];
   return NULL;
}

static int
mock_escape(void *opaque, uint32_t escape, uint32_t input_bytes,
            const void *input, uint32_t output_bytes, void *output)
{
   (void)opaque;
   if (escape == NVX_ESCAPE_3D_CHANNEL_LEASE &&
       input_bytes == sizeof(NVX_3D_CHANNEL_LEASE) &&
       output_bytes == sizeof(NVX_3D_CHANNEL_LEASE)) {
      const NVX_3D_CHANNEL_LEASE *request = input;
      NVX_3D_CHANNEL_LEASE *result = output;
      if (!Nvx3dValidateChannelLease(request, request->Operation))
         return 0;
      *result = *request;
      if (request->Operation == NVX_3D_CHANNEL_OPEN) {
         result->ChannelHandle = 0xc56f0002U;
         result->GraphicsObjectHandle = 0xc6970001U;
      }
      return 1;
   }
   if (escape == NVX_ESCAPE_3D_CLIENT_CLOSE &&
       input_bytes == sizeof(NVX_3D_CLIENT_SESSION) && output_bytes == 0)
      return 1;
   if (escape == NVX_ESCAPE_3D_ALLOC &&
       input_bytes == sizeof(NVX_3D_ALLOC_BUFFER) &&
       output_bytes == sizeof(NVX_3D_ALLOC_BUFFER)) {
      const NVX_3D_ALLOC_BUFFER *request = input;
      NVX_3D_ALLOC_BUFFER *result = output;
      struct mock_buffer *buffer = NULL;
      unsigned i;
      for (i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
         if (!buffers[i].id) {
            buffer = &buffers[i];
            break;
         }
      }
      if (!buffer || !request || request->Bytes == 0)
         return 0;
      buffer->data = calloc(1, request->Bytes);
      if (!buffer->data)
         return 0;
      buffer->id = next_buffer_id++;
      buffer->bytes = request->Bytes;
      *result = *request;
      result->BufferId = buffer->id;
      if (request->Usage & NVX_3D_USAGE_GART_BUFFER) {
         result->GpuVa = next_gart_va;
         next_gart_va += request->Bytes;
      } else {
         result->GpuVa = next_gpu_va;
         next_gpu_va += request->Bytes;
      }
      return 1;
   }
   if (escape == NVX_ESCAPE_3D_FREE &&
       input_bytes == sizeof(NVX_3D_FREE_BUFFER)) {
      const NVX_3D_FREE_BUFFER *request = input;
      struct mock_buffer *buffer = find_buffer(request->BufferId);
      if (!buffer)
         return 0;
      free(buffer->data);
      memset(buffer, 0, sizeof(*buffer));
      return 1;
   }
   if (escape == NVX_ESCAPE_3D_UPLOAD && input_bytes >=
       sizeof(NVX_3D_UPLOAD_BUFFER)) {
      const NVX_3D_UPLOAD_BUFFER *request = input;
      struct mock_buffer *buffer = find_buffer(request->BufferId);
      uint32_t payload = input_bytes - sizeof(*request);
      if (!buffer || request->Bytes != payload ||
          request->Offset > buffer->bytes ||
          payload > buffer->bytes - request->Offset)
         return 0;
      memcpy(buffer->data + request->Offset,
             (const uint8_t *)input + sizeof(*request), payload);
      return 1;
   }
   if (escape == NVX_ESCAPE_3D_DOWNLOAD &&
       input_bytes == sizeof(NVX_3D_DOWNLOAD_BUFFER)) {
      const NVX_3D_DOWNLOAD_BUFFER *request = input;
      struct mock_buffer *buffer = find_buffer(request->BufferId);
      if (!buffer || request->Bytes != output_bytes ||
          request->Offset > buffer->bytes ||
          output_bytes > buffer->bytes - request->Offset)
         return 0;
      memcpy(output, buffer->data + request->Offset, output_bytes);
      return 1;
   }
   if (escape == NVX_ESCAPE_3D_WAIT &&
       input_bytes == sizeof(NVX_3D_WAIT_FENCE) &&
       output_bytes == sizeof(NVX_3D_WAIT_FENCE)) {
      const NVX_3D_WAIT_FENCE *request = input;
      NVX_3D_WAIT_FENCE *result = output;
      *result = *request;
      result->Status = NVX_3D_STATUS_SUCCESS;
      return 1;
   }
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
   assert(errno == ENOENT);
   assert(drmPrimeFDToHandle(1, 1, &handle) == -1);
   assert(errno == ENOSYS && handle == 0);
   assert(drmPrimeHandleToFD(1, 1, 0, &prime_fd) == -1);
   assert(errno == ENOSYS && prime_fd == -1);
   drmFreeDevice(&device);
   assert(device == NULL);

   memset(&winsys, 0, sizeof(winsys));
   winsys.escape = mock_escape;
   winsys.opaque = &winsys;
   winsys.ClientToken = 0x123400000001ULL;
   winsys.caps.Version = NVX_3D_ABI_VERSION;
   winsys.caps.Size = sizeof(winsys.caps);
   winsys.caps.GraphicsClass = 0xc797U;
   winsys.caps.Features = NVX_3D_FEATURE_GR_CHANNEL |
                          NVX_3D_FEATURE_USER_ALLOC |
                          NVX_3D_FEATURE_UPLOAD |
                          NVX_3D_FEATURE_DOWNLOAD |
                          NVX_3D_FEATURE_FIXED_PROFILE_SUBMIT;
   winsys.caps.DeviceId = 0x2488U;
   winsys.caps.Chipset = 0x170U;
   winsys.caps.VramApertureBytes = 64U * 1024U * 1024U;
   winsys.caps.GartBytes =
      (uint64_t)NVX_GART_SLICE_COUNT * NVX_GART_SLICE_BYTES;
   winsys.caps.MaxBufferBytes = 16U * 1024U * 1024U;
   assert(NvxXf86DrmRegisterDevice(&winsys) == -1);
   winsys.caps.PciLocation = Nvx3dPackPciLocation(0x2aU, 0xabU);
   fd = NvxXf86DrmRegisterDevice(&winsys);
   assert(fd > 0 && NvxXf86DrmIsRegistered(fd));

   {
      struct drm_nouveau_gem_new create = {0};
      struct drm_nouveau_gem_info info = {0};
      struct drm_nouveau_gem_cpu_prep prep = {0};
      uint32_t *map;
      create.info.size = 4096;
      create.info.domain = NOUVEAU_GEM_DOMAIN_GART;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &create,
                                 sizeof(create)) == 0);
      assert(create.info.handle != 0);
      assert(create.info.domain == (NOUVEAU_GEM_DOMAIN_GART |
                                    NOUVEAU_GEM_DOMAIN_MAPPABLE));
      info.handle = create.info.handle;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_INFO, &info,
                                 sizeof(info)) == 0);
      assert(info.domain == create.info.domain && info.size == 4096);
      map = NvxXf86DrmMapBuffer(fd, create.info.handle,
                                NVX_XF86DRM_MAP_READ |
                                NVX_XF86DRM_MAP_WRITE);
      assert(map != NULL);
      map[0] = 0x89abcdefU;
      assert(NvxXf86DrmUnmapBuffer(fd, create.info.handle) == 0);
      assert(drmCloseBufferHandle(fd, create.info.handle) == 0);

      memset(&create, 0, sizeof(create));
      create.info.domain = NOUVEAU_GEM_DOMAIN_VRAM;
      create.info.size = 4096;
      create.align = 3U;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &create,
                                 sizeof(create)) == -1);
      create.align = 0;
      create.info.tile_mode = 0x10U;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &create,
                                 sizeof(create)) == -1);
      create.info.tile_mode = 0;
      create.info.tile_flags = 0x100U;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &create,
                                 sizeof(create)) == -1);
      create.info.tile_flags = 0;
      create.info.domain = NOUVEAU_GEM_DOMAIN_VRAM |
                           NOUVEAU_GEM_DOMAIN_GART;
      create.align = 0x10000U;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &create,
                                 sizeof(create)) == 0);
      assert(create.info.handle != 0 && create.info.offset == 0x10000000ULL);
      assert((create.info.offset & 0xffffU) == 0);
      assert(create.info.domain & NOUVEAU_GEM_DOMAIN_MAPPABLE);
      info.handle = create.info.handle;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_INFO, &info,
                                 sizeof(info)) == 0);
      assert(info.size == 4096 && info.offset == create.info.offset);
      map = NvxXf86DrmMapBuffer(fd, create.info.handle,
                                NVX_XF86DRM_MAP_READ |
                                NVX_XF86DRM_MAP_WRITE);
      assert(map != NULL && map[0] == 0);
      map[0] = 0x12345678U;
      assert(NvxXf86DrmFlushBuffer(fd, create.info.handle) == 0);
      assert(buffers[0].data[0] == 0x78U);
      assert(NvxXf86DrmUnmapBuffer(fd, create.info.handle) == 0);
      map = NvxXf86DrmMapBuffer(fd, create.info.handle,
                                NVX_XF86DRM_MAP_READ);
      assert(map != NULL && map[0] == 0x12345678U);
      assert(NvxXf86DrmUnmapBuffer(fd, create.info.handle) == 0);
      prep.handle = create.info.handle;
      prep.flags = NOUVEAU_GEM_CPU_PREP_NOWAIT;
      assert(drmCommandWrite(fd, DRM_NOUVEAU_GEM_CPU_PREP, &prep,
                             sizeof(prep)) == 0);
      assert(NvxXf86DrmFlushBuffer(fd, create.info.handle) == 0);
      assert(drmCloseBufferHandle(fd, create.info.handle) == 0);
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_INFO, &info,
                                 sizeof(info)) == -1);
      assert(errno == ENOENT);
   }

   {
      struct drm_nouveau_gem_new create = {0};
      create.info.size = 8192;
      create.info.domain = NOUVEAU_GEM_DOMAIN_VRAM;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_NEW, &create,
                                 sizeof(create)) == 0);
   }

   {
      drmVersionPtr version = drmGetVersion(fd);
      assert(version && version->version_major == 0 &&
             version->version_minor == 0 && version->version_patchlevel == 0 &&
             version->name_len == 3 && strcmp(version->name, "nvx") == 0);
      drmFreeVersion(version);
   }
   assert(drmGetDevice2(fd, 0, &device) == 0);
   assert(device && device->bustype == DRM_BUS_PCI);
   assert(device->businfo.pci->domain == 0 &&
          device->businfo.pci->bus == 0x2aU &&
          device->businfo.pci->dev == 0x0bU &&
          device->businfo.pci->func == 0x05U);
   assert(device->deviceinfo.pci->vendor_id == 0x10deU &&
          device->deviceinfo.pci->device_id == 0x2488U);
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
      struct drm_nouveau_gem_pushbuf pushbuf = {0};
      NVX_XF86DRM_STATUS status;
      uint32_t previous_unsupported;
      channel.fb_ctxdma_handle = 0xffffffffU;
      channel.tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_GR;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_CHANNEL_ALLOC, &channel,
                                 sizeof(channel)) == 0);
      assert((uint32_t)channel.channel == 0xc56f0002U);
      assert(channel.pushbuf_domains ==
             (NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART));
      assert(channel.notifier_handle == 0);

      memset(&status, 0, sizeof(status));
      NvxXf86DrmGetStatus(&status);
      previous_unsupported = status.UnsupportedCount;
      assert(drmCommandWriteRead(fd, DRM_NOUVEAU_GEM_PUSHBUF, &pushbuf,
                                 sizeof(pushbuf)) == -1);
      assert(errno == ENOSYS);
      NvxXf86DrmGetStatus(&status);
      assert(status.UnsupportedCount == previous_unsupported + 1U);
      assert(status.LastUnsupportedKind ==
             NVX_XF86DRM_UNSUPPORTED_WRITE_READ);
      assert(status.LastUnsupportedOperation == DRM_NOUVEAU_GEM_PUSHBUF);
      {
         struct drm_nouveau_channel_free channel_free = {
            .channel = channel.channel
         };
         assert(drmCommandWrite(fd, DRM_NOUVEAU_CHANNEL_FREE, &channel_free,
                                sizeof(channel_free)) == 0);
      }
   }

   NvxXf86DrmUnregisterDevice(fd);
   assert(!NvxXf86DrmIsRegistered(fd));
   assert(find_buffer(2) == NULL);
   assert(drmGetVersion(fd) == NULL && errno == ENODEV);
   return 0;
}
