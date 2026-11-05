#include "videoprt_shim.h"
#include <string.h>

/*
 * VxD Ring 0 VMM service signatures
 */
extern void * __attribute__((stdcall)) _MapPhysToLinear(uint32_t physAddr, uint32_t nBytes, uint32_t flags);
extern void * __attribute__((stdcall)) _PageAllocate(uint32_t nPages, uint32_t pType, uint32_t VM, uint32_t AlignMask, uint32_t minPhys, uint32_t maxPhys, uint32_t *PhysAddr, uint32_t flags);
extern int    __attribute__((stdcall)) _PageFree(void *hMem, uint32_t flags);
extern void   __attribute__((stdcall)) Out_Debug_String(const char *str);

/* Fallback / Mock implementations for building & testing */
void VideoPortZeroMemory(PVOID Destination, ULONG Length) {
    if (Destination && Length) {
        memset(Destination, 0, Length);
    }
}

void VideoPortMoveMemory(PVOID Destination, PVOID Source, ULONG Length) {
    if (Destination && Source && Length) {
        memmove(Destination, Source, Length);
    }
}

PVOID VideoPortGetDeviceBase(PVOID HwDeviceExtension, PHYSICAL_ADDRESS IoAddress, ULONG NumberOfBytes, UCHAR InIoSpace) {
    (void)HwDeviceExtension;
    (void)InIoSpace;
    /* In Ring 0 VxD: Call _MapPhysToLinear(phys, bytes, 0) */
    uint32_t phys = IoAddress.LowPart;
    #ifdef __VXDKERNEL__
    return _MapPhysToLinear(phys, NumberOfBytes, 0);
    #else
    return (PVOID)(uintptr_t)phys;
    #endif
}

VOID VideoPortFreeDeviceBase(PVOID HwDeviceExtension, PVOID MappedAddress) {
    (void)HwDeviceExtension;
    (void)MappedAddress;
}

VP_STATUS VideoPortMapMemory(PVOID HwDeviceExtension, PHYSICAL_ADDRESS PhysicalAddress, ULONG *Length, ULONG *InIoSpace, PVOID *VirtualAddress) {
    (void)HwDeviceExtension;
    (void)InIoSpace;
    if (!VirtualAddress || !Length) return ERROR_INVALID_PARAMETER;
    
    uint32_t phys = PhysicalAddress.LowPart;
    #ifdef __VXDKERNEL__
    *VirtualAddress = _MapPhysToLinear(phys, *Length, 0);
    #else
    *VirtualAddress = (PVOID)(uintptr_t)phys;
    #endif
    return NO_ERROR;
}

VP_STATUS VideoPortUnmapMemory(PVOID HwDeviceExtension, PVOID VirtualAddress, HANDLE ProcessHandle) {
    (void)HwDeviceExtension;
    (void)VirtualAddress;
    (void)ProcessHandle;
    return NO_ERROR;
}

VOID VideoPortStallExecution(ULONG Microseconds) {
    /* Precise delay using port 0x80 or TSC */
    for (volatile ULONG i = 0; i < Microseconds * 20; i++) {
        __asm__ __volatile__("outb %%al, $0x80" : : : "memory");
    }
}

PVOID VideoPortAllocateCommonBuffer(PVOID HwDeviceExtension, PVOID DmaAdapter, ULONG DesiredLength, PPHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled, PVOID Reserved) {
    (void)HwDeviceExtension;
    (void)DmaAdapter;
    (void)CacheEnabled;
    (void)Reserved;

    uint32_t pages = (DesiredLength + 4095) / 4096;
    uint32_t phys = 0;
    void *virt = NULL;

    #ifdef __VXDKERNEL__
    /* Allocate contiguous physical pages below 4GB */
    virt = _PageAllocate(pages, 0 /* PG_SYS */, 0, 0, 0, 0xFFFFFFFF, &phys, 0);
    #endif

    if (LogicalAddress) {
        LogicalAddress->LowPart = phys;
        LogicalAddress->HighPart = 0;
    }
    return virt;
}

VOID VideoPortFreeCommonBuffer(PVOID HwDeviceExtension, ULONG Length, PVOID VirtualAddress, PHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled) {
    (void)HwDeviceExtension;
    (void)Length;
    (void)LogicalAddress;
    (void)CacheEnabled;
    #ifdef __VXDKERNEL__
    if (VirtualAddress) {
        _PageFree(VirtualAddress, 0);
    }
    #endif
}

VOID VideoPortDebugPrint(ULONG DebugPrintLevel, const char *DebugMessage, ...) {
    (void)DebugPrintLevel;
    #ifdef __VXDKERNEL__
    if (DebugMessage) {
        Out_Debug_String(DebugMessage);
    }
    #endif
}

