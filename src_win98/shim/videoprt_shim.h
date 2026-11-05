#ifndef VIDEOPRT_SHIM_H
#define VIDEOPRT_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VideoPort status and basic types */
typedef int32_t VP_STATUS;
#define NO_ERROR                    0L
#define ERROR_INVALID_PARAMETER     87L
#define ERROR_NOT_ENOUGH_MEMORY     8L
#define ERROR_DEV_NOT_EXIST         55L

typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint8_t  UCHAR;
typedef void     VOID;
typedef void    *PVOID;
typedef void    *HANDLE;
typedef int      BOOL;
typedef uint8_t  BOOLEAN;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef union _LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t  HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

typedef enum _VIDEO_PORT_CACHE_TYPE {
    VpNonCached = 0,
    VpWriteCombined,
    VpCached
} VIDEO_PORT_CACHE_TYPE;

typedef struct _VIDEO_ACCESS_RANGE {
    PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    UCHAR RangeInMemory;
    UCHAR RangeShareable;
    UCHAR RangePassiveDecode;
} VIDEO_ACCESS_RANGE, *PVIDEO_ACCESS_RANGE;

typedef struct _VIDEO_PORT_CONFIG_INFO {
    ULONG Length;
    ULONG SystemIoBusNumber;
    ULONG SlotNumber;
    ULONG BusInterruptLevel;
    ULONG BusInterruptVector;
    ULONG InterruptMode;
    ULONG NumAccessRanges;
    PVIDEO_ACCESS_RANGE AccessRanges;
    ULONG VdmPhysicalVideoMemoryAddress;
    ULONG VdmPhysicalVideoMemoryLength;
} VIDEO_PORT_CONFIG_INFO, *PVIDEO_PORT_CONFIG_INFO;

/* VMM / VxD Service structures */
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

/* Miniport functions shimmed into VxD */
void     VideoPortZeroMemory(PVOID Destination, ULONG Length);
void     VideoPortMoveMemory(PVOID Destination, PVOID Source, ULONG Length);
PVOID    VideoPortGetDeviceBase(PVOID HwDeviceExtension, PHYSICAL_ADDRESS IoAddress, ULONG NumberOfBytes, UCHAR InIoSpace);
VOID     VideoPortFreeDeviceBase(PVOID HwDeviceExtension, PVOID MappedAddress);
VP_STATUS VideoPortMapMemory(PVOID HwDeviceExtension, PHYSICAL_ADDRESS PhysicalAddress, ULONG *Length, ULONG *InIoSpace, PVOID *VirtualAddress);
VP_STATUS VideoPortUnmapMemory(PVOID HwDeviceExtension, PVOID VirtualAddress, HANDLE ProcessHandle);
ULONG    VideoPortGetBusData(PVOID HwDeviceExtension, ULONG BusDataType, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length);
ULONG    VideoPortSetBusData(PVOID HwDeviceExtension, ULONG BusDataType, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length);
VOID     VideoPortStallExecution(ULONG Microseconds);
VOID     VideoPortDebugPrint(ULONG DebugPrintLevel, const char *DebugMessage, ...);
PVOID    VideoPortAllocateCommonBuffer(PVOID HwDeviceExtension, PVOID DmaAdapter, ULONG DesiredLength, PPHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled, PVOID Reserved);
VOID     VideoPortFreeCommonBuffer(PVOID HwDeviceExtension, ULONG Length, PVOID VirtualAddress, PHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled);

#ifdef __cplusplus
}
#endif

#endif /* VIDEOPRT_SHIM_H */
