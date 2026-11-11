/*
 * XP cross-build replacement for the libdrm entry points used by Nouveau.
 *
 * Discovery and command submission are intentionally not emulated here.
 * Each DRM operation must be translated to a specific validated NVX request;
 * returning fabricated DRM handles would let Mesa proceed with a channel and
 * pushbuffer that do not exist on this platform.
 */
#include <errno.h>
#include <stdlib.h>

#include "xf86drm.h"

static int
nvx_drm_unsupported(void)
{
   errno = ENOSYS;
   return -1;
}

drmVersionPtr
drmGetVersion(int fd)
{
   (void)fd;
   nvx_drm_unsupported();
   return NULL;
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
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd;
   (void)flags;
   if (device)
      *device = NULL;
   return nvx_drm_unsupported();
}

void
drmFreeDevice(drmDevicePtr *device)
{
   if (!device || !*device)
      return;
   free((*device)->businfo.pci);
   free((*device)->deviceinfo.pci);
   free(*device);
   *device = NULL;
}

int
drmCommandWrite(int fd, unsigned long command, void *data,
                unsigned long size)
{
   (void)fd;
   (void)command;
   (void)data;
   (void)size;
   return nvx_drm_unsupported();
}

int
drmCommandWriteRead(int fd, unsigned long command, void *data,
                    unsigned long size)
{
   (void)fd;
   (void)command;
   (void)data;
   (void)size;
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
