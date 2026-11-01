/* Host-only VideoPort shim for running GSP state tests without Windows/GPU.
 * The production ABI is separately checked by the MinGW build. */
#ifndef NVX_HOST_VIDEOPORT
#define NVX_HOST_VIDEOPORT
#include <stdint.h>
#include <stddef.h>
typedef void VOID, *PVOID;
typedef uint8_t UCHAR, BOOLEAN, *PUCHAR;
typedef char CHAR;
typedef uint16_t USHORT, *PUSHORT;
typedef int32_t VP_STATUS;
#define NO_ERROR 0
typedef uint32_t ULONG, *PULONG;
typedef struct { ULONG Eax,Ebx,Ecx,Edx,Esi,Edi,Ebp; } VIDEO_X86_BIOS_ARGUMENTS;
VP_STATUS VideoPortInt10(PVOID, VIDEO_X86_BIOS_ARGUMENTS *);
typedef uint64_t ULONG64, *PULONG64;
typedef struct { int64_t QuadPart; } PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;
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


typedef struct {
    BOOLEAN ScatterGather, Dma32BitAddresses, Dma64BitAddresses;
    ULONG MaximumLength;
} VP_DEVICE_DESCRIPTION, *PVP_DEVICE_DESCRIPTION;
typedef enum { Error, Warn, Trace, Info } VIDEO_DEBUG_LEVEL;
#define TRUE 1
#define FALSE 0
VOID VideoPortDebugPrint(VIDEO_DEBUG_LEVEL, const CHAR *, ...);
VOID VideoPortZeroMemory(PVOID, ULONG);
VOID VideoPortMoveMemory(PVOID, PVOID, ULONG);
VOID VideoPortStallExecution(ULONG);
PVOID VideoPortGetDmaAdapter(PVOID, PVP_DEVICE_DESCRIPTION);
VOID VideoPortPutDmaAdapter(PVOID, PVOID);
PVOID VideoPortAllocateCommonBuffer(PVOID,PVOID,ULONG,PPHYSICAL_ADDRESS,BOOLEAN,PVOID);
VOID VideoPortFreeCommonBuffer(PVOID,ULONG,PVOID,PHYSICAL_ADDRESS,BOOLEAN);
ULONG VideoPortReadRegisterUlong(PULONG);
VOID VideoPortWriteRegisterUlong(PULONG,ULONG);
#endif
