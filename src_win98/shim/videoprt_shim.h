#ifndef VIDEOPRT_SHIM_H
#define VIDEOPRT_SHIM_H

#include "miniport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NVX_VXD_DEV_EXT {
    uint32_t bar0_phys;
    uint32_t bar0_size;
    void    *bar0_virt;

    uint32_t bar1_phys;
    uint32_t bar1_size;
    void    *bar1_virt;

    uint32_t pci_bus;
    uint32_t pci_slot;
    uint32_t pci_func;
} NVX_VXD_DEV_EXT;

VOID NvxVxDDeviceInit(VOID);

typedef struct _DIOCParams {
    uint32_t Internal1;
    uint32_t VMHandle;
    uint32_t Internal2;
    uint32_t dwIoControlCode;
    void    *lpvInBuffer;
    uint32_t cbInBuffer;
    void    *lpvOutBuffer;
    uint32_t cbOutBuffer;
    uint32_t *lpcbBytesReturned;
    void    *lpoOverlapped;
    uint32_t hDevice;
    uint32_t tagProcess;
} DIOCParams;

int NvxVxDDeviceIoControl(DIOCParams *params);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOPRT_SHIM_H */
