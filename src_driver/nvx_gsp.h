#ifndef _NVX_GSP_H_
#define _NVX_GSP_H_

#include "miniport.h"
#include "nvx_display.h"
#include "include/nvx_nvidia_open_shim.h"

#define MAX_NVX_MODES 32

/* GSP Mailbox & Falcon Register Offsets (derived from NVIDIA swref) */
#ifndef NV_PGSP_FALCON_MAILBOX0
#define NV_PGSP_FALCON_MAILBOX0        0x110040
#endif
#ifndef NV_PGSP_FALCON_MAILBOX1
#define NV_PGSP_FALCON_MAILBOX1        0x110044
#endif
#ifndef NV_PGSP_FALCON_OS
#define NV_PGSP_FALCON_OS              0x110080
#endif
#ifndef NV_PGSP_FALCON_DOORBELL
#define NV_PGSP_FALCON_DOORBELL        0x110004
#endif
#ifndef NV_PGSP_FALCON_CPUCTL
#define NV_PGSP_FALCON_CPUCTL          0x110100
#endif
#ifndef NV_PGSP_FALCON_BOOTVEC
#define NV_PGSP_FALCON_BOOTVEC         0x110104
#endif
#ifndef NV_PGSP_FALCON_DMATRFBASE
#define NV_PGSP_FALCON_DMATRFBASE      0x110110
#endif
#ifndef NV_PGSP_FALCON_DMATRFMOFF
#define NV_PGSP_FALCON_DMATRFMOFF      0x110114
#endif
#ifndef NV_PGSP_FALCON_DMATRFCMD
#define NV_PGSP_FALCON_DMATRFCMD       0x110118
#endif
#ifndef NV_PGSP_FALCON_DMATRFFBOFF
#define NV_PGSP_FALCON_DMATRFFBOFF     0x11011C
#endif
#ifndef NV_PGSP_FALCON_DMATRFBASE1
#define NV_PGSP_FALCON_DMATRFBASE1     0x110128
#endif
#ifndef NV_PGSP_FALCON_CPUCTL_ALIAS
#define NV_PGSP_FALCON_CPUCTL_ALIAS    0x110130
#endif
#ifndef NV_PGSP_FALCON_ENGINE
#define NV_PGSP_FALCON_ENGINE          0x1103C0
#endif
#ifndef NV_PGSP_FBIF_BASE
#define NV_PGSP_FBIF_BASE              0x110600
#endif
#ifndef NV_PGSP_MAILBOX_ARG0
#define NV_PGSP_MAILBOX_ARG0           0x110804
#endif
#ifndef NV_PGSP_MAILBOX_ARG1
#define NV_PGSP_MAILBOX_ARG1           0x110808
#endif
#ifndef NV_PGSP_QUEUE_HEAD0
#define NV_PGSP_QUEUE_HEAD0            0x110C00
#endif
#ifndef NV_PGSP_QUEUE_TAIL0
#define NV_PGSP_QUEUE_TAIL0            0x110C04
#endif
#ifndef NV_PGSP_QUEUE_HEAD1
#define NV_PGSP_QUEUE_HEAD1            0x110C08
#endif
#ifndef NV_PGSP_QUEUE_TAIL1
#define NV_PGSP_QUEUE_TAIL1            0x110C0C
#endif
#ifndef NV_PGSP_FALCON_RISCV_STAT
#define NV_PGSP_FALCON_RISCV_STAT      0x111388
#endif
#ifndef NV_PGSP_FALCON_BCR
#define NV_PGSP_FALCON_BCR             0x111668
#endif

/* SEC2 Security Falcon Register Offsets (Ampere GA102/GA104) */
#ifndef NV_PSEC_FALCON_MAILBOX0
#define NV_PSEC_FALCON_MAILBOX0        0x840040
#endif
#ifndef NV_PSEC_FALCON_MAILBOX1
#define NV_PSEC_FALCON_MAILBOX1        0x840044
#endif
#ifndef NV_PSEC_FALCON_CPUCTL
#define NV_PSEC_FALCON_CPUCTL          0x840100
#endif
#ifndef NV_PSEC_FALCON_BOOTVEC
#define NV_PSEC_FALCON_BOOTVEC         0x840104
#endif
#ifndef NV_PSEC_FALCON_DMATRFBASE
#define NV_PSEC_FALCON_DMATRFBASE      0x840110
#endif
#ifndef NV_PSEC_FALCON_DMATRFMOFF
#define NV_PSEC_FALCON_DMATRFMOFF      0x840114
#endif
#ifndef NV_PSEC_FALCON_DMATRFCMD
#define NV_PSEC_FALCON_DMATRFCMD       0x840118
#endif
#ifndef NV_PSEC_FALCON_DMATRFFBOFF
#define NV_PSEC_FALCON_DMATRFFBOFF     0x84011C
#endif
#ifndef NV_PSEC_FALCON_DMATRFBASE1
#define NV_PSEC_FALCON_DMATRFBASE1     0x840128
#endif
#ifndef NV_PSEC_FALCON_CPUCTL_ALIAS
#define NV_PSEC_FALCON_CPUCTL_ALIAS    0x840130
#endif
#ifndef NV_PSEC_FALCON_ENGINE
#define NV_PSEC_FALCON_ENGINE          0x8403C0
#endif
#define NV_PSEC_FBIF_BASE              0x840600
#define NV_PSEC_FALCON2_MOD_SEL        0x841180
#define NV_PSEC_FALCON2_UCODE_ID       0x841198
#define NV_PSEC_FALCON2_ENGIDMASK      0x84119C
#define NV_PSEC_FALCON2_PARAADDR       0x841210

#define GSP_FW_WPR_META_MAGIC          0xdc3aae21371a60b3ULL
#define GSP_FW_WPR_META_REVISION       1
#define GSP_FW_WPR_VERIFIED            0xa0a0a0a0a0a0a0a0ULL

/* GSP Staging Memory Layout Offsets in DMA buffer */
#define GSP_FW_IMAGE_OFFSET            0x00000000ULL
#define GSP_FW_IMAGE_SIZE              0x02448000ULL
#define GSP_RADIX3_LVL0_OFFSET         0x02450000ULL
#define GSP_RADIX3_LVL1_OFFSET         0x02451000ULL
#define GSP_RADIX3_LVL2_OFFSET         0x02452000ULL
#define GSP_SIG_OFFSET                 0x02470000ULL
#define GSP_SIG_SIZE                   0x00001000ULL
#define GSP_BOOTLOADER_OFFSET          0x02480000ULL
#define GSP_BOOTER_LOAD_OFFSET         0x02490000ULL
#define GSP_WPR_META_OFFSET            0x024B0000ULL
#define GSP_SHARED_OFFSET              0x02500000ULL
#define GSP_SHARED_SIZE                0x00081000UL
#define GSP_CMD_QUEUE_OFFSET           0x02501000ULL
#define GSP_STAT_QUEUE_OFFSET          0x02541000ULL
#define GSP_LIBOS_ARGS_OFFSET          0x024E0000ULL
#define GSP_RM_ARGS_OFFSET             0x024E1000ULL
#define GSP_SCRATCH_OFFSET             0x02F00000ULL
#define GSP_SCRATCH_SIZE               0x10000UL
#define GSP_PAGE_TABLES_OFFSET         0x02E00000ULL
#define GSP_PAGE_TABLES_SIZE           0x9000UL
#define GSP_GR_CONTEXT_BYTES           0x00C00000UL
#define GSP_GR_CONTEXT_GPU_VA          0x10200000ULL
#define GSP_GR_CONTEXT_TEST_GPU_VA     0x10E00000ULL
#define GSP_DISPLAY_PUSH_OFFSET        0x02E10000ULL
#define GSP_GR_TEST_PTES_OFFSET        0x02E13000ULL
#define GSP_GR_TEST_PTES_SIZE          0x6000UL
#define GSP_UPLOAD_OFFSET              0x02600000ULL
#define GSP_UPLOAD_SIZE                0x00800000UL
#define GSP_UPLOAD_GPU_VA              0x24000000ULL
#define GSP_UPLOAD_MAX_BYTES           0x0000F000UL

#pragma pack(push, 1)
/* Libos Init Argument */
typedef struct _LIBOS_MEMORY_REGION_INIT_ARGUMENT {
    unsigned long long Id8;
    unsigned long long Pa;
    unsigned long long Size;
    unsigned char      Kind;
    unsigned char      Loc;
    unsigned char      Pad[6];
} LIBOS_MEMORY_REGION_INIT_ARGUMENT, *PLIBOS_MEMORY_REGION_INIT_ARGUMENT;

/* Firmware 535.113.01 ABI; newer RM headers are incompatible. */
typedef struct _MESSAGE_QUEUE_INIT_ARGUMENTS {
    ULONG64 SharedMemPhysAddr;
    ULONG PageTableEntryCount;
    ULONG Pad;
    ULONG64 CmdQueueOffset;
    ULONG64 StatQueueOffset;
    ULONG64 LocklessCmdQueueOffset;
    ULONG64 LocklessStatQueueOffset;
} MESSAGE_QUEUE_INIT_ARGUMENTS;

typedef struct _GSP_ARGUMENTS_CACHED {
    MESSAGE_QUEUE_INIT_ARGUMENTS MessageQueueInitArguments;
    ULONG OldLevel;
    ULONG Flags;
    UCHAR InPmTransition;
    UCHAR Reserved[3];
    ULONG GpuInstance;
    ULONG64 ProfilerPa;
    ULONG64 ProfilerSize;
} GSP_ARGUMENTS_CACHED, *PGSP_ARGUMENTS_CACHED;

typedef struct _NVX_MSGQ_HEADER {
    ULONG Version, Size, MsgSize, MsgCount;
    ULONG WritePtr, Flags, RxHdrOff, EntryOff;
    ULONG ReadPtr;
} NVX_MSGQ_HEADER;

/* Use official upstream GspFwWprMeta from open-gpu-kernel-modules (aliased for existing call sites) */
typedef GspFwWprMeta GSP_FW_WPR_META_V1;
typedef GspFwWprMeta *PGSP_FW_WPR_META_V1;

/* VBE 2.0/3.0 Controller Information Block */
typedef struct _VBE_INFO_BLOCK {
    CHAR   VbeSignature[4];       /* "VESA" */
    USHORT VbeVersion;            /* 0x0200, 0x0300 */
    ULONG  OemStringPtr;          /* Real mode far pointer */
    ULONG  Capabilities;
    ULONG  VideoModePtr;          /* Real mode far pointer to mode list */
    USHORT TotalMemory;           /* 64KB blocks */
    USHORT OemSoftwareRev;
    ULONG  OemVendorNamePtr;
    ULONG  OemProductNamePtr;
    ULONG  OemProductRevPtr;
    UCHAR  Reserved[222];
    UCHAR  OemData[256];
} VBE_INFO_BLOCK, *PVBE_INFO_BLOCK;

/* VBE 2.0/3.0 Mode Information Block */
typedef struct _VBE_MODE_INFO {
    USHORT ModeAttributes;        /* bit 0: supported, bit 7: linear buffer */
    UCHAR  WinAAttributes;
    UCHAR  WinBAttributes;
    USHORT WinGranularity;
    USHORT WinSize;
    USHORT WinASegment;
    USHORT WinBSegment;
    ULONG  WinFuncPtr;
    USHORT BytesPerScanLine;      /* Exact hardware scanline stride */
    USHORT XResolution;           /* Width in pixels */
    USHORT YResolution;           /* Height in pixels */
    UCHAR  XCharSize;
    UCHAR  YCharSize;
    UCHAR  NumberOfPlanes;
    UCHAR  BitsPerPixel;          /* BPP (8, 16, 24, 32) */
    UCHAR  NumberOfBanks;
    UCHAR  MemoryModel;
    UCHAR  BankSize;
    UCHAR  NumberOfImagePages;
    UCHAR  Reserved1;
    UCHAR  RedMaskSize;
    UCHAR  RedFieldPosition;
    UCHAR  GreenMaskSize;
    UCHAR  GreenFieldPosition;
    UCHAR  BlueMaskSize;
    UCHAR  BlueFieldPosition;
    UCHAR  RsvdMaskSize;
    UCHAR  RsvdFieldPosition;
    UCHAR  DirectColorModeInfo;
    ULONG  PhysBasePtr;           /* 32-bit physical linear framebuffer address */
    ULONG  OffScreenMemOffset;
    USHORT OffScreenMemSize;
    USHORT LinBytesPerScanLine;
    UCHAR  BnkNumberOfImagePages, LinNumberOfImagePages;
    UCHAR  LinRedMaskSize, LinRedFieldPosition;
    UCHAR  LinGreenMaskSize, LinGreenFieldPosition;
    UCHAR  LinBlueMaskSize, LinBlueFieldPosition;
    UCHAR  LinRsvdMaskSize, LinRsvdFieldPosition;
    ULONG  MaxPixelClock;
    UCHAR  Reserved2[190];
} VBE_MODE_INFO, *PVBE_MODE_INFO;

/* NVIDIA Firmware Container Binary Header */
typedef struct _NVFW_BIN_HDR {
    ULONG  BinMagic;              /* 0x10DE */
    ULONG  BinVer;                /* Version */
    ULONG  BinSize;               /* Total size */
    ULONG  HeaderOffset;          /* Descriptor offset */
    ULONG  DataOffset;            /* Code/payload offset */
    ULONG  DataSize;              /* Payload size */
} NVFW_BIN_HDR, *PNVFW_BIN_HDR;

/* NVIDIA GSP RISC-V Ucode Descriptor Header */
typedef struct _RM_RISCV_UCODE_DESC {
    ULONG  Version;
    ULONG  BootloaderOffset;
    ULONG  BootloaderSize;
    ULONG  BootloaderParamOffset;
    ULONG  BootloaderParamSize;
    ULONG  RiscvElfOffset;
    ULONG  RiscvElfSize;
    ULONG  AppVersion;
    ULONG  ManifestOffset;
    ULONG  ManifestSize;
    ULONG  MonitorDataOffset;
    ULONG  MonitorDataSize;
    ULONG  MonitorCodeOffset;
    ULONG  MonitorCodeSize;
    ULONG  BIsMonitorEnabled;
    ULONG  SwbromCodeOffset, SwbromCodeSize;
    ULONG  SwbromDataOffset, SwbromDataSize;
    ULONG  FbReservedSize;
    ULONG  IsEntireRiscvImageSignedAsCode;
} RM_RISCV_UCODE_DESC, *PRM_RISCV_UCODE_DESC;
#pragma pack(pop)

typedef struct _VIDEO_MODE_DEF {
    ULONG Width;
    ULONG Height;
    ULONG BitsPerPel;
    ULONG Frequency;
    ULONG VbeMode32;
    ULONG VbeModeAlt;
} VIDEO_MODE_DEF;

#define NVX_3D_USER_BUFFER_SLOTS 64U
typedef struct _NVX_3D_USER_BUFFER_ENTRY {
    ULONG64 Id;
    ULONG64 GpuVa;
    ULONG64 DepthGpuVa;
    ULONG Bytes;
    ULONG Usage;
    ULONG Busy;
} NVX_3D_USER_BUFFER_ENTRY;
typedef NVX_3D_USER_BUFFER_ENTRY *PNVX_3D_USER_BUFFER_ENTRY;

typedef struct _NVX_DEVICE_EXTENSION {
    ULONG DeviceId;
    ULONG RevisionId;
    PHYSICAL_ADDRESS PhysicalBar0;
    ULONG Bar0Length;
    PVOID Bar0MmioVirtual;
    PHYSICAL_ADDRESS PhysicalBar1;
    ULONG Bar1Length;
    PVOID FramebufferVirtual;
    ULONG CurrentModeIndex;
    ULONG NumModes;
    VIDEO_MODE_INFORMATION Modes[MAX_NVX_MODES];
    ULONG ModeVbeIds[MAX_NVX_MODES];
    PHYSICAL_ADDRESS ModeFramebuffer[MAX_NVX_MODES];

    /* Display Resolution Tracking */
    ULONG ActiveWidth;
    ULONG ActiveHeight;
    ULONG ActiveBpp;
    ULONG ActiveFreq;

    /* GSP Subsystem State */
    BOOLEAN GspInitialized; /* Only set after RM INIT_DONE, not WPR verification. */
    BOOLEAN GspBootVerified;
    UCHAR Pad[6];
    PVOID DmaAdapter;
    PHYSICAL_ADDRESS GspDmaPhysical;
    PVOID GspDmaVirtual;
    PVOID GspGrContextVirtual;
    PHYSICAL_ADDRESS GspGrContextPhysical;
    ULONG GspGrContextLength;
    ULONG GspFaultBufferHandle;
    PVOID GspFaultQueueVirtual;
    PHYSICAL_ADDRESS GspFaultQueuePhysical;
    ULONG GspFaultQueueLength;
    PVOID GspFaultShadowVirtual;
    PHYSICAL_ADDRESS GspFaultShadowPhysical;
    ULONG GspFaultShadowLength;
    BOOLEAN GspFaultShadowRegistered;
    BOOLEAN GspGrContextAliasMapped;
    BOOLEAN GspGrContextAliasVerified;
    BOOLEAN GspGrPromotionAccepted;
    ULONG GspDmaLength;
    ULONG Reserved6;
    PHYSICAL_ADDRESS CmdQueuePhysical;
    PVOID CmdQueueVirtual;
    ULONG CmdQueueSize;
    ULONG Reserved7;
    PHYSICAL_ADDRESS StatQueuePhysical;
    PVOID StatQueueVirtual;
    ULONG StatQueueSize;
    ULONG Reserved8;
    ULONG RpcSeqNumber;

    /* FWSEC staging is kept alive on ambiguous DMA/Falcon timeouts. */
    PVOID FwsecDmaAdapter;
    PHYSICAL_ADDRESS FwsecDmaPhysical;
    PVOID FwsecDmaVirtual;
    ULONG FwsecDmaLength;
    BOOLEAN FwsecFrtsReady;
    ULONG64 VerifiedFrtsOffset;
    ULONG64 VerifiedWprEnd;
    ULONG64 VerifiedVgaWorkspaceOffset;
    PHYSICAL_ADDRESS PhysicalInstBar;
    ULONG PciBusNumber;
    ULONG PciSlot;
    ULONG GspAssertCount;
    BOOLEAN GspMmuFaultQueued;
    ULONG GspClientHandle;
    ULONG GspDeviceHandle,GspSubdeviceHandle;
    ULONG GspDisplayHandle;
    ULONG GspSupportedDisplays;
    ULONG GspConnectedDisplays;
    ULONG NativeDisplayHandle;
    ULONG NativeDisplayCaps;
    BOOLEAN NativeDisplayAttempted,NativeDisplayChannelsReady;
    ULONG64 NativeDisplayInstance;
    ULONG NativeDisplayChannels[3];
    ULONG NativeDisplayCorePut;
    BOOLEAN NativeDisplayNotifierReady;
    BOOLEAN NativeDisplayFaulted;
    BOOLEAN NativeDualActive;
    ULONG MonitorSor[8],MonitorProtocol[8];
    BOOLEAN IsSecondaryDisplay;
    PVOID PrimaryDeviceExtension;
    PVOID SecondaryDeviceExtension;
    ULONG SecondaryDpDisplayId;
    BOOLEAN SecondaryDpTrainReady,SecondaryDpEnhancedFraming,SecondaryDpTrainingAttempted;
    BOOLEAN DualViewAttached;
    ULONG DisplayWindowMask;
    ULONG DisplayHeadCount;
    ULONG DisplayHeadMask;
    ULONG DisplayActiveIds[4];
    ULONG DisplayRoutableMask;
    ULONG DisplayHeadRoutingMap;
    NVX_DISPLAY_TIMING MonitorTiming[8];
    ULONG MonitorCount;
    ULONG MonitorIds[8];
    ULONG MonitorEdidBytes[8];
    UCHAR MonitorEdid[8][2048];
    ULONG GspVaSpaceHandle;
    ULONG GspScratchMemoryHandle;
    ULONG GspVirtualMemoryHandle;
    ULONG64 GspScratchGpuVa;
    ULONG64 GspVramGpuVa,GspVramMapBytes;
    ULONG GspChannelGroupHandle;
    ULONG GspContextShareHandle;
    ULONG GspChannelHandle;
    ULONG GspCopyEngineHandle;
    ULONG GspCopyEngineType;
    ULONG GspGraphicsTsgHandle;
    ULONG GspGraphicsContextShareHandle;
    ULONG GspGraphicsChannelHandle;
    ULONG GspGraphicsObjectHandle;
    BOOLEAN GspCopyVerified;
    BOOLEAN GspChannelFaulted;
    BOOLEAN GspGraphicsChannelReady;
    BOOLEAN GspGraphicsCommandVerified;
    BOOLEAN GspGraphicsDataWriteVerified;
    BOOLEAN GspGraphicsPixelClearVerified;
    BOOLEAN GspGraphicsTriangleVerified;
    BOOLEAN GspGraphicsClientTriangleVerified;
    BOOLEAN GspGraphicsChannelFaulted;
    BOOLEAN GspGraphicsUserAllocReady;
    BOOLEAN GspGraphicsUserSubmitReady;
    ULONG64 Nvx3dNextBufferId;
    ULONG64 Nvx3dLastFence;
    ULONG64 Nvx3dCompletedFence;
    NVX_3D_USER_BUFFER_ENTRY Nvx3dBuffers[NVX_3D_USER_BUFFER_SLOTS];
    BOOLEAN GspFbLayoutValid;
    ULONG GspFbRegionCount;
    ULONG64 GspFbFreeBase,GspFbFreeLimit;
    ULONG64 GspBar1PageDirectory,GspBar2PageDirectory;
    BOOLEAN GspScanoutMapped;
    ULONG64 GspScanoutPhysical,GspScanoutBytes;
    /* Reusable CE submission queue state */
    ULONG GspGPPut;
    ULONG GspNextFence;
    ULONG GspRunlistBase;
    ULONG GspSubmitToken;
    ULONG GspGraphicsRunlistBase;
    ULONG GspGraphicsSubmitToken;
    ULONG GspGraphicsGPPut;
    ULONG RpcTxSequence,RpcRxSequence;
    NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS GrContextInfoParams;
    UCHAR RpcTxRecord[32768];
    UCHAR RpcControlPayload[32768];
    UCHAR RpcRxRecord[65536];
} NVX_DEVICE_EXTENSION, *PNVX_DEVICE_EXTENSION;

/* GSP Firmware Blobs */
#ifdef __VXDKERNEL__
extern const unsigned char *g_pNvxFirmwareStart;
extern const unsigned char *g_pNvxFirmwareEnd;
#define _binary_dist_full_gsp_ga104_bin_start g_pNvxFirmwareStart
#define _binary_dist_full_gsp_ga104_bin_end   g_pNvxFirmwareEnd
#else
extern const unsigned char _binary_dist_full_gsp_ga104_bin_start[];
extern const unsigned char _binary_dist_full_gsp_ga104_bin_end[];
#endif

/* Logging utilities */
VOID LogStr(const char *s);
VOID LogHex(unsigned long long val);

/* GSP Engine entrypoints */
BOOLEAN NvxGspInitSubsystem(PVOID HwDeviceExtension);
BOOLEAN NvxGspInitDisplayChannels(PVOID HwDeviceExtension);
BOOLEAN NvxGspReleaseDisplayChannels(PVOID HwDeviceExtension);
BOOLEAN NvxNativeSetSecondaryScanout(PVOID HwDeviceExtension);
VOID NvxGspWakeSecondaryDpSink(PVOID HwDeviceExtension);
VOID NvxGspTrainSecondaryDpLink(PVOID HwDeviceExtension);
BOOLEAN NvxGspDiscoverScanout(PVOID HwDeviceExtension,ULONG modeIndex);
BOOLEAN NvxGspSubmitCopy(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG64 srcVa,ULONG bytes);
BOOLEAN NvxGspSubmitCopy2D(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG64 srcVa,
                           ULONG srcPitch,ULONG dstPitch,ULONG bytesPerLine,ULONG lineCount);
BOOLEAN NvxGspSetDepthPageKind(PVOID HwDeviceExtension,ULONG64 gpuVa,
                               ULONG kind,BOOLEAN map);
BOOLEAN NvxGspSubmitFill(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG color,ULONG bytes);
BOOLEAN NvxGspSubmitUserClear(PVOID HwDeviceExtension,ULONG64 dstVa,
                              ULONG color,ULONG64 fenceValue,
                              ULONG *matchingPixels);
BOOLEAN NvxGspSubmitUserTriangle(PVOID HwDeviceExtension,ULONG64 dstVa,
                                 ULONG64 fenceValue,ULONG *greenPixels,
                                 ULONG *whitePixels);
BOOLEAN NvxGspSubmitUserTriangleBuffer(PVOID HwDeviceExtension,ULONG64 dstVa,
                                 ULONG64 depthVa,ULONG64 vertexVa,ULONG color,
                                 BOOLEAN preserveTarget,
                                 BOOLEAN depthTest,
                                 ULONG depthFormat,
                                 ULONG64 fenceValue,ULONG *drawPixels,
                                 ULONG *whitePixels);
BOOLEAN NvxGspSubmitUserDepthClear(PVOID HwDeviceExtension,ULONG64 depthVa,
                                   BOOLEAN zf32,ULONG64 fenceValue);
BOOLEAN NvxGspRunZf32Diagnostic(PVOID HwDeviceExtension,ULONG64 dstVa,
                                ULONG64 depthVa,ULONG64 vertexVa,
                                ULONG64 fenceValue);
BOOLEAN NvxGspReadUserSurface(PVOID HwDeviceExtension,ULONG64 srcVa,
                              PULONG output,ULONG outputPixels);
BOOLEAN NvxGspWaitUserFence(PVOID HwDeviceExtension,ULONG64 fenceValue,
                            ULONG timeoutMs);
BOOLEAN NvxGspUpload(PVOID HwDeviceExtension,ULONG64 dstVa,const void *src,ULONG bytes);

VOID NvxGspShutdown(PVOID HwDeviceExtension);
BOOLEAN NvxGspUnregisterFaultShadow(PNVX_DEVICE_EXTENSION dev);
int NvxGspSequencerCore(void *context,unsigned int opcode);

#endif /* _NVX_GSP_H_ */
