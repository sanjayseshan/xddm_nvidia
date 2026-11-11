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
#define NVX_DRM_FD_BASE 0x4e580100

struct nvx_drm_device_slot {
   int fd;
   NVX_WINSYS_DEVICE device;
};

static struct nvx_drm_device_slot nvx_drm_devices[NVX_DRM_DEVICE_SLOTS];
static volatile int nvx_drm_lock;
static int nvx_drm_unsupported(void);
static int nvx_drm_lookup(int fd, NVX_WINSYS_DEVICE *device);

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
      return nvx_drm_unsupported();
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
      return nvx_drm_unsupported();
   create = (struct nvif_ioctl_new_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (create->oclass != NV_DEVICE || create->route != NVIF_IOCTL_V0_ROUTE_NVIF)
      return nvx_drm_unsupported();
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
      return nvx_drm_unsupported();
   method = (struct nvif_ioctl_mthd_v0 *)((uint8_t *)data + sizeof(*ioctl));
   if (method->method != NV_DEVICE_V0_INFO)
      return nvx_drm_unsupported();
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

static void
nvx_format_hex4(char text[5], uint32_t value)
{
   static const char digits[] = "0123456789abcdef";
   unsigned i;
   for (i = 0; i < 4; i++)
      text[i] = digits[(value >> ((3U - i) * 4U)) & 0xfU];
   text[4] = '\0';
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
       !device->caps.VramApertureBytes || !device->caps.MaxBufferBytes)
      return -1;

   nvx_drm_lock_acquire();
   for (i = 0; i < NVX_DRM_DEVICE_SLOTS; i++) {
      if (nvx_drm_devices[i].fd == 0) {
         fd = NVX_DRM_FD_BASE + (int)i;
         nvx_drm_devices[i].fd = fd;
         nvx_drm_devices[i].device = *device;
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
   drmPlatformBusInfoPtr bus;
   drmPlatformDeviceInfoPtr info;
   char **compatible;
   char device_id[5], chipset[5], fullname[128] = { 0 }, compat_name[64];
   size_t i, length;

   if (!out_device || flags || !nvx_drm_lookup(fd, &winsys)) {
      if (out_device)
         *out_device = NULL;
      errno = ENODEV;
      return -1;
   }
   *out_device = NULL;

   device = (drmDevicePtr)calloc(1, sizeof(*device));
   bus = (drmPlatformBusInfoPtr)calloc(1, sizeof(*bus));
   info = (drmPlatformDeviceInfoPtr)calloc(1, sizeof(*info));
   compatible = (char **)calloc(2, sizeof(*compatible));
   if (!device || !bus || !info || !compatible)
      goto fail;

   nvx_format_hex4(device_id, winsys.caps.DeviceId);
   nvx_format_hex4(chipset, winsys.caps.Chipset);
   memcpy(fullname, "NVX HDC adapter PCI ", 20);
   memcpy(fullname + 20, device_id, 4);
   memcpy(fullname + 24, " chipset ", 9);
   memcpy(fullname + 33, chipset, 5);
   length = strlen(fullname);
   if (length >= sizeof(bus->fullname))
      length = sizeof(bus->fullname) - 1U;
   memcpy(bus->fullname, fullname, length + 1U);

   memcpy(compat_name, "nvx,nvidia-ga104-", 17);
   memcpy(compat_name + 17, chipset, 5);
   compatible[0] = nvx_drm_strdup(compat_name);
   if (!compatible[0])
      goto fail;

   device->bustype = DRM_BUS_PLATFORM;
   device->businfo.platform = bus;
   device->deviceinfo.platform = info;
   info->compatible = compatible;
   *out_device = device;
   return 0;

fail:
   if (compatible) {
      for (i = 0; i < 2; i++)
         free(compatible[i]);
   }
   free(compatible);
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
   return nvx_drm_unsupported();
}

int
drmCommandWriteRead(int fd, unsigned long command, void *data,
                    unsigned long size)
{
   if (command == DRM_NOUVEAU_GETPARAM)
      return nvx_drm_getparam(fd, data, size);
   if (command == DRM_NOUVEAU_NVIF)
      return nvx_drm_nvif_device_info(fd, data, size);
   return nvx_drm_unsupported();
}

int
drmCommandRead(int fd, unsigned long command, void *data,
               unsigned long size)
{
   (void)fd;
   (void)command;
   (void)data;
   (void)size;
   return nvx_drm_unsupported();
}

int
drmCommandNone(int fd, unsigned long command)
{
   (void)fd;
   (void)command;
   return nvx_drm_unsupported();
}

int
drmIoctl(int fd, unsigned long request, void *data)
{
   (void)fd;
   (void)request;
   (void)data;
   return nvx_drm_unsupported();
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   (void)fd;
   (void)handle;
   return nvx_drm_unsupported();
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   (void)prime_fd;
   if (handle)
      *handle = 0;
   return nvx_drm_unsupported();
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)handle;
   (void)flags;
   if (prime_fd)
      *prime_fd = -1;
   return nvx_drm_unsupported();
}
