/*
 * Compile-time ioctl encoding shim for Mesa's BSD-flavoured DRM UAPI headers
 * under MinGW. The NVX XP winsys does not issue these DRM ioctls; device access
 * is provided by the project miniport ABI. Keep this outside the Mesa pin.
 */
#ifndef NVX_MESA_XP_SYS_IOCCOM_H
#define NVX_MESA_XP_SYS_IOCCOM_H

#define NVX_IOC_DIRSHIFT 30
#define NVX_IOC_SIZESHIFT 16
#define NVX_IOC_TYPESHIFT 8
#define NVX_IOC_NRMASK 0xffU
#define NVX_IOC_TYPEMASK 0xffU
#define NVX_IOC_SIZEMASK 0x3fffU

#ifndef _IOC
#define _IOC(dir, type, nr, size) \
   (((unsigned int)(dir) << NVX_IOC_DIRSHIFT) | \
    (((unsigned int)(size) & NVX_IOC_SIZEMASK) << NVX_IOC_SIZESHIFT) | \
    (((unsigned int)(type) & NVX_IOC_TYPEMASK) << NVX_IOC_TYPESHIFT) | \
    ((unsigned int)(nr) & NVX_IOC_NRMASK))
#endif

#ifndef _IO
#define _IO(type, nr) _IOC(0U, (type), (nr), 0U)
#endif
#ifndef _IOR
#define _IOR(type, nr, data_type) \
   _IOC(2U, (type), (nr), sizeof(data_type))
#endif
#ifndef _IOW
#define _IOW(type, nr, data_type) \
   _IOC(1U, (type), (nr), sizeof(data_type))
#endif
#ifndef _IOWR
#define _IOWR(type, nr, data_type) \
   _IOC(3U, (type), (nr), sizeof(data_type))
#endif

#endif
