#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#define FILE_DEVICE_VIDEO 0x00000023

typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;
typedef LONG VP_STATUS;

#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#ifndef ERROR_INVALID_FUNCTION
#define ERROR_INVALID_FUNCTION 1
#endif
#ifndef ERROR_DEV_NOT_EXIST
#define ERROR_DEV_NOT_EXIST 55
#endif
#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87
#endif
#ifndef ERROR_INSUFFICIENT_BUFFER
#define ERROR_INSUFFICIENT_BUFFER 122
#endif
#ifndef ERROR_NOT_SUPPORTED
#define ERROR_NOT_SUPPORTED 50
#endif
#ifndef ERROR_DEVICE_NOT_READY
#define ERROR_DEVICE_NOT_READY 21
#endif

#define VIDEO_ENUM_MORE_DEVICES    234  /* ERROR_MORE_DATA (0xEA) */
#define VIDEO_ENUM_NO_MORE_DEVICES 259  /* ERROR_NO_MORE_ITEMS (0x103) */
#define VIDEO_ENUM_INVALID_DEVICE  123  /* ERROR_INVALID_NAME */

typedef enum _INTERFACE_TYPE {
    InterfaceTypeUndefined = -1,
    Internal,
    Isa,
    Eisa,
    MicroChannel,
    TurboChannel,
    PCIBus,
    VMEBus,
    NuBus,
    PCMCIABus,
    CBus,
    MPIBus,
    MPSABus,
    ProcessorInternal,
    InternalPowerBus,
    PNPISABus,
    PNPBus,
    MaximumInterfaceType
} INTERFACE_TYPE, *PINTERFACE_TYPE;

typedef enum _VIDEO_DEBUG_LEVEL {
    Error = 0,
    Warn,
    Trace,
    Info
} VIDEO_DEBUG_LEVEL, *PVIDEO_DEBUG_LEVEL;

typedef struct _VIDEO_ACCESS_RANGE {
    PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    UCHAR RangeInIoSpace;
    UCHAR RangeVisible;
    UCHAR RangeShareable;
    UCHAR RangePassiveDecode;
} VIDEO_ACCESS_RANGE, *PVIDEO_ACCESS_RANGE;

typedef struct _VIDEO_PORT_CONFIG_INFO {
    ULONG Length;
    ULONG SystemIoBusNumber;
    INTERFACE_TYPE AdapterInterfaceType;
    ULONG BusInterruptLevel;
    ULONG BusInterruptVector;
    ULONG InterruptMode;
    ULONG NumEmulatorAccessEntries;
    PVOID EmulatorAccessEntries;
    ULONG_PTR EmulatorAccessEntriesContext;
    PHYSICAL_ADDRESS VdmPhysicalVideoMemoryAddress;
    ULONG VdmPhysicalVideoMemoryLength;
    ULONG HardwareStateSize;
    ULONG DmaChannel;
    ULONG DmaPort;
    UCHAR DmaShareable;
    UCHAR InterruptShareable;
    BOOLEAN Master;
    ULONG DmaWidth;
    ULONG DmaSpeed;
    BOOLEAN bMapBuffers;
    BOOLEAN NeedPhysicalAddresses;
    BOOLEAN DemandMode;
    ULONG MaximumTransferLength;
    ULONG NumberOfPhysicalBreaks;
    BOOLEAN ScatterGather;
    ULONG MaximumScatterGatherChunkSize;
    PVOID VideoPortGetProcAddress;
    PWSTR DriverRegistryPath;
    ULONGLONG SystemMemorySize;
} VIDEO_PORT_CONFIG_INFO, *PVIDEO_PORT_CONFIG_INFO;

typedef struct _VIDEO_CHILD_ENUM_INFO {
    ULONG Size;
    ULONG ChildDescriptorSize;
    ULONG ChildIndex;
    ULONG ACPIHwId;
    PVOID ChildHwDeviceExtension;
} VIDEO_CHILD_ENUM_INFO, *PVIDEO_CHILD_ENUM_INFO;

typedef enum _VIDEO_CHILD_TYPE {
    Monitor = 1,
    NonPrimaryChip,
    VideoChip,
    Other
} VIDEO_CHILD_TYPE, *PVIDEO_CHILD_TYPE;

typedef enum _VIDEO_POWER_STATE {
    VideoPowerUnspecified = 0,
    VideoPowerOn = 1,
    VideoPowerStandBy,
    VideoPowerSuspend,
    VideoPowerOff,
    VideoPowerHibernate,
    VideoPowerShutdown,
    VideoPowerMaximum
} VIDEO_POWER_STATE, *PVIDEO_POWER_STATE;

typedef struct _VIDEO_POWER_MANAGEMENT {
    ULONG Length;
    ULONG DPMSVersion;
    ULONG PowerState;
} VIDEO_POWER_MANAGEMENT, *PVIDEO_POWER_MANAGEMENT;

#ifndef NTAPI
#define NTAPI __stdcall
#endif
#ifndef VPAPI
#define VPAPI NTAPI
#endif

typedef VP_STATUS (NTAPI *PVIDEO_HW_FIND_ADAPTER)(
    PVOID HwDeviceExtension,
    PVOID HwContext,
    PWSTR ArgumentString,
    PVIDEO_PORT_CONFIG_INFO ConfigInfo,
    PUCHAR Again
);

typedef BOOLEAN (NTAPI *PVIDEO_HW_INITIALIZE)(PVOID HwDeviceExtension);
typedef BOOLEAN (NTAPI *PVIDEO_HW_INTERRUPT)(PVOID HwDeviceExtension);

typedef struct _STATUS_BLOCK {
    VP_STATUS Status;
    ULONG_PTR Information;
} STATUS_BLOCK, *PSTATUS_BLOCK;

typedef struct _VIDEO_REQUEST_PACKET {
    ULONG IoControlCode;
    PSTATUS_BLOCK StatusBlock;
    PVOID InputBuffer;
    ULONG InputBufferLength;
    PVOID OutputBuffer;
    ULONG OutputBufferLength;
} VIDEO_REQUEST_PACKET, *PVIDEO_REQUEST_PACKET;

typedef BOOLEAN (NTAPI *PVIDEO_HW_START_IO)(
    PVOID HwDeviceExtension,
    PVIDEO_REQUEST_PACKET RequestPacket
);

typedef BOOLEAN (NTAPI *PVIDEO_HW_RESET_HW)(
    PVOID HwDeviceExtension,
    ULONG Columns,
    ULONG Rows
);

typedef VP_STATUS (NTAPI *PVIDEO_HW_SET_POWER_STATE)(
    PVOID HwDeviceExtension,
    ULONG HwId,
    PVIDEO_POWER_MANAGEMENT VideoPowerControl
);

typedef VP_STATUS (NTAPI *PVIDEO_HW_GET_POWER_STATE)(
    PVOID HwDeviceExtension,
    ULONG HwId,
    PVIDEO_POWER_MANAGEMENT VideoPowerControl
);

typedef VP_STATUS (NTAPI *PVIDEO_HW_GET_CHILD_DESCRIPTOR)(
    PVOID HwDeviceExtension,
    PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
    PVIDEO_CHILD_TYPE VideoChildType,
    PUCHAR pChildDescriptor,
    PULONG UId,
    PULONG pUnused
);

/*
 * Standard 64-bit VIDEO_HW_INITIALIZATION_DATA (144 bytes = 0x90)
 */
typedef struct _VIDEO_HW_INITIALIZATION_DATA {
    ULONG HwInitDataSize;
    INTERFACE_TYPE AdapterInterfaceType;
    PVIDEO_HW_FIND_ADAPTER HwFindAdapter;
    PVIDEO_HW_INITIALIZE HwInitialize;
    PVIDEO_HW_INTERRUPT HwInterrupt;
    PVIDEO_HW_START_IO HwStartIO;
    ULONG HwDeviceExtensionSize;
    ULONG StartingDeviceNumber;
    PVIDEO_HW_RESET_HW HwResetHw;
    PVOID HwTimer;
    PVOID HwStartDma;
    PVIDEO_HW_SET_POWER_STATE HwSetPowerState;
    PVIDEO_HW_GET_POWER_STATE HwGetPowerState;
    PVIDEO_HW_GET_CHILD_DESCRIPTOR HwGetVideoChildDescriptor;
    PVOID HwQueryInterface;
    ULONG HwChildDeviceExtensionSize;
    PVIDEO_ACCESS_RANGE HwLegacyResourceList;
    ULONG HwLegacyResourceCount;
    PVOID HwGetLegacyResources;
    BOOLEAN AllowEarlyEnumeration;
    ULONG Reserved;
} VIDEO_HW_INITIALIZATION_DATA, *PVIDEO_HW_INITIALIZATION_DATA;

typedef struct _VIDEO_NUM_MODES {
    ULONG NumModes;
    ULONG ModeInformationLength;
} VIDEO_NUM_MODES, *PVIDEO_NUM_MODES;

typedef struct _VIDEO_MODE_INFORMATION {
    ULONG Length;
    ULONG ModeIndex;
    ULONG VisScreenWidth;
    ULONG VisScreenHeight;
    ULONG ScreenStride;
    ULONG NumberOfPlanes;
    ULONG BitsPerPlane;
    ULONG Frequency;
    ULONG XMillimeter;
    ULONG YMillimeter;
    ULONG NumberRedBits;
    ULONG NumberGreenBits;
    ULONG NumberBlueBits;
    ULONG RedMask;
    ULONG GreenMask;
    ULONG BlueMask;
    ULONG AttributeFlags;
    ULONG VideoMemoryBitmapWidth;
    ULONG VideoMemoryBitmapHeight;
    ULONG DriverSpecificAttributeFlags;
} VIDEO_MODE_INFORMATION, *PVIDEO_MODE_INFORMATION;

#define VIDEO_MODE_COLOR    0x0001
#define VIDEO_MODE_GRAPHICS 0x0002

typedef struct _VIDEO_MODE {
    ULONG RequestedMode;
} VIDEO_MODE, *PVIDEO_MODE;

typedef struct _VIDEO_MEMORY {
    PVOID RequestedVirtualAddress;
} VIDEO_MEMORY, *PVIDEO_MEMORY;

typedef struct _VIDEO_MEMORY_INFORMATION {
    PVOID VideoRamBase;
    ULONG VideoRamLength;
    PVOID FrameBufferBase;
    ULONG FrameBufferLength;
} VIDEO_MEMORY_INFORMATION, *PVIDEO_MEMORY_INFORMATION;

typedef struct _VIDEO_SHARE_MEMORY {
    PVOID RequestedVirtualAddress;
    ULONG RequestedLength;
    PVOID VirtualAddress;
    ULONG SharedViewOffset;
    ULONG SharedViewSize;
} VIDEO_SHARE_MEMORY, *PVIDEO_SHARE_MEMORY;

#define IOCTL_VIDEO_QUERY_AVAIL_MODES     CTL_CODE(FILE_DEVICE_VIDEO, 0x100, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230400 */
#define IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES CTL_CODE(FILE_DEVICE_VIDEO, 0x101, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230404 */
#define IOCTL_VIDEO_QUERY_CURRENT_MODE   CTL_CODE(FILE_DEVICE_VIDEO, 0x102, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230408 */
#define IOCTL_VIDEO_SET_CURRENT_MODE     CTL_CODE(FILE_DEVICE_VIDEO, 0x103, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x0023040C */
#define IOCTL_VIDEO_RESET_DEVICE         CTL_CODE(FILE_DEVICE_VIDEO, 0x104, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230410 */
#define IOCTL_VIDEO_SET_COLOR_REGISTERS  CTL_CODE(FILE_DEVICE_VIDEO, 0x107, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x0023041C */
#define IOCTL_VIDEO_MAP_VIDEO_MEMORY     CTL_CODE(FILE_DEVICE_VIDEO, 0x116, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230458 */
#define IOCTL_VIDEO_UNMAP_VIDEO_MEMORY   CTL_CODE(FILE_DEVICE_VIDEO, 0x117, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x0023045C */
#define IOCTL_VIDEO_SWITCH_DUALVIEW      CTL_CODE(FILE_DEVICE_VIDEO, 0x123, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x0023048C */

#define VIDEO_DUALVIEW_REMOVABLE 0x00000001UL

/* Custom Miniport Hardware Acceleration IOCTLs */
#define IOCTL_NVX_ACCEL_COPY             CTL_CODE(FILE_DEVICE_VIDEO, 0x200, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_ACCEL_FILL             CTL_CODE(FILE_DEVICE_VIDEO, 0x201, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_ACCEL_UPLOAD           CTL_CODE(FILE_DEVICE_VIDEO, 0x202, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_SCANOUT_GPU_VA         CTL_CODE(FILE_DEVICE_VIDEO, 0x203, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_DIAGNOSTIC_TRACE       CTL_CODE(FILE_DEVICE_VIDEO, 0x204, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_DIAGNOSTIC_STATE       CTL_CODE(FILE_DEVICE_VIDEO, 0x205, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_GET_LOG                CTL_CODE(FILE_DEVICE_VIDEO, 0x206, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define NVX_UPLOAD_KIND_COPYBITS         1UL
#define NVX_UPLOAD_KIND_BITBLT           2UL
#define NVX_UPLOAD_KIND_TEXTOUT          3UL

typedef struct _NVX_ACCEL_COPY_PARAMS {
    ULONG64 DstGpuVa;
    ULONG64 SrcGpuVa;
    ULONG   BytesPerLine;
    ULONG   SrcPitch;
    ULONG   DstPitch;
    ULONG   LineCount;
} NVX_ACCEL_COPY_PARAMS, *PNVX_ACCEL_COPY_PARAMS;

typedef struct _NVX_ACCEL_FILL_PARAMS {
    ULONG64 DstGpuVa;
    ULONG   Color;
    ULONG   Bytes;
} NVX_ACCEL_FILL_PARAMS, *PNVX_ACCEL_FILL_PARAMS;

typedef struct _NVX_ACCEL_UPLOAD_PARAMS {
    ULONG64 DstGpuVa;
    ULONG   Bytes;
    ULONG   Reserved;
    UCHAR   Data[1];
} NVX_ACCEL_UPLOAD_PARAMS, *PNVX_ACCEL_UPLOAD_PARAMS;

typedef struct _NVX_DIAGNOSTIC_STATE {
    ULONG Flags;
    ULONG NumModes;
    ULONG CurrentModeIndex;
    ULONG MonitorCount;
    ULONG64 GspVramGpuVa;
} NVX_DIAGNOSTIC_STATE, *PNVX_DIAGNOSTIC_STATE;

typedef enum _BUS_DATA_TYPE {
    ConfigurationSpaceUndefined = -1,
    Cmos,
    EisaConfiguration,
    Pos,
    CbusConfiguration,
    PCIConfiguration,
    VMEConfiguration,
    NuBusConfiguration,
    PCMCIAConfiguration,
    MPIConfiguration,
    MPSAConfiguration,
    PNPISAConfiguration,
    SgiInternalConfiguration,
    MaximumBusDataType
} BUS_DATA_TYPE, *PBUS_DATA_TYPE;

typedef struct _PCI_COMMON_CONFIG {
    USHORT VendorID;
    USHORT DeviceID;
    USHORT Command;
    USHORT Status;
    UCHAR RevisionID;
    UCHAR ProgIf;
    UCHAR SubClass;
    UCHAR BaseClass;
    UCHAR CacheLineSize;
    UCHAR LatencyTimer;
    UCHAR HeaderType;
    UCHAR BIST;
    ULONG BaseAddresses[6];
    ULONG CIS;
    USHORT SubVendorID;
    USHORT SubSystemID;
    ULONG ROMBaseAddress;
    UCHAR CapabilitiesPtr;
    UCHAR Reserved1[3];
    ULONG Reserved2;
    UCHAR InterruptLine;
    UCHAR InterruptPin;
    UCHAR MinimumGrant;
    UCHAR MaximumLatency;
} PCI_COMMON_CONFIG, *PPCI_COMMON_CONFIG;

/* VideoPort API function prototypes */
ULONG VPAPI VideoPortInitialize(
    PVOID Context1,
    PVOID Context2,
    PVIDEO_HW_INITIALIZATION_DATA HwInitializationData,
    PVOID HwContext
);

VOID VPAPI VideoPortZeroMemory(PVOID Destination, ULONG Length);
VOID VPAPI VideoPortMoveMemory(PVOID Destination, PVOID Source, ULONG Length);

PVOID VPAPI VideoPortGetDeviceBase(
    PVOID HwDeviceExtension,
    PHYSICAL_ADDRESS IoAddress,
    ULONG NumberOfBytes,
    UCHAR InIoSpace
);

VOID VPAPI VideoPortFreeDeviceBase(
    PVOID HwDeviceExtension,
    PVOID MappedAddress
);

typedef struct _VP_DEVICE_DESCRIPTION {
    BOOLEAN ScatterGather;
    BOOLEAN Dma32BitAddresses;
    BOOLEAN Dma64BitAddresses;
    ULONG MaximumLength;
} VP_DEVICE_DESCRIPTION, *PVP_DEVICE_DESCRIPTION;

PVOID VPAPI VideoPortGetDmaAdapter(
    PVOID HwDeviceExtension,
    PVP_DEVICE_DESCRIPTION DeviceDescription
);

VOID VPAPI VideoPortPutDmaAdapter(
    PVOID HwDeviceExtension,
    PVOID DmaAdapter
);

PVOID VPAPI VideoPortAllocateCommonBuffer(
    PVOID HwDeviceExtension,
    PVOID DmaAdapter,
    ULONG DesiredLength,
    PPHYSICAL_ADDRESS LogicalAddress,
    BOOLEAN CacheEnabled,
    PVOID Reserved
);

VOID VPAPI VideoPortFreeCommonBuffer(
    PVOID HwDeviceExtension,
    ULONG Length,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS LogicalAddress,
    BOOLEAN CacheEnabled
);

VOID VPAPI VideoPortStallExecution(
    ULONG Microseconds
);

ULONG VPAPI VideoPortReadRegisterUlong(
    PULONG Register
);

VOID VPAPI VideoPortWriteRegisterUlong(
    PULONG Register,
    ULONG Value
);

UCHAR VPAPI VideoPortReadRegisterUchar(
    PUCHAR Register
);

VOID VPAPI VideoPortWriteRegisterUchar(
    PUCHAR Register,
    UCHAR Value
);

VP_STATUS VPAPI VideoPortMapMemory(
    PVOID HwDeviceExtension,
    PHYSICAL_ADDRESS PhysicalAddress,
    PULONG Length,
    PULONG InIoSpace,
    PVOID *VirtualAddress
);

VP_STATUS VPAPI VideoPortUnmapMemory(
    PVOID HwDeviceExtension,
    PVOID VirtualAddress,
    HANDLE ProcessHandle
);

ULONG VPAPI VideoPortGetBusData(
    PVOID HwDeviceExtension,
    BUS_DATA_TYPE BusDataType,
    ULONG SlotNumber,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length
);

ULONG VPAPI VideoPortSetBusData(
    PVOID HwDeviceExtension,
    BUS_DATA_TYPE BusDataType,
    ULONG SlotNumber,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length
);

VP_STATUS VPAPI VideoPortGetAccessRanges(
    PVOID HwDeviceExtension,
    ULONG NumRequestedResources,
    PVOID RequestedResources,
    ULONG NumAccessRanges,
    PVIDEO_ACCESS_RANGE AccessRanges,
    PVOID VendorId,
    PVOID DeviceId,
    PULONG Slot
);

VOID VideoPortDebugPrint(
    VIDEO_DEBUG_LEVEL DebugPrintLevel,
    PCSTR DebugMessage,
    ...
);

typedef struct _VIDEO_X86_BIOS_ARGUMENTS {
    ULONG Eax;
    ULONG Ebx;
    ULONG Ecx;
    ULONG Edx;
    ULONG Esi;
    ULONG Edi;
    ULONG Ebp;
} VIDEO_X86_BIOS_ARGUMENTS, *PVIDEO_X86_BIOS_ARGUMENTS;

VP_STATUS VPAPI VideoPortInt10(
    PVOID HwDeviceExtension,
    PVIDEO_X86_BIOS_ARGUMENTS BiosArguments
);

VP_STATUS VPAPI VideoPortCreateSecondaryDisplay(
    PVOID HwDeviceExtension,
    PVOID *SecondaryDeviceExtension,
    ULONG ulFlag
);
VP_STATUS VPAPI VideoPortEnumerateChildren(PVOID HwDeviceExtension, PVOID Reserved);
VOID NvxProbeFwsecRom(PVOID hwDeviceExtension, PUCHAR bar0);
