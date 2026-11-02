#ifndef _NVX_GSP_H_
#define _NVX_GSP_H_

#include "miniport.h"

#define MAX_NVX_MODES 32

/* GSP Mailbox & Falcon Register Offsets */
#define NV_PGSP_FALCON_MAILBOX0        0x110040
#define NV_PGSP_FALCON_MAILBOX1        0x110044
#define NV_PGSP_FALCON_OS              0x110080
#define NV_PGSP_FALCON_DOORBELL        0x110004
#define NV_PGSP_FALCON_CPUCTL          0x110100
#define NV_PGSP_FALCON_BOOTVEC         0x110104
#define NV_PGSP_FALCON_DMATRFBASE      0x110110
#define NV_PGSP_FALCON_DMATRFMOFF      0x110114
#define NV_PGSP_FALCON_DMATRFCMD       0x110118
#define NV_PGSP_FALCON_DMATRFFBOFF     0x11011C
#define NV_PGSP_FALCON_DMATRFBASE1     0x110128
#define NV_PGSP_FALCON_CPUCTL_ALIAS    0x110130
#define NV_PGSP_FALCON_ENGINE          0x1103C0
#define NV_PGSP_FBIF_BASE              0x110600
#define NV_PGSP_MAILBOX_ARG0           0x110804
#define NV_PGSP_MAILBOX_ARG1           0x110808
#define NV_PGSP_QUEUE_HEAD0            0x110C00
#define NV_PGSP_QUEUE_TAIL0            0x110C04
#define NV_PGSP_QUEUE_HEAD1            0x110C08
#define NV_PGSP_QUEUE_TAIL1            0x110C0C
#define NV_PGSP_FALCON_RISCV_STAT      0x111388
#define NV_PGSP_FALCON_BCR             0x111668

/* SEC2 Security Falcon Register Offsets (Ampere GA102/GA104) */
#define NV_PSEC_FALCON_MAILBOX0        0x840040
#define NV_PSEC_FALCON_MAILBOX1        0x840044
#define NV_PSEC_FALCON_CPUCTL          0x840100
#define NV_PSEC_FALCON_BOOTVEC         0x840104
#define NV_PSEC_FALCON_DMATRFBASE      0x840110
#define NV_PSEC_FALCON_DMATRFMOFF      0x840114
#define NV_PSEC_FALCON_DMATRFCMD       0x840118
#define NV_PSEC_FALCON_DMATRFFBOFF     0x84011C
#define NV_PSEC_FALCON_DMATRFBASE1     0x840128
#define NV_PSEC_FALCON_CPUCTL_ALIAS    0x840130
#define NV_PSEC_FALCON_ENGINE          0x8403C0
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

/* GSP firmware WPR metadata V1 (Used by Booter to verify and unlock WPR) */
typedef struct _GSP_FW_WPR_META_V1 {
    unsigned long long Magic;
    unsigned long long Revision;
    unsigned long long SysmemAddrOfRadix3Elf;
    unsigned long long SizeOfRadix3Elf;
    unsigned long long SysmemAddrOfBootloader;
    unsigned long long SizeOfBootloader;
    unsigned long long BootloaderCodeOffset;
    unsigned long long BootloaderDataOffset;
    unsigned long long BootloaderManifestOffset;
    unsigned long long SysmemAddrOfSignature;
    unsigned long long SizeOfSignature;
    unsigned long long GspFwRsvdStart;
    unsigned long long NonWprHeapOffset;
    unsigned long long NonWprHeapSize;
    unsigned long long GspFwWprStart;
    unsigned long long GspFwHeapOffset;
    unsigned long long GspFwHeapSize;
    unsigned long long GspFwOffset;
    unsigned long long BootBinOffset;
    unsigned long long FrtsOffset;
    unsigned long long FrtsSize;
    unsigned long long GspFwWprEnd;
    unsigned long long FbSize;
    unsigned long long VgaWorkspaceOffset;
    unsigned long long VgaWorkspaceSize;
    unsigned long long BootCount;
    unsigned char      Reserved1[16];
    unsigned long long SysmemAddrOfCrashReportQueue;
    unsigned int       SizeOfCrashReportQueue;
    unsigned int       Reserved2;
    unsigned char      GspFwHeapVfPartitionCount;
    unsigned char      Flags;
    unsigned short     PagingConfig;
    unsigned int       Reserved3;
    unsigned long long Verified;
} GSP_FW_WPR_META_V1, *PGSP_FW_WPR_META_V1;
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
    ULONG GspClientHandle;
    ULONG GspDeviceHandle,GspSubdeviceHandle;
    ULONG GspVaSpaceHandle;
    ULONG GspScratchMemoryHandle;
    ULONG RpcTxSequence,RpcRxSequence;
    UCHAR RpcRxRecord[65536];
} NVX_DEVICE_EXTENSION, *PNVX_DEVICE_EXTENSION;

/* GSP Firmware Blobs */
extern const unsigned char _binary_dist_full_gsp_ga104_bin_start[];
extern const unsigned char _binary_dist_full_gsp_ga104_bin_end[];

/* Logging utilities */
VOID LogStr(const char *s);
VOID LogHex(unsigned long long val);

/* GSP Engine entrypoints */
BOOLEAN NvxGspInitSubsystem(PVOID HwDeviceExtension);

VOID NvxGspShutdown(PVOID HwDeviceExtension);
int NvxGspSequencerCore(void *context,unsigned int opcode);

#endif /* _NVX_GSP_H_ */
