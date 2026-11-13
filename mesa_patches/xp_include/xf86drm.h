#ifndef NVX_XP_XF86DRM_H
#define NVX_XP_XF86DRM_H

/*
 * Minimal libdrm API surface used by Mesa's Nouveau DRM winsys.
 *
 * This is a Windows XP compatibility surface, not a Linux DRM implementation.
 * The matching nvx_xf86drm.c maps supported version/device queries and GEM
 * PCI identity, channel leasing, BO lifecycle, and CPU synchronization onto
 * the NVX miniport ABI. Nonempty pushbuffer submission, PRIME, and generic
 * ioctls remain unsupported without validated NVX equivalents.
 */
#include <stdint.h>
#include <fcntl.h>
#include "../nvx_winsys.h"
#include "../nvx_nouveau_bo.h"

/* XP's CRT does not define the POSIX close-on-exec open flag. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

#define DRM_BUS_PCI 0
#define DRM_BUS_USB 1
#define DRM_BUS_PLATFORM 2
#define DRM_PLATFORM_DEVICE_NAME_LEN 512

typedef struct _drmPciBusInfo {
   uint16_t domain;
   uint8_t bus;
   uint8_t dev;
   uint8_t func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
   uint16_t vendor_id;
   uint16_t device_id;
   uint8_t subvendor_id;
   uint8_t subdevice_id;
   uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmPlatformBusInfo {
   char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmDevice {
   char **nodes;
   int available_nodes;
   int bustype;
   union {
      drmPciBusInfoPtr pci;
      drmPlatformBusInfoPtr platform;
   } businfo;
   union {
      drmPciDeviceInfoPtr pci;
      drmPlatformDeviceInfoPtr platform;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

/* Diagnostics for DRM operations that have no Windows/NVX equivalent yet. */
#define NVX_XF86DRM_UNSUPPORTED_WRITE       1U
#define NVX_XF86DRM_UNSUPPORTED_WRITE_READ  2U
#define NVX_XF86DRM_UNSUPPORTED_READ        3U
#define NVX_XF86DRM_UNSUPPORTED_NONE        4U
#define NVX_XF86DRM_UNSUPPORTED_IOCTL       5U
#define NVX_XF86DRM_UNSUPPORTED_CLOSE       6U
#define NVX_XF86DRM_UNSUPPORTED_PRIME_TO_HANDLE 7U
#define NVX_XF86DRM_UNSUPPORTED_HANDLE_TO_PRIME 8U
#define NVX_XF86DRM_MAP_READ  0x00000001U
#define NVX_XF86DRM_MAP_WRITE 0x00000002U
typedef struct NVX_XF86DRM_STATUS {
   uint32_t UnsupportedCount;
   uint32_t LastUnsupportedKind;
   uint32_t LastUnsupportedOperation;
   uint32_t ChannelAllocCount;
   uint32_t LastChannelAllocResult;
   uint32_t LastChannelHandle;
   uint32_t GemNewCount;
   uint32_t LastGemNewResult;
   uint32_t LastGemNewDomain;
   uint32_t LastGemNewTileMode;
   uint32_t LastGemNewTileFlags;
   uint64_t LastGemNewSize;
   uint32_t SclassCalls;
   uint32_t LastSclassResult;
   uint32_t LastSclassCount;
   uint32_t LastSclass[16];
} NVX_XF86DRM_STATUS;

/* Register an already queried NVX adapter and obtain a process-local DRM
 * compatibility descriptor.  This is metadata transport only; it does not
 * create a Nouveau channel or authorize GPU commands. */
int NvxXf86DrmRegisterDevice(const NVX_WINSYS_DEVICE *device);
void NvxXf86DrmUnregisterDevice(int fd);
int NvxXf86DrmIsRegistered(int fd);
void NvxXf86DrmGetStatus(NVX_XF86DRM_STATUS *status);
#if defined(_WIN32) || defined(_WIN64)
int __stdcall
NvxXf86DrmReadDrawTargetRaw(uint32_t draw_kind, uint32_t *green_pixels,
                            uint32_t *blue_rgba_pixels,
                            uint32_t *blue_bgra_pixels, uint32_t *bytes,
                            uint64_t *target_va, uint64_t *buffer_id);
#endif
void *NvxXf86DrmMapBuffer(int fd, uint32_t handle, uint32_t access);
int NvxXf86DrmUpdateBufferMapAccess(int fd, uint32_t handle,
                                    uint32_t access);
int NvxXf86DrmUnmapBuffer(int fd, uint32_t handle);
int NvxXf86DrmFlushBuffer(int fd, uint32_t handle);

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmCommandWrite(int fd, unsigned long command, void *data,
                    unsigned long size);
int drmCommandWriteRead(int fd, unsigned long command, void *data,
                        unsigned long size);
int drmCommandRead(int fd, unsigned long command, void *data,
                   unsigned long size);
int drmCommandNone(int fd, unsigned long command);
int drmIoctl(int fd, unsigned long request, void *data);
int drmCloseBufferHandle(int fd, uint32_t handle);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags,
                       int *prime_fd);

#ifdef __cplusplus
}
#endif

#endif /* NVX_XP_XF86DRM_H */
