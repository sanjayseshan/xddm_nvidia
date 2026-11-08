#include <stdint.h>
#include <stddef.h>
#include "videoprt_shim.h"
#include "miniport.h"
#include "nvx_gsp.h"

/* VMM Service Call signatures */
extern void * __attribute__((stdcall)) _MapPhysToLinear(uint32_t physAddr, uint32_t nBytes, uint32_t flags);
extern void * __attribute__((stdcall)) _PageAllocate(uint32_t nPages, uint32_t pType, uint32_t VM, uint32_t AlignMask, uint32_t minPhys, uint32_t maxPhys, uint32_t *PhysAddr, uint32_t flags);
extern int    __attribute__((stdcall)) _PageFree(void *hMem, uint32_t flags);
extern uint32_t __attribute__((stdcall)) _CopyPageTable(uint32_t LinPgNum, uint32_t nPages, void *Buffer, uint32_t Flags);
extern void   __attribute__((stdcall)) Out_Debug_String(const char *str);

/* Device extensions for Primary and Secondary heads */
NVX_DEVICE_EXTENSION g_PrimaryDevExt;
NVX_DEVICE_EXTENSION g_SecondaryDevExt;

/* I/O Port helpers */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* COM1 Direct Serial Logging */
static void NvxShimLog(const char *str) {
    while (*str) {
        if (*str == '\n') {
            for (volatile uint32_t to = 0; (inb(0x3FD) & 0x20) == 0 && to < 100000; to++);
            outb(0x3F8, '\r');
        }
        for (volatile uint32_t to = 0; (inb(0x3FD) & 0x20) == 0 && to < 100000; to++);
        outb(0x3F8, (uint8_t)*str++);
    }
}

static void NvxShimLogHex(uint32_t val) {
    char buf[9];
    const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
    NvxShimLog(buf);
}

/* VideoPort Register Access */
ULONG VideoPortReadRegisterUlong(PULONG Register) {
    return *(volatile ULONG *)Register;
}

VOID VideoPortWriteRegisterUlong(PULONG Register, ULONG Value) {
    *(volatile ULONG *)Register = Value;
}

UCHAR VideoPortReadRegisterUchar(PUCHAR Register) {
    return *(volatile UCHAR *)Register;
}

VOID VideoPortWriteRegisterUchar(PUCHAR Register, UCHAR Value) {
    *(volatile UCHAR *)Register = Value;
}

USHORT VideoPortReadRegisterUshort(PUSHORT Register) {
    return *(volatile USHORT *)Register;
}

VOID VideoPortWriteRegisterUshort(PUSHORT Register, USHORT Value) {
    *(volatile USHORT *)Register = Value;
}

/* VideoPort Memory / Device Base */
PVOID VideoPortGetDeviceBase(PVOID HwDeviceExtension, PHYSICAL_ADDRESS IoAddress, ULONG NumberOfBytes, UCHAR InIoSpace) {
    (void)HwDeviceExtension; (void)InIoSpace;
    uint32_t phys = IoAddress.LowPart;
    void *virt = _MapPhysToLinear(phys, NumberOfBytes, 0);
    if (virt == (void *)0xFFFFFFFF) {
        NvxShimLog("NVX [SHIM]: _MapPhysToLinear failed for phys=0x");
        NvxShimLogHex(phys);
        NvxShimLog("\n");
        return NULL;
    }
    return virt;
}

VOID VideoPortFreeDeviceBase(PVOID HwDeviceExtension, PVOID MappedAddress) {
    (void)HwDeviceExtension; (void)MappedAddress;
}

VP_STATUS VideoPortMapMemory(PVOID HwDeviceExtension, PHYSICAL_ADDRESS PhysicalAddress, PULONG Length, PULONG InIoSpace, PVOID *VirtualAddress) {
    (void)HwDeviceExtension; (void)InIoSpace;
    if (!VirtualAddress || !Length) return ERROR_INVALID_PARAMETER;
    uint32_t phys = PhysicalAddress.LowPart;
    *VirtualAddress = _MapPhysToLinear(phys, *Length, 0);
    return (*VirtualAddress != (PVOID)-1 && *VirtualAddress != NULL) ? NO_ERROR : ERROR_INVALID_PARAMETER;
}

VP_STATUS VideoPortUnmapMemory(PVOID HwDeviceExtension, PVOID VirtualAddress, HANDLE ProcessHandle) {
    (void)HwDeviceExtension; (void)VirtualAddress; (void)ProcessHandle;
    return NO_ERROR;
}

/* VideoPort DMA Common Buffer Allocation using VMM _PageAllocate */
PVOID VideoPortAllocateCommonBuffer(PVOID HwDeviceExtension, PVOID DmaAdapter, ULONG DesiredLength, PPHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled, PVOID Reserved) {
    (void)HwDeviceExtension; (void)DmaAdapter; (void)CacheEnabled; (void)Reserved;
    uint32_t pages = (DesiredLength + 4095) / 4096;
    uint32_t phys = 0;

    /* VMM _PageAllocate flags:
     * PAGEZEROINIT (0x0001) | PAGEUSEALIGN (0x0002) | PAGECONTIG (0x0004) | PAGEFIXED (0x0008) = 0x000F
     * pType = 1 (PG_SYS: system global non-pageable memory)
     * minPhys = 0, maxPhys = 0x000FFFFF (pages up to 4GB)
     */
    void *virt = _PageAllocate(pages, 1 /* PG_SYS */, 0, 0, 0, 0x000FFFFF, &phys, 0x0000000F);
    if (!virt) {
        /* Fallback without zero-init */
        virt = _PageAllocate(pages, 1 /* PG_SYS */, 0, 0, 0, 0x000FFFFF, &phys, 0x0000000E);
    }
    if (!virt) {
        /* Fallback with unconstrained pages */
        virt = _PageAllocate(pages, 1 /* PG_SYS */, 0, 0, 0, 0x000FFFFF, &phys, 0x0000000B);
    }
    if (!virt) {
        NvxShimLog("NVX [SHIM]: _PageAllocate failed!\n");
        return NULL;
    }

    uint32_t physAddr = (phys != 0 && phys < 0x100000) ? (phys << 12) : phys;

    /* If phys is still 0, query PTE via _CopyPageTable */
    if (physAddr == 0) {
        uint32_t pte = 0;
        uint32_t ret = _CopyPageTable(((uint32_t)virt) >> 12, 1, &pte, 0);
        NvxShimLog("NVX [SHIM]: _CopyPageTable ret=0x");
        NvxShimLogHex(ret);
        NvxShimLog(" pte=0x");
        NvxShimLogHex(pte);
        NvxShimLog("\n");
        if (ret > 0 && (pte & 0xFFFFF000)) {
            physAddr = (pte & 0xFFFFF000);
        }
    }

    if (LogicalAddress) {
        LogicalAddress->LowPart = physAddr;
        LogicalAddress->HighPart = 0;
    }
    NvxShimLog("NVX [SHIM]: CommonBuffer Virt=0x");
    NvxShimLogHex((uint32_t)virt);
    NvxShimLog(" Phys=0x");
    NvxShimLogHex(physAddr);
    NvxShimLog("\n");
    return virt;
}

VOID VideoPortFreeCommonBuffer(PVOID HwDeviceExtension, ULONG Length, PVOID VirtualAddress, PHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled) {
    (void)HwDeviceExtension; (void)Length; (void)LogicalAddress; (void)CacheEnabled;
    if (VirtualAddress) {
        _PageFree(VirtualAddress, 0);
    }
}

PVOID VideoPortGetDmaAdapter(PVOID HwDeviceExtension, PVP_DEVICE_DESCRIPTION DeviceDescription) {
    (void)HwDeviceExtension; (void)DeviceDescription;
    return (PVOID)1; /* non-NULL dummy handle */
}

VOID VideoPortPutDmaAdapter(PVOID HwDeviceExtension, PVOID DmaAdapter) {
    (void)HwDeviceExtension; (void)DmaAdapter;
}

/* VideoPort PCI Bus Data via Port 0xCF8 / 0xCFC */
static uint32_t PciReadConfig32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000UL | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void PciWriteConfig32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = 0x80000000UL | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

ULONG VideoPortGetBusData(PVOID HwDeviceExtension, BUS_DATA_TYPE BusDataType, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length) {
    (void)HwDeviceExtension; (void)BusDataType;
    uint8_t bus = 0;
    uint8_t dev = (SlotNumber >> 3) & 0x1F;
    uint8_t func = SlotNumber & 0x07;
    uint8_t *dest = (uint8_t *)Buffer;

    for (ULONG i = 0; i < Length; i++) {
        uint8_t curOff = (uint8_t)(Offset + i);
        uint32_t dwordVal = PciReadConfig32(bus, dev, func, curOff & ~3);
        dest[i] = (uint8_t)(dwordVal >> ((curOff & 3) * 8));
    }
    return Length;
}

ULONG VideoPortSetBusData(PVOID HwDeviceExtension, BUS_DATA_TYPE BusDataType, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length) {
    (void)HwDeviceExtension; (void)BusDataType;
    uint8_t bus = 0;
    uint8_t dev = (SlotNumber >> 3) & 0x1F;
    uint8_t func = SlotNumber & 0x07;
    const uint8_t *src = (const uint8_t *)Buffer;

    for (ULONG i = 0; i < Length; i++) {
        uint8_t curOff = (uint8_t)(Offset + i);
        uint32_t regOff = curOff & ~3;
        uint32_t dwordVal = PciReadConfig32(bus, dev, func, regOff);
        uint32_t shift = (curOff & 3) * 8;
        dwordVal &= ~(0xFFUL << shift);
        dwordVal |= ((uint32_t)src[i] << shift);
        PciWriteConfig32(bus, dev, func, regOff, dwordVal);
    }
    return Length;
}

/* VideoPort Access Ranges */
VP_STATUS VideoPortGetAccessRanges(
    PVOID HwDeviceExtension,
    ULONG NumPathRanges,
    PVOID PathRange,
    ULONG NumAccessRanges,
    PVIDEO_ACCESS_RANGE AccessRanges,
    PVOID VendorId,
    PVOID DeviceId,
    PULONG Slot
) {
    (void)HwDeviceExtension; (void)NumPathRanges; (void)PathRange; (void)VendorId; (void)DeviceId;
    if (!AccessRanges || NumAccessRanges < 2) return ERROR_INVALID_PARAMETER;

    /* Scan PCI Bus 0 Dev 2 Func 0 for GA104 BAR0 and BAR1 */
    uint8_t bus = 0, dev = 2, func = 0;
    uint32_t bar0 = PciReadConfig32(bus, dev, func, 0x10) & ~0xF;
    uint32_t bar1 = PciReadConfig32(bus, dev, func, 0x14) & ~0xF;
    uint32_t bar2 = PciReadConfig32(bus, dev, func, 0x1C) & ~0xF;
    uint32_t bar3 = PciReadConfig32(bus, dev, func, 0x24) & ~0x3;

    if (Slot) *Slot = (dev << 3) | func;

    AccessRanges[0].RangeStart.LowPart = bar0 ? bar0 : 0xFD000000;
    AccessRanges[0].RangeStart.HighPart = 0;
    AccessRanges[0].RangeLength = 0x01000000; /* 16 MB */
    AccessRanges[0].RangeInIoSpace = 0;

    AccessRanges[1].RangeStart.LowPart = bar1 ? bar1 : 0xE0000000;
    AccessRanges[1].RangeStart.HighPart = 0;
    AccessRanges[1].RangeLength = 0x10000000; /* 256 MB */
    AccessRanges[1].RangeInIoSpace = 0;

    AccessRanges[2].RangeStart.LowPart = bar2 ? bar2 : 0xF0000000;
    AccessRanges[2].RangeStart.HighPart = 0;
    AccessRanges[2].RangeLength = 0x02000000; /* 32 MB */
    AccessRanges[2].RangeInIoSpace = 0;

    AccessRanges[3].RangeStart.LowPart = bar3 ? bar3 : 0xC000;
    AccessRanges[3].RangeStart.HighPart = 0;
    AccessRanges[3].RangeLength = 0x80;
    AccessRanges[3].RangeInIoSpace = 1;

    return NO_ERROR;
}

VP_STATUS VideoPortCreateSecondaryDisplay(PVOID HwDeviceExtension, PVOID *SecondaryDeviceExtension, ULONG Flag) {
    (void)HwDeviceExtension; (void)Flag;
    if (!SecondaryDeviceExtension) return ERROR_INVALID_PARAMETER;
    *SecondaryDeviceExtension = (PVOID)&g_SecondaryDevExt;
    return NO_ERROR;
}

VP_STATUS VideoPortEnumerateChildren(PVOID HwDeviceExtension, PVOID Reserved) {
    (void)HwDeviceExtension; (void)Reserved;
    return NO_ERROR;
}

VOID VideoPortStallExecution(ULONG Microseconds) {
    for (volatile ULONG i = 0; i < Microseconds * 20; i++) {
        __asm__ __volatile__("outb %%al, $0x80" : : : "memory");
    }
}

VOID VideoPortZeroMemory(PVOID Destination, ULONG Length) {
    if (Destination && Length) {
        uint8_t *d = (uint8_t *)Destination;
        while (Length--) *d++ = 0;
    }
}

VOID VideoPortMoveMemory(PVOID Destination, PVOID Source, ULONG Length) {
    if (Destination && Source && Length) {
        uint8_t *d = (uint8_t *)Destination;
        const uint8_t *s = (const uint8_t *)Source;
        if (d < s) {
            while (Length--) *d++ = *s++;
        } else {
            d += Length; s += Length;
            while (Length--) *--d = *--s;
        }
    }
}

VOID VideoPortDebugPrint(VIDEO_DEBUG_LEVEL DebugPrintLevel, PCSTR DebugMessage, ...) {
    (void)DebugPrintLevel;
    (void)DebugMessage;
}

/* Win98 / NT Shims */
UCHAR __attribute__((stdcall)) KeGetCurrentIrql(VOID) {
    return 0;
}

uint32_t __attribute__((stdcall)) ZwCreateFile(void *a, uint32_t b, void *c, void *d, void *e, uint32_t f, uint32_t g, uint32_t h, uint32_t i, void *j, uint32_t k) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;
    return 0xC0000002;
}

uint32_t __attribute__((stdcall)) ZwWriteFile(void *a, void *b, void *c, void *d, void *e, void *f, uint32_t g, void *h, void *i) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;
    return 0xC0000002;
}

/* Basic C runtime memory routines */
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    VideoPortMoveMemory(dest, (PVOID)src, n);
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    return memmove(dest, src, n);
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* VideoPortInitialize for Windows 98 SE */
ULONG VideoPortInitialize(PVOID Context1, PVOID Context2, PVIDEO_HW_INITIALIZATION_DATA HwInitData, PVOID HwContext) {
    (void)Context1; (void)Context2; (void)HwContext;
    NvxShimLog("NVX [SHIM]: VideoPortInitialize entered\n");
    VIDEO_PORT_CONFIG_INFO configInfo;
    VideoPortZeroMemory(&configInfo, sizeof(configInfo));
    configInfo.Length = sizeof(configInfo);
    configInfo.SystemIoBusNumber = 0;

    VideoPortZeroMemory(&g_PrimaryDevExt, sizeof(g_PrimaryDevExt));
    VideoPortZeroMemory(&g_SecondaryDevExt, sizeof(g_SecondaryDevExt));

    /* 1. Find Primary Adapter */
    UCHAR again = 0;
    ULONG status = HwInitData->HwFindAdapter(&g_PrimaryDevExt, NULL, NULL, &configInfo, &again);
    if (status != NO_ERROR) {
        NvxShimLog("NVX [SHIM]: HwFindAdapter failed!\n");
        return status;
    }

    /* 2. Initialize Primary Head (Head 0, Dell 1080p) */
    if (!HwInitData->HwInitialize(&g_PrimaryDevExt)) {
        NvxShimLog("NVX [SHIM]: Primary HwInitialize failed!\n");
        return 1;
    }

    /* 3. Initialize Secondary Head (Head 1, Lenovo 1440p) */
    if (g_PrimaryDevExt.SecondaryDeviceExtension) {
        if (!HwInitData->HwInitialize(g_PrimaryDevExt.SecondaryDeviceExtension)) {
            NvxShimLog("NVX [SHIM]: Secondary HwInitialize failed!\n");
        }
    }

    return NO_ERROR;
}

/* GSP Firmware Dynamic Pointers */
const unsigned char *g_pNvxFirmwareStart = NULL;
const unsigned char *g_pNvxFirmwareEnd = NULL;

extern int __attribute__((stdcall)) _R0_OpenFile(const char *path, uint32_t *phFile);
extern int __attribute__((stdcall)) _R0_ReadFileAt(uint32_t hFile, uint32_t count, void *buf, uint32_t fileOffset, uint32_t *pBytesRead);
extern int __attribute__((stdcall)) _R0_CloseFile(uint32_t hFile);

static int NvxLoadFirmwareIntoMemory(VOID) {
    if (g_pNvxFirmwareStart) return 1;

    const char *paths[] = {
        "C:\\WINDOWS\\SYSTEM\\GSP_GA104.BIN",
        "C:\\GSP_GA104.BIN",
        "C:\\DRIVER\\GSP_GA104.BIN"
    };

    uint32_t hFile = 0;
    int found = 0;
    for (int i = 0; i < 3; i++) {
        NvxShimLog("NVX [FIRMWARE]: Probing for GSP container: ");
        NvxShimLog(paths[i]);
        NvxShimLog("\n");
        if (_R0_OpenFile(paths[i], &hFile) == 0 && hFile != 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        NvxShimLog("NVX [FIRMWARE]: GSP container not found on disk!\n");
        return 0;
    }

    uint32_t totalBytes = 38061600;
    uint32_t pages = (totalBytes + 4095) / 4096;
    uint32_t phys = 0;

    void *fwBuf = _PageAllocate(pages, 1 /* PG_SYS */, 0, 0, 0, 0x000FFFFF, &phys, 0x00000009);
    if (!fwBuf) {
        fwBuf = _PageAllocate(pages, 1 /* PG_SYS */, 0, 0, 0, 0x000FFFFF, &phys, 0x00000001);
    }
    if (!fwBuf) {
        NvxShimLog("NVX [FIRMWARE]: _PageAllocate failed for firmware staging buffer!\n");
        _R0_CloseFile(hFile);
        return 0;
    }

    NvxShimLog("NVX [FIRMWARE]: Staging buffer allocated at 0x");
    NvxShimLogHex((uint32_t)fwBuf);
    NvxShimLog(", reading firmware payload...\n");
    uint8_t *dst = (uint8_t *)fwBuf;
    uint32_t bytesRemaining = totalBytes;
    uint32_t totalRead = 0;
    uint32_t lastDot = 0;

    while (bytesRemaining > 0) {
        uint32_t chunk = (bytesRemaining > 32768) ? 32768 : bytesRemaining;
        uint32_t bytesRead = 0;
        int err = _R0_ReadFileAt(hFile, chunk, dst + totalRead, totalRead, &bytesRead);
        if (err != 0 || bytesRead == 0) {
            NvxShimLog("\nNVX [FIRMWARE]: ReadFile failed! err=0x");
            NvxShimLogHex((uint32_t)err);
            NvxShimLog(" bytesRead=0x");
            NvxShimLogHex(bytesRead);
            NvxShimLog(" at totalRead=0x");
            NvxShimLogHex(totalRead);
            NvxShimLog("\n");
            break;
        }
        totalRead += bytesRead;
        bytesRemaining -= bytesRead;
        if (totalRead - lastDot >= 4 * 1024 * 1024) {
            NvxShimLog(".");
            lastDot = totalRead;
        }
    }
    NvxShimLog("\n");

    _R0_CloseFile(hFile);

    if (totalRead < 38000000) {
        NvxShimLog("NVX [FIRMWARE]: Incomplete firmware read!\n");
        _PageFree(fwBuf, 0);
        return 0;
    }

    NvxShimLog("NVX [FIRMWARE]: Authentic GSP container loaded (36.28MB)!\n");
    g_pNvxFirmwareStart = (const unsigned char *)fwBuf;
    g_pNvxFirmwareEnd = (const unsigned char *)fwBuf + totalRead;
    return 1;
}

/* VxD Device Init Entry Point (called from vxd_entry.asm) */
extern ULONG DriverEntry(PVOID Context1, PVOID Context2);

static void NvxEnableWriteCombining(uint32_t base, uint32_t size) {
    if (!base || !size) return;
    uint32_t cap_lo = 0, cap_hi = 0;
    __asm__ __volatile__("rdmsr" : "=a"(cap_lo), "=d"(cap_hi) : "c"(0xFE));
    if (!(cap_lo & 0x400)) {
        NvxShimLog("NVX [MTRR]: WC not supported by CPU MTRRcap\n");
        return;
    }
    uint32_t vcnt = cap_lo & 0xFF;
    for (uint32_t i = 0; i < vcnt && i < 16; i++) {
        uint32_t mask_lo = 0, mask_hi = 0;
        __asm__ __volatile__("rdmsr" : "=a"(mask_lo), "=d"(mask_hi) : "c"(0x201 + 2 * i));
        if (!(mask_lo & 0x800)) { /* Found an unallocated variable MTRR */
            uint32_t base_val = (base & 0xFFFFF000) | 1; /* Type 1 = Write Combining */
            uint32_t mask_val = (~(size - 1) & 0xFFFFF000) | 0x800; /* Valid bit 11 */
            __asm__ __volatile__("wrmsr" : : "a"(base_val), "d"(0), "c"(0x200 + 2 * i));
            __asm__ __volatile__("wrmsr" : : "a"(mask_val), "d"(0x0F), "c"(0x201 + 2 * i));
            NvxShimLog("NVX [MTRR]: Hardware Write Combining enabled for 0x");
            NvxShimLogHex(base);
            NvxShimLog(" (size 0x");
            NvxShimLogHex(size);
            NvxShimLog(") in MTRR ");
            NvxShimLogHex(i);
            NvxShimLog("\n");
            return;
        }
    }
}

VOID NvxVxDDeviceInit(VOID) {
    NvxShimLog("\n========================================\n");
    NvxShimLog(" NVX Miniport Driver (GA104 Win98 Ring 0 Authentic GSP)\n");
    NvxShimLog("========================================\n");
    NvxShimLog("NVX [SHIM]: Calling DriverEntry...\n");
    ULONG st = DriverEntry(NULL, NULL);
    NvxShimLog("NVX [SHIM]: DriverEntry returned: 0x");
    NvxShimLogHex(st);
    NvxShimLog("\n");
    /* MTRR manipulation via rdmsr/wrmsr deferred to avoid hypervisor #GP faults */
    /* if (g_PrimaryDevExt.PhysicalBar1.LowPart) {
        NvxEnableWriteCombining(g_PrimaryDevExt.PhysicalBar1.LowPart, 0x10000000);
    } */
}

VOID NvxBootstrapGsp(VOID) {
    if (g_PrimaryDevExt.GspInitialized) return;

    NvxShimLog("\nNVX [GSP]: Starting deferred GSP bootstrap (post-Init_Complete)...\n");
    if (!NvxLoadFirmwareIntoMemory()) {
        NvxShimLog("NVX [GSP]: Firmware loading failed or deferred.\n");
        return;
    }

    PUCHAR bar0 = (PUCHAR)g_PrimaryDevExt.Bar0MmioVirtual;
    ULONG vbiosBar1Block = 0;
    if (bar0) {
        vbiosBar1Block = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1704));
        NvxShimLog("NVX [BAR1]: VBIOS 0x1704 (NV_PBUS_BAR1_BLOCK) = 0x");
        NvxShimLogHex(vbiosBar1Block);
        NvxShimLog("\n");
        NvxProbeFwsecRom(&g_PrimaryDevExt, bar0);
    }

    BOOLEAN ok = NvxGspInitSubsystem(&g_PrimaryDevExt);
    NvxShimLog("NVX [GSP]: NvxGspInitSubsystem returned: ");
    NvxShimLogHex(ok);
    NvxShimLog("\n");

    if (g_PrimaryDevExt.GspCopyVerified) {
        NvxShimLog("NVX [NATIVE]: Win98 GSP copy verified! Training secondary DP link...\n");
        NvxGspTrainSecondaryDpLink(&g_PrimaryDevExt);
    }

    if (bar0) {
        ULONG gspBar1Block = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1704));
        NvxShimLog("NVX [BAR1]: GSP 0x1704 = 0x");
        NvxShimLogHex(gspBar1Block);
        NvxShimLog(" -> Restoring VBIOS 0x1704: 0x");
        NvxShimLogHex(vbiosBar1Block);
        NvxShimLog("\n");
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x1704), vbiosBar1Block);
        ULONG restoredBar1Block = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1704));
        NvxShimLog("NVX [BAR1]: Verified 0x1704 after restore = 0x");
        NvxShimLogHex(restoredBar1Block);
        NvxShimLog("\n");
    }

    if (g_pNvxFirmwareStart) {
        _PageFree((void *)g_pNvxFirmwareStart, 0);
        g_pNvxFirmwareStart = NULL;
        g_pNvxFirmwareEnd = NULL;
    }
}

VOID NvxVxDInitComplete(VOID) {
    NvxShimLog("NVX: Init_Complete message received!\n");
    NvxBootstrapGsp();
}

/* VxD DeviceIOControl Entry Point (called from vxd_entry.asm) */
#define IOCTL_NVX_GET_HEAD0_INFO  0x0001
#define IOCTL_NVX_GET_HEAD1_INFO  0x0002
#define IOCTL_NVX_GPU_COPY        0x0003
#define IOCTL_NVX_INIT_GSP        0x0004
#define IOCTL_NVX_TRAIN_DP        0x0005
#define IOCTL_NVX_SET_SECONDARY_SCANOUT 0x0006

typedef struct _NVX_HEAD_INFO {
    uint32_t PhysLfb;
    uint32_t Width;
    uint32_t Height;
    uint32_t Pitch;
    uint32_t Bpp;
} NVX_HEAD_INFO;

int NvxVxDDeviceIoControl(DIOCParams *params) {
    if (!params) return 0;
    NvxShimLog("NVX [IOCTL]: Received dwIoControlCode=0x");
    NvxShimLogHex(params->dwIoControlCode);
    NvxShimLog("\n");
    switch (params->dwIoControlCode) {
        case 0: /* DIOC_OPEN */
        case -1: /* DIOC_CLOSEHANDLE */
            return 0;

        case IOCTL_NVX_GET_HEAD0_INFO: {
            if (params->lpvOutBuffer && params->cbOutBuffer >= sizeof(NVX_HEAD_INFO)) {
                NVX_HEAD_INFO *info = (NVX_HEAD_INFO *)params->lpvOutBuffer;
                info->PhysLfb = g_PrimaryDevExt.PhysicalBar1.LowPart;
                info->Width = g_PrimaryDevExt.ActiveWidth ? g_PrimaryDevExt.ActiveWidth : 1920;
                info->Height = g_PrimaryDevExt.ActiveHeight ? g_PrimaryDevExt.ActiveHeight : 1080;
                info->Pitch = info->Width * 4;
                info->Bpp = 32;
                if (params->lpcbBytesReturned) *params->lpcbBytesReturned = sizeof(NVX_HEAD_INFO);
                return 0;
            }
            return -1;
        }

        case IOCTL_NVX_GET_HEAD1_INFO: {
            if (params->lpvOutBuffer && params->cbOutBuffer >= sizeof(NVX_HEAD_INFO)) {
                NVX_HEAD_INFO *info = (NVX_HEAD_INFO *)params->lpvOutBuffer;
                info->PhysLfb = g_SecondaryDevExt.PhysicalBar1.LowPart;
                info->Width = g_SecondaryDevExt.ActiveWidth ? g_SecondaryDevExt.ActiveWidth : 2560;
                info->Height = g_SecondaryDevExt.ActiveHeight ? g_SecondaryDevExt.ActiveHeight : 1440;
                info->Pitch = info->Width * 4;
                info->Bpp = 32;
                if (params->lpcbBytesReturned) *params->lpcbBytesReturned = sizeof(NVX_HEAD_INFO);
                return 0;
            }
            return -1;
        }

        case IOCTL_NVX_GPU_COPY:
        case IOCTL_NVX_INIT_GSP:
            NvxBootstrapGsp();
            return 0;

        case IOCTL_NVX_TRAIN_DP:
            if (g_PrimaryDevExt.GspInitialized && g_PrimaryDevExt.SecondaryDpTrainReady) {
                NvxShimLog("NVX [DP]: Secondary DP link training requested via IOCTL.\n");
                NvxGspTrainSecondaryDpLink(&g_PrimaryDevExt);
                return 0;
            }
            return -1;

        case IOCTL_NVX_SET_SECONDARY_SCANOUT:
            NvxShimLog("NVX [IOCTL]: Triggering dual hardware scanout...\n");
            if (!g_PrimaryDevExt.FramebufferVirtual && g_PrimaryDevExt.PhysicalBar1.LowPart) {
                g_PrimaryDevExt.FramebufferVirtual = _MapPhysToLinear(g_PrimaryDevExt.PhysicalBar1.LowPart, 0x01000000, 0);
            }
            BOOLEAN scanOk = NvxNativeSetSecondaryScanout(&g_PrimaryDevExt);
            NvxShimLog("NVX [IOCTL]: NvxNativeSetSecondaryScanout returned: ");
            NvxShimLogHex(scanOk);
            NvxShimLog("\n");
            return scanOk ? 0 : -1;

        default:
            return 0;
    }
}
