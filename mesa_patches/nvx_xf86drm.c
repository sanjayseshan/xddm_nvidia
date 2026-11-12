/*
 * XP replacement for the libdrm entry points used by Nouveau.
 *
 * Device/version discovery is backed by a registered NVX adapter. GPU
 * commands remain unsupported until they have operation-specific validated
 * NVX implementations; this layer never forwards arbitrary DRM ioctls.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "drm-uapi/nouveau_drm.h"
#include "nvif/class.h"
#include "nvif/cl0080.h"
#include "nvif/ioctl.h"

#define NVX_DRM_DEVICE_SLOTS 8U
#define NVX_DRM_BUFFER_SLOTS 64U
#define NVX_DRM_FD_BASE 0x4e580100

struct nvx_drm_bo_slot {
   uint32_t handle;
   NVX_NOUVEAU_BO bo;
};

struct nvx_drm_device_slot {
   int fd;
   NVX_WINSYS_DEVICE device;
   uint64_t client_token;
   uint32_t next_handle;
   struct nvx_drm_bo_slot buffers[NVX_DRM_BUFFER_SLOTS];
};

static struct nvx_drm_device_slot nvx_drm_devices[NVX_DRM_DEVICE_SLOTS];
static volatile int nvx_drm_lock;
static NVX_XF86DRM_STATUS nvx_drm_status;
static int nvx_drm_unsupported(void);
static int nvx_drm_unsupported_operation(uint32_t kind,
                                        unsigned long operation);
static int nvx_drm_lookup(int fd, NVX_WINSYS_DEVICE *device);

static struct nvx_drm_device_slot *
nvx_drm_find_device_locked(int fd)
{
   unsigned i;
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++)
      if (nvx_drm_devices[i].fd == fd)
         return &nvx_drm_devices[i];
   return NULL;
}

static struct nvx_drm_bo_slot *
nvx_drm_find_bo_locked(struct nvx_drm_device_slot *device, uint32_t handle)
{
   unsigned i;
   if (!device || !handle)
      return NULL;
   for (i = 0; i < NVX_DRM_BUFFER_SLOTS; i++)
      if (device->buffers[i].handle == handle)
         return &device->buffers[i];
   return NULL;
}

static int
nvx_drm_getparam(int fd, void *data, unsigned long size)
{
   struct drm_nouveau_getparam *request = data;
   NVX_WINSYS_DEVICE device;

   if (!request || size != sizeof(*request) ||
       !nvx_drm_lookup(fd, &device))
      return nvx_drm_unsupported();
   switch (request->param) {
   case NOUVEAU_GETPARAM_FB_SIZE:
      /* This is the bounded GPUVA aperture reported by NVX, not physical
       * VRAM capacity. Treat it as a conservative Nouveau allocation limit. */
      request->value = device.caps.VramApertureBytes;
      break;
   case NOUVEAU_GETPARAM_AGP_SIZE:
      request->value = device.caps.GartBytes;
      break;
   case NOUVEAU_GETPARAM_PCI_VENDOR:
      request->value = 0x10deU;
      break;
   case NOUVEAU_GETPARAM_PCI_DEVICE:
      request->value = device.caps.DeviceId;
      break;
   case NOUVEAU_GETPARAM_CHIPSET_ID:
      request->value = device.caps.Chipset;
      break;
   default:
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                          DRM_NOUVEAU_GETPARAM);
   }
   return 0;
}

static int
nvx_drm_nvif_device_new(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvif_ioctl_new_v0 *create;
   NVX_WINSYS_DEVICE device;
   const unsigned long minimum = sizeof(*ioctl) + sizeof(*create) +
                                 sizeof(struct nv_device_v0);

   if (!nvx_drm_lookup(fd, &device) || !data ||
       size != minimum || ioctl->type != NVIF_IOCTL_V0_NEW)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                          DRM_NOUVEAU_NVIF);
   create = (struct nvif_ioctl_new_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (create->oclass != NV_DEVICE || create->route != NVIF_IOCTL_V0_ROUTE_NVIF)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                          DRM_NOUVEAU_NVIF);
   /* nouveau_device_new() creates a client-side descriptor. NVX owns the
    * actual channel and engine objects, so this does not bind a GPU object. */
   return 0;
}

static int
nvx_drm_nvif_device_info(int fd, void *data, unsigned long size)
{
   struct nvif_ioctl_v0 *ioctl = data;
   struct nvif_ioctl_mthd_v0 *method;
   struct nv_device_info_v0 *info;
   NVX_WINSYS_DEVICE device;
   const unsigned long expected = sizeof(*ioctl) + sizeof(*method) +
                                  sizeof(*info);

   if (!data || size != expected || !nvx_drm_lookup(fd, &device) ||
       ioctl->type != NVIF_IOCTL_V0_MTHD)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                          DRM_NOUVEAU_NVIF);
   method = (struct nvif_ioctl_mthd_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (method->method != NV_DEVICE_V0_INFO)
      return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                          DRM_NOUVEAU_NVIF);
   info = (struct nv_device_info_v0 *)((uint8_t *)method + sizeof(*method));
   memset(info, 0, sizeof(*info));
   info->version = 0;
   info->platform = NV_DEVICE_INFO_V0_PCIE;
   info->chipset = (uint16_t)device.caps.Chipset;
   info->ram_size = device.caps.VramApertureBytes;
   memcpy(info->chip, "GA104", sizeof("GA104"));
   memcpy(info->name, "NVX Ampere device", sizeof("NVX Ampere device"));
   return 0;
}

static void
nvx_drm_lock_acquire(void)
{
   while (__sync_lock_test_and_set(&nvx_drm_lock, 1))
      ;
}

static void
nvx_drm_lock_release(void)
{
   __sync_lock_release(&nvx_drm_lock);
}

static int
nvx_drm_unsupported(void)
{
   errno = ENOSYS;
   return -1;
}

static int
nvx_drm_unsupported_operation(uint32_t kind, unsigned long operation)
{
   nvx_drm_lock_acquire();
   nvx_drm_status.UnsupportedCount++;
   nvx_drm_status.LastUnsupportedKind = kind;
   nvx_drm_status.LastUnsupportedOperation = (uint32_t)operation;
   nvx_drm_lock_release();
   return nvx_drm_unsupported();
}

static int
nvx_drm_lookup(int fd, NVX_WINSYS_DEVICE *device)
{
   unsigned i;
   int found = 0;

   if (!device)
      return 0;
   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == fd) {
         *device = nvx_drm_devices[i].device;
         found = 1;
         break;
      }
   }
   nvx_drm_lock_release();
   return found;
}

static char *
nvx_drm_strdup(const char *text)
{
   size_t bytes;
   char *copy;

   if (!text)
      return NULL;
   bytes = strlen(text) + 1U;
   copy = (char *)malloc(bytes);
   if (copy)
      memcpy(copy, text, bytes);
   return copy;
}

int
NvxXf86DrmRegisterDevice(const NVX_WINSYS_DEVICE *device)
{
   unsigned i;
   int fd = -1;

   if (!device || !device->escape || !device->opaque ||
       device->caps.Version != NVX_3D_ABI_VERSION ||
       device->caps.Size != sizeof(device->caps) ||
       device->caps.GraphicsClass != 0xc797U ||
       !device->caps.DeviceId || !device->caps.Chipset ||
       !(device->caps.PciLocation & NVX_3D_PCI_LOCATION_VALID) ||
       !device->caps.VramApertureBytes || !device->caps.MaxBufferBytes ||
       !device->ClientToken)
      return -1;

   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == 0) {
         fd = NVX_DRM_FD_BASE + (int)i;
         memset(&nvx_drm_devices[i], 0, sizeof(nvx_drm_devices[i]));
         nvx_drm_devices[i].fd = fd;
         nvx_drm_devices[i].device = *device;
         nvx_drm_devices[i].client_token = device->ClientToken;
         nvx_drm_devices[i].next_handle = 1U;
         break;
      }
   }
   nvx_drm_lock_release();
   if (fd < 0)
      errno = EMFILE;
   return fd;
}

void
NvxXf86DrmUnregisterDevice(int fd)
{
   unsigned i;
   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == fd) {
         unsigned j;
         int all_freed = 1;
         for (j = 0; j < NVX_DRM_BUFFER_SLOTS; j++) {
            struct nvx_drm_bo_slot *buffer = &nvx_drm_devices[i].buffers[j];
            if (!buffer->handle)
               continue;
            if (!NvxNouveauBoDestroy(&buffer->bo)) {
               all_freed = 0;
               continue;
            }
            memset(buffer, 0, sizeof(*buffer));
         }
         if (!all_freed)
            break;
         if (!NvxWinsysCloseClient(&nvx_drm_devices[i].device))
            break;
         memset(&nvx_drm_devices[i], 0, sizeof(nvx_drm_devices[i]));
         break;
      }
   }
   nvx_drm_lock_release();
}

int
NvxXf86DrmIsRegistered(int fd)
{
   NVX_WINSYS_DEVICE device;
   return nvx_drm_lookup(fd, &device);
}

void
NvxXf86DrmGetStatus(NVX_XF86DRM_STATUS *status)
{
   if (!status)
      return;
   nvx_drm_lock_acquire();
   *status = nvx_drm_status;
   nvx_drm_lock_release();
}

static int
nvx_drm_gem_new(struct nvx_drm_device_slot *device,
                struct drm_nouveau_gem_new *request)
{
   struct nvx_drm_bo_slot *slot = NULL;
   NVX_3D_ALLOC_BUFFER description;
   uint64_t requested_bytes;
   uint32_t allocated_bytes, handle;
   unsigned i;

   if (!device || !request || request->info.handle ||
       !request->info.size ||
       request->info.size > device->device.caps.MaxBufferBytes ||
       request->info.size > UINT32_MAX || request->align > 0x10000U ||
       (request->info.domain & (NOUVEAU_GEM_DOMAIN_CPU |
                                NOUVEAU_GEM_DOMAIN_COHERENT)) ||
       (request->info.domain &&
        !(request->info.domain & NOUVEAU_GEM_DOMAIN_VRAM)))
      return 0;

   requested_bytes = request->info.size;
   if (requested_bytes > UINT32_MAX - (NVX_3D_PAGE_BYTES - 1U))
      return 0;
   allocated_bytes = Nvx3dAlignU32((uint32_t)requested_bytes,
                                   NVX_3D_PAGE_BYTES);
   if (allocated_bytes > device->device.caps.MaxBufferBytes)
      return 0;

   for (i = 0; i < NVX_DRM_BUFFER_SLOTS; i++) {
      if (!device->buffers[i].handle) {
         slot = &device->buffers[i];
         break;
      }
   }
   if (!slot)
      return 0;

   memset(&description, 0, sizeof(description));
   description.Size = sizeof(description);
   description.Version = NVX_3D_ABI_VERSION;
   description.Bytes = allocated_bytes;
   handle = device->next_handle++;
   if (!handle)
      handle = device->next_handle++;

   if (!NvxNouveauBoCreate(&device->device, &description,
                           device->client_token, &slot->bo))
      return 0;

   slot->handle = handle;
   request->info.handle = handle;
   request->info.domain = NOUVEAU_GEM_DOMAIN_VRAM |
                          NOUVEAU_GEM_DOMAIN_MAPPABLE;
   request->info.size = requested_bytes;
   request->info.offset = slot->bo.allocation.GpuVa;
   request->info.map_handle = handle;
   request->info.tile_flags |= NOUVEAU_GEM_TILE_NONCONTIG;
   return 1;
}

static int
nvx_drm_gem_info(struct nvx_drm_device_slot *device,
                 struct drm_nouveau_gem_info *info)
{
   struct nvx_drm_bo_slot *slot;

   if (!device || !info || !(slot = nvx_drm_find_bo_locked(device,
                                                            info->handle)))
      return 0;
   info->domain = NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_MAPPABLE;
   info->size = slot->bo.allocation.Bytes;
   info->offset = slot->bo.allocation.GpuVa;
   info->map_handle = slot->handle;
   info->tile_flags |= NOUVEAU_GEM_TILE_NONCONTIG;
   return 1;
}

void *
NvxXf86DrmMapBuffer(int fd, uint32_t handle, uint32_t access)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   void *map = NULL;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot)
      map = NvxNouveauBoMap(&slot->bo, access);
   nvx_drm_lock_release();
   if (!map)
      errno = found ? EIO : ENOENT;
   return map;
}

int
NvxXf86DrmUnmapBuffer(int fd, uint32_t handle)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   int result = 0;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot)
      result = NvxNouveauBoUnmap(&slot->bo);
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EIO : ENOENT;
   return result ? 0 : -1;
}

int
NvxXf86DrmFlushBuffer(int fd, uint32_t handle)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *slot;
   int result = 0;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   slot = nvx_drm_find_bo_locked(device, handle);
   found = slot != NULL;
   if (slot)
      result = NvxNouveauBoFlush(&slot->bo);
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EIO : ENOENT;
   return result ? 0 : -1;
}

drmVersionPtr
drmGetVersion(int fd)
{
   drmVersionPtr version;

   if (!NvxXf86DrmIsRegistered(fd)) {
      errno = ENODEV;
      return NULL;
   }
   version = (drmVersionPtr)calloc(1, sizeof(*version));
   if (!version)
      return NULL;
   version->name = nvx_drm_strdup("nvx");
   if (!version->name) {
      free(version);
      return NULL;
   }
   version->name_len = 3;
   /* Zero version denotes the NVX transport; it is not a claimed Nouveau DRM
    * kernel version. Nouveau's XP path checks registration directly. */
   return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
   if (!version)
      return;
   free(version->name);
   free(version->date);
   free(version->desc);
   free(version);
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *out_device)
{
   NVX_WINSYS_DEVICE winsys;
   drmDevicePtr device;
   drmPciBusInfoPtr bus;
   drmPciDeviceInfoPtr info;

   if (!out_device || flags || !nvx_drm_lookup(fd, &winsys) ||
       !(winsys.caps.PciLocation & NVX_3D_PCI_LOCATION_VALID)) {
      if (out_device)
         *out_device = NULL;
      errno = ENODEV;
      return -1;
   }
   *out_device = NULL;

   device = (drmDevicePtr)calloc(1, sizeof(*device));
   bus = (drmPciBusInfoPtr)calloc(1, sizeof(*bus));
   info = (drmPciDeviceInfoPtr)calloc(1, sizeof(*info));
   if (!device || !bus || !info)
      goto fail;

   bus->domain = 0;
   bus->bus = (uint8_t)(winsys.caps.PciLocation &
                        NVX_3D_PCI_LOCATION_BUS_MASK);
   bus->dev = (uint8_t)((winsys.caps.PciLocation >>
                         NVX_3D_PCI_LOCATION_SLOT_SHIFT) & 0x1fU);
   bus->func = (uint8_t)((winsys.caps.PciLocation >>
                          (NVX_3D_PCI_LOCATION_SLOT_SHIFT + 5U)) & 0x7U);
   info->vendor_id = 0x10deU;
   info->device_id = (uint16_t)winsys.caps.DeviceId;
   device->bustype = DRM_BUS_PCI;
   device->businfo.pci = bus;
   device->deviceinfo.pci = info;
   *out_device = device;
   return 0;

fail:
   free(info);
   free(bus);
   free(device);
   errno = ENOMEM;
   return -1;
}

void
drmFreeDevice(drmDevicePtr *device)
{
   unsigned i;
   if (!device || !*device)
      return;
   if ((*device)->bustype == DRM_BUS_PLATFORM) {
      drmPlatformDeviceInfoPtr info = (*device)->deviceinfo.platform;
      if (info) {
         if (info->compatible) {
            for (i = 0; info->compatible[i]; i++)
               free(info->compatible[i]);
         }
         free(info->compatible);
         free(info);
      }
      free((*device)->businfo.platform);
   } else if ((*device)->bustype == DRM_BUS_PCI) {
      free((*device)->businfo.pci);
      free((*device)->deviceinfo.pci);
   }
   free((*device)->nodes);
   free(*device);
   *device = NULL;
}

int
drmCommandWrite(int fd, unsigned long command, void *data,
                unsigned long size)
{
   if (command == DRM_NOUVEAU_NVIF)
      return nvx_drm_nvif_device_new(fd, data, size);
   if (command == DRM_NOUVEAU_GEM_CPU_PREP) {
      struct drm_nouveau_gem_cpu_prep *request = data;
      struct nvx_drm_device_slot *device;
      struct nvx_drm_bo_slot *buffer;
      int found;
      int ok = 0;

      if (!request || size != sizeof(*request))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      buffer = nvx_drm_find_bo_locked(device, request->handle);
      found = buffer != NULL;
      if (buffer) {
         ok = !buffer->bo.last_fence ||
              (!(request->flags & NOUVEAU_GEM_CPU_PREP_NOWAIT) &&
               NvxWinsysWaitFence(&device->device, buffer->bo.last_fence,
                                  5000U));
      }
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? EBUSY : ENOENT;
      return -1;
   }
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE,
                                       command);
}

int
drmCommandWriteRead(int fd, unsigned long command, void *data,
                    unsigned long size)
{
   struct nvx_drm_device_slot *device;
   int ok;

   if (command == DRM_NOUVEAU_GETPARAM)
      return nvx_drm_getparam(fd, data, size);
   if (command == DRM_NOUVEAU_NVIF)
      return nvx_drm_nvif_device_info(fd, data, size);
   if (command == DRM_NOUVEAU_GEM_NEW) {
      struct drm_nouveau_gem_new *request = data;
      int found;
      if (!request || size != sizeof(*request))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE_READ, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      found = device != NULL;
      ok = nvx_drm_gem_new(device, request);
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? ENOMEM : ENODEV;
      return -1;
   }
   if (command == DRM_NOUVEAU_GEM_INFO) {
      struct drm_nouveau_gem_info *info = data;
      int found;
      if (!info || size != sizeof(*info))
         return nvx_drm_unsupported_operation(
            NVX_XF86DRM_UNSUPPORTED_WRITE_READ, command);
      nvx_drm_lock_acquire();
      device = nvx_drm_find_device_locked(fd);
      found = device != NULL;
      ok = nvx_drm_gem_info(device, info);
      nvx_drm_lock_release();
      if (ok)
         return 0;
      errno = found ? ENOENT : ENODEV;
      return -1;
   }
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_WRITE_READ,
                                       command);
}

int
drmCommandRead(int fd, unsigned long command, void *data,
               unsigned long size)
{
   (void)fd;
   (void)data;
   (void)size;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_READ,
                                       command);
}

int
drmCommandNone(int fd, unsigned long command)
{
   (void)fd;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_NONE,
                                       command);
}

int
drmIoctl(int fd, unsigned long request, void *data)
{
   (void)fd;
   (void)data;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_IOCTL,
                                       request);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   struct nvx_drm_device_slot *device;
   struct nvx_drm_bo_slot *buffer;
   int result = 0;
   int found;

   nvx_drm_lock_acquire();
   device = nvx_drm_find_device_locked(fd);
   buffer = nvx_drm_find_bo_locked(device, handle);
   found = buffer != NULL;
   if (buffer && NvxNouveauBoDestroy(&buffer->bo)) {
      memset(buffer, 0, sizeof(*buffer));
      result = 1;
   }
   nvx_drm_lock_release();
   if (!result)
      errno = found ? EIO : ENOENT;
   return result ? 0 : -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   if (handle)
      *handle = 0;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_PRIME_TO_HANDLE,
                                       (uint32_t)prime_fd);
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)flags;
   if (prime_fd)
      *prime_fd = -1;
   return nvx_drm_unsupported_operation(NVX_XF86DRM_UNSUPPORTED_HANDLE_TO_PRIME,
                                       handle);
}
