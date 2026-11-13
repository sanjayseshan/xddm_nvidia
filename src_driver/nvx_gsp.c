/*
 * NVIDIA Ampere GA104 GSP Coprocessor Subsystem
 * Target: Windows XP Professional x64 Edition / Windows Server 2003 x64
 *
 * Implements Falcon IMEM DMA transfer, bootloader staging, queue initialization,
 * checked RM objects, and an isolated GPU copy/completion probe. Native
 * display channels and XDDM acceleration integration remain separate work.
 */

#include "nvx_gsp.h"
#include "nvx_fw.h"
#include "nvx_rpc.h"
#include "nvx_seq.h"
#include "nvx_mmu.h"
#include "nvx_gr_context.h"
#include "nvx_openrm_shim.h"
#include "nvx_3d_abi.h"
#include <class/cl0002.h>
#include <class/cl0090.h>
#include <class/clc369.h>
#include <class/clc097tex.h>
#include <class/clb069.h>
#include <ctrl/ctrlb069.h>
#include <ctrl/ctrl2080/ctrl2080internal.h>
#include <ctrl/ctrl2080/ctrl2080nvd.h>
#include <ctrl/ctrl2080/ctrl2080dma.h>
#include <stddef.h>
#include "bootloader_blob.h"
#include "booter_load_blob.h"

_Static_assert(GSP_VRAM_MAP_BYTES ==
               GSP_VRAM_SLICE_COUNT * GSP_VRAM_SLICE_BYTES,
               "VRAM PTE slices must cover the published aperture");
_Static_assert(GSP_VRAM_LPT_OFFSET + GSP_VRAM_SLICE_COUNT * 0x100UL ==
               GSP_UPLOAD_PTE_OFFSET,
               "upload SPT must follow the VRAM LPT pool");
_Static_assert(GSP_UPLOAD_PTE_OFFSET + 0x1000UL <= GSP_PAGE_TABLES_SIZE,
               "page-table allocation must contain the upload SPT");
_Static_assert(GSP_PAGE_TABLES_OFFSET + GSP_PAGE_TABLES_SIZE <=
               GSP_DISPLAY_PUSH_OFFSET,
               "page-table pool must not overlap display push buffers");

#ifndef NVX_ENABLE_GR_CHANNEL_PROBE
#define NVX_ENABLE_GR_CHANNEL_PROBE 0
#endif
#ifndef NVX_ENABLE_GR_SUBMISSION_PROBE
#define NVX_ENABLE_GR_SUBMISSION_PROBE 0
#endif
#ifndef NVX_ENABLE_GR_PROMOTE_PROBE
#define NVX_ENABLE_GR_PROMOTE_PROBE 0
#endif
#ifndef NVX_ENABLE_GR_UNPRIVILEGED_CONTEXT_PROBE
#define NVX_ENABLE_GR_UNPRIVILEGED_CONTEXT_PROBE 0
#endif

#define NVX_RM_CLASS_KEPLER_I2M_B        0x0000a140U
#define NVX_RM_CLASS_AMPERE_DMA_COPY_B   0x0000c7b5U
#define NVX_RM_CLASS_FERMI_2D_A          0x0000902dU
#define NVX_RM_OBJECT_NVC0_M2MF          0xc6970010U
#define NVX_RM_OBJECT_NVC0_COPY          0xc6970011U
#define NVX_RM_OBJECT_NVC0_2D            0xc6970012U
#define NVX_RM_OBJECT_NVC0_COMPUTE       0xc6970013U
#define NVX_RM_CLASS_AMPERE_COMPUTE_B    0x0000c7c0U

/* Convert a pixel coordinate in an Ampere/Turing 2D GOB ARGB target to its
   byte offset. TileMode carries the vertical GOB block height in bits 6:4.
   The intra-GOB sector ordering differs from the legacy Fermi helper below. */
static ULONG NvxGspBlockLinearPixelOffsetEx(ULONG x,ULONG y,
                                             ULONG widthPixels,
                                             ULONG tileMode)
{
    return Nvx3dTuringColorPixelOffset(x,y,widthPixels,tileMode);
}

static ULONG NvxGspBlockLinearPixelOffset(ULONG x,ULONG y,ULONG widthPixels)
{
    return NvxGspBlockLinearPixelOffsetEx(x,y,widthPixels,0);
}

static ULONG NvxGspReadBlockLinearPixel(const volatile UCHAR *bytes,
                                         ULONG x,ULONG y,ULONG widthPixels)
{
    return *(const volatile ULONG *)(bytes+
        NvxGspBlockLinearPixelOffset(x,y,widthPixels));
}

/* NVK's zeta surfaces use a one-GOB vertical block (y_log2 == 0), matching
   the bounded ZF32 diagnostic's SET_ZT_BLOCK_SIZE state. */
static ULONG NvxGspReadDepthBlockLinearPixel(const volatile UCHAR *bytes,
                                              ULONG x,ULONG y,
                                              ULONG widthPixels)
{
    ULONG lx=x&15U,ly=y&7U;
    ULONG k=(lx&1U)|((lx&2U))|((ly&1U)<<2)|((lx&4U)<<1)|
        ((ly&2U)<<3)|((ly&4U)<<3)|((lx&8U)<<3);
    ULONG gobsPerRow=(widthPixels+15U)/16U;
    ULONG gob=(y/8U)*gobsPerRow+(x/16U);
    return *(const volatile ULONG *)(bytes+gob*512U+k*4U);
}

#if NVX_ENABLE_GR_SUBMISSION_PROBE
/* The first two programs are NVIDIA's MIT-licensed Ampere head-surface
 * vertex and basic fragment shaders (offsets 0x000 and 0x300). */
__asm__(".section .rdata,\"dr\"\n"
        ".balign 256\n"
        ".globl NvxAmpereHeadSurfacePrograms\n"
        "NvxAmpereHeadSurfacePrograms:\n"
        ".incbin \"../open-gpu-kernel-modules/src/nvidia-modeset/src/shaders/g_ampere_shaders\", 0, 1408\n"
        ".text\n");
extern const UCHAR NvxAmpereHeadSurfacePrograms[1408];

static VOID NvxGspBuildProbeTexture(PULONG sampler, PULONG header, ULONG64 gpuVa)
{
    VideoPortZeroMemory(sampler,8*sizeof(ULONG));
    VideoPortZeroMemory(header,8*sizeof(ULONG));
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_COMPONENTS,_SIZES_A8B8G8R8,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_R_DATA_TYPE,_NUM_UNORM,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_G_DATA_TYPE,_NUM_UNORM,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_B_DATA_TYPE,_NUM_UNORM,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_A_DATA_TYPE,_NUM_UNORM,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_X_SOURCE,_IN_R,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_Y_SOURCE,_IN_G,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_Z_SOURCE,_IN_B,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_W_SOURCE,_IN_A,header);
    FLD_SET_DRF_NUM_MW(C097,_TEXHEAD_BL,_ADDRESS_BITS31TO9,(ULONG)(gpuVa>>9),header);
    FLD_SET_DRF_NUM_MW(C097,_TEXHEAD_BL,_ADDRESS_BITS48TO32,(ULONG)(gpuVa>>32),header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_HEADER_VERSION,_SELECT_BLOCKLINEAR,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_GOBS_PER_BLOCK_WIDTH,_ONE_GOB,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_GOBS_PER_BLOCK_HEIGHT,_ONE_GOB,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_GOBS_PER_BLOCK_DEPTH,_ONE_GOB,header);
    FLD_SET_DRF_NUM_MW(C097,_TEXHEAD_BL,_WIDTH_MINUS_ONE,0,header);
    FLD_SET_DRF_NUM_MW(C097,_TEXHEAD_BL,_HEIGHT_MINUS_ONE,0,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_TEXTURE_TYPE,_TWO_D_NO_MIPMAP,header);
    FLD_SET_DRF_DEF_MW(C097,_TEXHEAD_BL,_BORDER_SIZE,_BORDER_SAMPLER_COLOR,header);
    FLD_SET_DRF_NUM_MW(C097,_TEXHEAD_BL,_NORMALIZED_COORDS,0,header);
    sampler[0]=DRF_DEF(C097,_TEXSAMP0,_ADDRESS_U,_CLAMP_TO_EDGE)|
                DRF_DEF(C097,_TEXSAMP0,_ADDRESS_V,_CLAMP_TO_EDGE)|
                DRF_DEF(C097,_TEXSAMP0,_ADDRESS_P,_CLAMP_TO_EDGE);
    sampler[1]=DRF_DEF(C097,_TEXSAMP1,_MAG_FILTER,_MAG_POINT)|
                DRF_DEF(C097,_TEXSAMP1,_MIN_FILTER,_MIN_POINT)|
                DRF_DEF(C097,_TEXSAMP1,_MIP_FILTER,_MIP_NONE);
}

#endif

/* Convert one normalized 8-bit channel to its IEEE-754 single-precision bit
 * pattern without relying on floating-point runtime helpers in the miniport. */
static ULONG NvxUnorm8ToFloatBits(ULONG channel)
{
    int exponent=-1;
    ULONG64 significand;
    channel&=0xffU;
    if(!channel) return 0;
    if(channel==255U) return 0x3f800000U;
    while((((ULONG64)channel)<<(-exponent))<255U) exponent--;
    significand=(((ULONG64)channel)<<(23-exponent));
    significand=(significand+127U)/255U;
    return ((ULONG)(exponent+127)<<23)|
           ((ULONG)significand-0x00800000U);
}

static inline ULONG NvxGspFloatBits(float value)
{
    union { float f; ULONG u; } bits;
    bits.f=value;
    return bits.u;
}

/* ABI checks deliberately target firmware 535, not the current OpenRM ABI. */
_Static_assert(sizeof(GspFwWprMeta) == 256, "WPR ABI");
_Static_assert(offsetof(GspFwWprMeta, verified) == 248, "WPR verified ABI");
_Static_assert(sizeof(GSP_ARGUMENTS_CACHED) == 80, "535 RM args ABI");
_Static_assert(sizeof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS)==24032, "fault registration ABI");
_Static_assert(offsetof(NVX_DEVICE_EXTENSION,RpcRxRecord)%8==0, "RPC scratch alignment");
#if NVX_ENABLE_GR_CHANNEL_PROBE
static VOID NvxGspDumpFaultShadow(PNVX_DEVICE_EXTENSION dev);
#endif
_Static_assert(sizeof(LIBOS_MEMORY_REGION_INIT_ARGUMENT) == 32, "LibOS ABI");
_Static_assert(offsetof(RM_RISCV_UCODE_DESC, MonitorCodeOffset) == 48, "BL ABI");
_Static_assert(sizeof(VP_DEVICE_DESCRIPTION) == 8, "VideoPort DMA ABI");
_Static_assert(offsetof(VP_DEVICE_DESCRIPTION, Dma64BitAddresses) == 2, "DMA flags ABI");
typedef struct {
    ULONG index,flags;
    ULONG64 size,startInternal,limitInternal;
    ULONG bigPageSize;
    ULONG64 base;
} NVX_VASPACE_535;
_Static_assert(sizeof(NVX_VASPACE_535)==48,"535 VASpace ABI");
_Static_assert(offsetof(NVX_VASPACE_535,base)==40,"535 VASpace base");
typedef struct {
    ULONG owner,type,flags,width,height,pitch,attr,attr2,format,comprCovg,zcullCovg;
    ULONG64 rangeLo,rangeHi,size,alignment,offset,limit,address;
    ULONG ctagOffset,hVASpace,internalflags,tag;
} NVX_MEMORY_535;
_Static_assert(sizeof(NVX_MEMORY_535)==120,"535 memory alloc ABI");
_Static_assert(offsetof(NVX_MEMORY_535,size)==64,"535 memory size offset");
typedef struct {
    ULONG64 base,limit,reserved;
    ULONG performance;
    UCHAR compressed,iso,protectedMemory,blackList[17];
} NVX_FB_REGION_535;
_Static_assert(sizeof(NVX_FB_REGION_535)==48,"535 FB region ABI");
_Static_assert(offsetof(NVX_FB_REGION_535,protectedMemory)==30,"535 FB region flags");

static VOID NvxGspLogPbdmaFault(PNVX_DEVICE_EXTENSION dev)
{
    ULONG i;
    if(!dev||!dev->Bar0MmioVirtual||dev->Bar0Length<4) return;
    for(i=0;i<NVX_GR_RUNLIST_PBDMA_CONFIG_COUNT;i++) {
        ULONG configOffset=dev->GspGraphicsRunlistBase+
                           NVX_GR_RUNLIST_PBDMA_CONFIG(i);
        ULONG config,pbdmaId,pbdmaBase;
        ULONG intrOffset,intr1Offset,methodOffset,dataOffset,targetOffset;
        ULONG gpBaseOffset,gpBaseHiOffset,gpGetOffset,gpPutOffset,gpFetchOffset;
        ULONG statusOffset,topGetOffset,topGetHiOffset,pbFetchOffset,pbFetchHiOffset;
        ULONG gpCrcOffset,pbCrcOffset;
        ULONG pbGetOffset,pbGetHiOffset,pbPutOffset,pbPutHiOffset;
        ULONG pbHeaderOffset,pbCountOffset;
        ULONG intr,intr1,method,data,target,status,topGet,topGetHi;
        ULONG gpBase,gpBaseHi,gpGet,gpPut,gpFetch,pbFetch,pbFetchHi,gpCrc,pbCrc;
        ULONG pbGet,pbGetHi,pbPut,pbPutHi,pbHeader,pbCount;
        if(configOffset>dev->Bar0Length-4) break;
        config=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                                                   configOffset));
        if(!(config&NVX_GR_RUNLIST_PBDMA_CONFIG_VALID)) continue;
        pbdmaId=config&0xffU;
        pbdmaBase=((config>>10)&0xffffU)<<10;
        intrOffset=pbdmaBase+NVX_PPBDMA_INTR_0_REL;
        intr1Offset=pbdmaBase+NVX_PPBDMA_INTR_1_REL;
        methodOffset=pbdmaBase+NVX_PPBDMA_METHOD0_REL;
        dataOffset=pbdmaBase+NVX_PPBDMA_DATA0_REL;
        targetOffset=pbdmaBase+NVX_PPBDMA_TARGET_REL;
        statusOffset=pbdmaBase+NVX_PPBDMA_STATUS_REL;
        topGetOffset=pbdmaBase+NVX_PPBDMA_TOP_LEVEL_GET_REL;
        topGetHiOffset=pbdmaBase+NVX_PPBDMA_TOP_LEVEL_GET_HI_REL;
        gpBaseOffset=pbdmaBase+NVX_PPBDMA_GP_BASE_REL;
        gpBaseHiOffset=pbdmaBase+NVX_PPBDMA_GP_BASE_HI_REL;
        gpGetOffset=pbdmaBase+NVX_PPBDMA_GP_GET_REL;
        gpPutOffset=pbdmaBase+NVX_PPBDMA_GP_PUT_REL;
        gpFetchOffset=pbdmaBase+NVX_PPBDMA_GP_FETCH_REL;
        pbFetchOffset=pbdmaBase+NVX_PPBDMA_PB_FETCH_REL;
        pbFetchHiOffset=pbdmaBase+NVX_PPBDMA_PB_FETCH_HI_REL;
        gpCrcOffset=pbdmaBase+NVX_PPBDMA_GP_CRC_REL;
        pbGetOffset=pbdmaBase+NVX_PPBDMA_PB_GET_REL;
        pbGetHiOffset=pbdmaBase+NVX_PPBDMA_PB_GET_HI_REL;
        pbPutOffset=pbdmaBase+NVX_PPBDMA_PB_PUT_REL;
        pbPutHiOffset=pbdmaBase+NVX_PPBDMA_PB_PUT_HI_REL;
        pbHeaderOffset=pbdmaBase+NVX_PPBDMA_PB_HEADER_REL;
        pbCountOffset=pbdmaBase+NVX_PPBDMA_PB_COUNT_REL;
        pbCrcOffset=pbdmaBase+NVX_PPBDMA_PB_CRC_REL;
        if(intrOffset>dev->Bar0Length-4||intr1Offset>dev->Bar0Length-4||
           statusOffset>dev->Bar0Length-4||topGetOffset>dev->Bar0Length-4||
           topGetHiOffset>dev->Bar0Length-4||
           methodOffset>dev->Bar0Length-4||dataOffset>dev->Bar0Length-4||
           targetOffset>dev->Bar0Length-4||gpBaseOffset>dev->Bar0Length-4||
           gpBaseHiOffset>dev->Bar0Length-4||gpGetOffset>dev->Bar0Length-4||
           gpPutOffset>dev->Bar0Length-4||gpFetchOffset>dev->Bar0Length-4||
           pbFetchOffset>dev->Bar0Length-4||pbFetchHiOffset>dev->Bar0Length-4||
           gpCrcOffset>dev->Bar0Length-4||pbCrcOffset>dev->Bar0Length-4||
           pbGetOffset>dev->Bar0Length-4||pbGetHiOffset>dev->Bar0Length-4||
           pbPutOffset>dev->Bar0Length-4||pbPutHiOffset>dev->Bar0Length-4||
           pbHeaderOffset>dev->Bar0Length-4||pbCountOffset>dev->Bar0Length-4) break;
        intr=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+intrOffset));
        if((intr&0xffff0000U)==0xbadf0000U) {
            LogStr("NVX [PBDMA]: BAR0 register window is unreadable at index=");
            LogHex(i);LogStr(" offset=");LogHex(intrOffset);LogStr(" value=");LogHex(intr);LogStr("\n");
            break;
        }
        intr1=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+intr1Offset));
        status=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+statusOffset));
        topGet=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+topGetOffset));
        topGetHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+topGetHiOffset));
        method=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+methodOffset));
        data=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+dataOffset));
        target=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+targetOffset));
        gpBase=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpBaseOffset));
        gpBaseHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpBaseHiOffset));
        gpGet=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpGetOffset));
        gpPut=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpPutOffset));
        gpFetch=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpFetchOffset));
        pbFetch=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbFetchOffset));
        pbFetchHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbFetchHiOffset));
        gpCrc=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpCrcOffset));
        pbGet=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbGetOffset));
        pbGetHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbGetHiOffset));
        pbPut=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbPutOffset));
        pbPutHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbPutHiOffset));
        pbHeader=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbHeaderOffset));
        pbCount=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbCountOffset));
        pbCrc=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+pbCrcOffset));
        LogStr("NVX [PBDMA]: runlistIndex=");LogHex(i);
        LogStr(" id=");LogHex(pbdmaId);LogStr(" base=");LogHex(pbdmaBase);
        LogStr(" config=");LogHex(config);LogStr(" INTR0=");LogHex(intr);
        LogStr(" INTR1=");LogHex(intr1);LogStr(" STATUS=");LogHex(status);
        LogStr(" TARGET=");LogHex(target);
        LogStr(" METHOD0=");LogHex(method);LogStr(" DATA0=");LogHex(data);
        LogStr(" GP_BASE=");LogHex(gpBase);LogStr(" GP_BASE_HI=");LogHex(gpBaseHi);
        LogStr(" GP_GET=");LogHex(gpGet);LogStr(" GP_PUT=");LogHex(gpPut);LogStr("\n");
        LogStr("NVX [PBDMA-PB]: id=");LogHex(pbdmaId);
        LogStr(" TOP_GET=");LogHex(topGet);LogStr(" TOP_GET_HI=");LogHex(topGetHi);
        LogStr(" GP_FETCH=");LogHex(gpFetch);LogStr(" GP_CRC=");LogHex(gpCrc);
        LogStr(" PB_FETCH=");LogHex(pbFetch);LogStr(" PB_FETCH_HI=");LogHex(pbFetchHi);
        LogStr(" PB_GET=");LogHex(pbGet);
        LogStr(" PB_GET_HI=");LogHex(pbGetHi);LogStr(" PB_PUT=");LogHex(pbPut);
        LogStr(" PB_PUT_HI=");LogHex(pbPutHi);LogStr(" PB_HEADER=");LogHex(pbHeader);
        LogStr(" PB_COUNT=");LogHex(pbCount);LogStr(" PB_CRC=");LogHex(pbCrc);LogStr("\n");
    }
}

/* One synchronous post-init RPC. Unexpected events remain unconsumed. */
static BOOLEAN NvxGspCall(PNVX_DEVICE_EXTENSION dev,ULONG expected,
                         const void *payload,ULONG bytes)
{
    volatile NVX_MSGQ_HEADER *cmd=dev->CmdQueueVirtual,*stat=dev->StatQueueVirtual;
    ULONG write=cmd->WritePtr,next,available;
    uint32_t elements=1;
    if(!dev->GspInitialized||write>=63||stat->ReadPtr>=63) return FALSE;
    if(bytes<=4016) {
        next=(write+1)%63;
        if(next==stat->ReadPtr||!NvxEncodeRpc((PUCHAR)dev->CmdQueueVirtual+4096+write*4096,
            4096,expected,dev->RpcTxSequence,payload,bytes)) return FALSE;
    } else {
        ULONG index;
        available=(stat->ReadPtr+63-write-1)%63;
        if(!NvxEncodeRpcRecord(dev->RpcTxRecord,sizeof(dev->RpcTxRecord),expected,
            dev->RpcTxSequence,payload,bytes,&elements)) return FALSE;
        if(elements>available||elements>sizeof(dev->RpcTxRecord)/4096) return FALSE;
        for(index=0;index<elements;index++) {
            VideoPortMoveMemory((PUCHAR)dev->CmdQueueVirtual+4096+((write+index)%63)*4096,
                                dev->RpcTxRecord+index*4096,4096);
        }
        next=(write+elements)%63;
    }
    __sync_synchronize();cmd->WritePtr=next;__sync_synchronize();
    dev->RpcTxSequence++;
    nvx_msgq_notify_remote(FCN_FLAG_NOTIFY_MSG_WRITE, dev);
    LogStr("NVX [RM]: RPC submitted function=");LogHex(expected);LogStr("\n");
    for(ULONG wait=0;wait<20000;wait++) {
        ULONG read=cmd->ReadPtr,head=stat->WritePtr;
        uint32_t elements=0,decoded=0,function=0,result=0,sequence=0;
        int gathered;
        if(head>=63||read>=63) return FALSE;
        __sync_synchronize();
        gathered=NvxGatherRpc((PUCHAR)dev->StatQueueVirtual+4096,63*4096,
            63,read,head,dev->RpcRxRecord,sizeof(dev->RpcRxRecord),&elements);
        if(gathered<0) return FALSE;
        if(!gathered) {VideoPortStallExecution(100);continue;}
        if(!NvxDecodeRpcRecord(dev->RpcRxRecord,elements*4096,&function,&result,&sequence,&decoded)||
            decoded!=elements||sequence!=dev->RpcRxSequence) return FALSE;
        LogStr("NVX [RM]: reply function=");LogHex(function);
        LogStr(" result=");LogHex(result);LogStr("\n");
        if(!result && (function==0x101b||function==0x100c||function==0x1004||function==0x1005||function==0x1006||function==0x100f)) {
            const ULONG *words=(const ULONG *)dev->RpcRxRecord;
            if(function==0x1006) {
                char message[257];
                if(words[14]<300) return FALSE;
                VideoPortMoveMemory(message,(PVOID)(words+23),256);message[256]=0;
                LogStr("NVX [RM ERROR]: type=");LogHex(words[20]);
                LogStr(" runlist=");LogHex(words[21]);LogStr(" channel=");LogHex(words[22]);
                LogStr(" message=");LogStr(message);LogStr("\n");
                if(words[20]==0x38) dev->NativeDisplayFaulted=TRUE;
            } else if(function==0x101b) {
                if(words[14]<36) return FALSE;
                dev->GspAssertCount=words[20];
                LogStr("NVX [RM]: firmware assertion count=");LogHex(words[20]);LogStr("\n");
            } else if(function==0x1004) {
                if(words[14]<52) return FALSE;
                LogStr("NVX [RM]: channel RC fault engine=");LogHex(words[20]);
                LogStr(" chid=");LogHex(words[21]);LogStr(" type=");LogHex(words[22]);
                LogStr(" scope=");LogHex(words[23]);LogStr("\n");
                if(words[22]==ROBUST_CHANNEL_PBDMA_ERROR) NvxGspLogPbdmaFault(dev);
                if(words[21]==1) dev->GspChannelFaulted=TRUE;
                if(words[21]==2) dev->GspGraphicsChannelFaulted=TRUE;
            } else if(function==0x1005) {
                /* NVIDIA uses this notification to schedule the UVM MMU
                   fault handler. XP has no such handler: record the fault,
                   stop both accelerated queues, and drain the notification
                   so RM teardown and fault queries can still complete. */
                if(words[14]<32) return FALSE;
#if NVX_ENABLE_GR_CHANNEL_PROBE
                NvxGspDumpFaultShadow(dev);
#endif
                dev->GspMmuFaultQueued=TRUE;
                dev->GspChannelFaulted=TRUE;
                dev->GspGraphicsChannelFaulted=TRUE;
                dev->GspGrContextAliasVerified=FALSE;
                LogStr("NVX [RM]: MMU fault queued; UVM fault handling unavailable, accelerated queues disabled.\n");
            } else if(function==0x100f) {
                /* PERF_BRIDGELESS_INFO_UPDATE: informational, no reply or
                   power-management action required (also ignored by Nouveau).
                   Consume it before waiting for the real RM response. */
                LogStr("NVX [RM]: consumed asynchronous performance notification.\n");
            } else {
                if(words[14]<40||words[21]>words[14]-40) return FALSE;
                LogStr("NVX [RM]: LibOS notification bytes=");LogHex(words[21]);LogStr("\n");
            }
            __sync_synchronize();cmd->ReadPtr=(read+elements)%63;__sync_synchronize();
            dev->RpcRxSequence++;
            continue;
        }
        if(function!=expected) return FALSE;
        __sync_synchronize();cmd->ReadPtr=(read+elements)%63;__sync_synchronize();
        dev->RpcRxSequence++;
        return result==0;
    }
    LogStr("NVX [RM]: RPC timed out; staging retained.\n");return FALSE;
}

/* RM 535 GSP_RM_ALLOC: fixed 32-byte header followed by class parameters. */
static BOOLEAN NvxGspAlloc(PNVX_DEVICE_EXTENSION dev,ULONG parent,ULONG object,
                          ULONG hclass,const void *params,ULONG bytes)
{
    ULONG payload[128]={0};
    const ULONG *words=(const ULONG *)dev->RpcRxRecord;
    if(bytes>400||(bytes&&!params)) return FALSE;
    payload[0]=0xc1d00001;payload[1]=parent;payload[2]=object;payload[3]=hclass;payload[5]=bytes;
    if(bytes) VideoPortMoveMemory(payload+8,(PVOID)params,bytes);
    if(!NvxGspCall(dev,103,payload,32+bytes)) return FALSE;
    if(words[14]<64||words[20]!=payload[0]||words[21]!=parent||words[22]!=object||words[23]!=hclass) return FALSE;
    LogStr("NVX [RM]: allocation class=");LogHex(hclass);
    LogStr(" status=");LogHex(words[24]);LogStr("\n");
    return words[24]==0;
}

static BOOLEAN NvxGspControl(PNVX_DEVICE_EXTENSION dev,ULONG object,ULONG cmd,
                            const void *params,ULONG bytes)
{
    ULONG stackPayload[522]={0};
    ULONG *payload=stackPayload;
    const ULONG *words=(const ULONG *)dev->RpcRxRecord;
    if(bytes>sizeof(dev->RpcControlPayload)-24) return FALSE;
    if(bytes>2064) {
        payload=(ULONG *)dev->RpcControlPayload;
        VideoPortZeroMemory(payload,24+bytes);
    }
    payload[0]=dev->GspClientHandle;
    payload[1]=object;
    payload[2]=cmd;
    payload[3]=0; /* status */
    payload[4]=bytes;
    payload[5]=0; /* flags */
    if(bytes&&params) VideoPortMoveMemory(payload+6,(PVOID)params,bytes);
    if(!NvxGspCall(dev,76,payload,24+bytes)) return FALSE;
    if(words[14]<56+bytes||words[20]!=payload[0]||words[21]!=object||
       words[22]!=cmd||words[23]!=0||words[24]!=bytes) {
        LogStr("NVX [RM]: control rejected cmd=");LogHex(cmd);
        LogStr(" reply length=");LogHex(words[14]);
        if(words[14]>=56) {
            LogStr(" status=");LogHex(words[23]);LogStr(" returned bytes=");LogHex(words[24]);
        }
        LogStr(" expected bytes=");LogHex(bytes);LogStr("\n");return FALSE;
    }
    return TRUE;
}

static BOOLEAN __attribute__((unused)) NvxGspGetClassEngineId(PNVX_DEVICE_EXTENSION dev,
    ULONG channel,ULONG object,ULONG *classEngineId)
{
    NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS params={0};
    const NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS *reply;
    if(!dev||!channel||!object||!classEngineId) return FALSE;
    params.hObject=object;
    if(!NvxGspControl(dev,channel,NVA06F_CTRL_GET_CLASS_ENGINEID,
                      &params,sizeof(params))) return FALSE;
    reply=(const NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS *)(dev->RpcRxRecord+104);
    if(!reply->classEngineID) return FALSE;
    *classEngineId=reply->classEngineID;
    return TRUE;
}

/* Change the allocated depth page's existing 64KB leaf PTE to its zeta kind,
   so GR and CE use the same VA and the same mapping for that VRAM page. */
BOOLEAN NvxGspSetDepthPageKind(PVOID HwDeviceExtension,ULONG64 gpuVa,
                               ULONG kind,BOOLEAN map)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    NV2080_CTRL_DMA_INVALIDATE_TLB_PARAMS invalidate;
    ULONG64 offset,value;
    ULONG page,slice;
    PULONG64 lpt;
    PULONG64 tables;
    if(!dev||!dev->GspDmaVirtual||!dev->GspVaSpaceHandle||
       gpuVa<dev->GspVramGpuVa||(gpuVa&0xffffULL)||
       (map&&kind!=3U&&kind!=5U&&kind!=6U)||(!map&&kind!=0U))
        return FALSE;
    offset=gpuVa-dev->GspVramGpuVa;
    if(offset>dev->GspVramMapBytes||0x10000ULL>dev->GspVramMapBytes-offset)
        return FALSE;
    tables=(PULONG64)((PUCHAR)dev->GspDmaVirtual+GSP_PAGE_TABLES_OFFSET);
    slice=(ULONG)(offset/GSP_VRAM_SLICE_BYTES);
    page=(ULONG)((offset%GSP_VRAM_SLICE_BYTES)/0x10000ULL);
    if(slice>=GSP_VRAM_SLICE_COUNT||page>=32U) return FALSE;
    lpt=(PULONG64)((PUCHAR)tables+GSP_VRAM_LPT_OFFSET+slice*0x100);
    /* This is a VER2 video-aperture PTE. Keep the peer-id field (35:33)
       clear; ATOMIC_DISABLE is bit 7. The old bit-35 value encoded peer ID 4
       while leaving atomics enabled, which does not match UVM's PTE builder. */
    value=(offset>>4)|1ULL|
          (map?((1ULL<<7)|((ULONG64)kind<<56)):0ULL);
    lpt[page]=value;
    __sync_synchronize();
    VideoPortZeroMemory(&invalidate,sizeof(invalidate));
    invalidate.hVASpace=dev->GspVaSpaceHandle;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,
                      NV2080_CTRL_CMD_DMA_INVALIDATE_TLB,
                      &invalidate,sizeof(invalidate))) return FALSE;
    LogStr("NVX [MMU]: depth page ");LogHex(gpuVa);
    LogStr(map?" mapped kind ":" restored kind ");LogHex(map?kind:0U);
    LogStr(" PTE=");LogHex(lpt[page]);
    LogStr("\n");
    return TRUE;
}

BOOLEAN NvxGspSetDepthRangeKind(PVOID HwDeviceExtension,ULONG64 gpuVa,
                                ULONG bytes,ULONG kind,BOOLEAN map)
{
    ULONG offset,done=0;
    if(!bytes||(gpuVa&0xffffULL)||(bytes&0xffffU)) return FALSE;
    for(offset=0;offset<bytes;offset+=0x10000U) {
        if(!NvxGspSetDepthPageKind(HwDeviceExtension,gpuVa+offset,kind,map)) {
            if(map) {
                while(done) {
                    done-=0x10000U;
                    (void)NvxGspSetDepthPageKind(HwDeviceExtension,
                        gpuVa+done,0U,FALSE);
                }
            }
            return FALSE;
        }
        done+=0x10000U;
    }
    return TRUE;
}

static BOOLEAN NvxGspInvalidateVaSpace(PNVX_DEVICE_EXTENSION dev)
{
    NV2080_CTRL_DMA_INVALIDATE_TLB_PARAMS invalidate;
    if(!dev||!dev->GspVaSpaceHandle) return FALSE;
    VideoPortZeroMemory(&invalidate,sizeof(invalidate));
    invalidate.hVASpace=dev->GspVaSpaceHandle;
    return NvxGspControl(dev,dev->GspSubdeviceHandle,
                         NV2080_CTRL_CMD_DMA_INVALIDATE_TLB,
                         &invalidate,sizeof(invalidate));
}

static volatile ULONG g_NvxGartPteBusy;

static BOOLEAN NvxGspLockGartPtes(VOID)
{
    ULONG wait;
    for(wait=0;wait<100000U;wait++) {
        if(!__sync_lock_test_and_set(&g_NvxGartPteBusy,1U)) return TRUE;
        VideoPortStallExecution(1U);
    }
    return FALSE;
}

static VOID NvxGspUnlockGartPtes(VOID)
{
    __sync_lock_release(&g_NvxGartPteBusy);
}

BOOLEAN NvxGspMapGartRange(PVOID HwDeviceExtension,ULONG firstPage,
                           ULONG pageCount,ULONG64 physical,ULONG bytes)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PULONG64 entries;
    ULONG capacity=NVX_GART_SLICE_COUNT*NVX_GART_PTES_PER_SLICE;
    ULONG clearBytes;
    if(!dev||!dev->GspGartReady||!dev->GspGartPteVirtual||
       !pageCount||firstPage>=NVX_GART_PAGE_COUNT||
       pageCount>NVX_GART_PAGE_COUNT-firstPage||
       !bytes||(bytes&0xfffU)||(physical&0xfffULL)||
       physical>0x100000000ULL-bytes||
       (ULONG64)bytes>(ULONG64)pageCount*NVX_GART_PAGE_BYTES)
        return FALSE;
    if(!NvxGspLockGartPtes()) {
        LogStr("NVX [GART]: page-table lock timeout mapping page=");
        LogHex(firstPage);LogStr(" count=");LogHex(pageCount);
        LogStr(" bytes=");LogHex(bytes);LogStr("\n");
        return FALSE;
    }
    entries=(PULONG64)dev->GspGartPteVirtual;
    clearBytes=pageCount*sizeof(ULONG64);
    VideoPortZeroMemory(entries+firstPage,clearBytes);
    if(!NvxGartBuildPtes((uint64_t *)entries,capacity,firstPage,pageCount,
                         physical,bytes,0)) {
        LogStr("NVX [GART]: PTE build rejected page=");
        LogHex(firstPage);LogStr(" count=");LogHex(pageCount);
        LogStr(" physical=");LogHex(physical);LogStr(" bytes=");
        LogHex(bytes);LogStr("\n");
        NvxGspUnlockGartPtes();
        return FALSE;
    }
    LogStr("NVX [GART]: mapped page=");LogHex(firstPage);
    LogStr(" count=");LogHex(pageCount);
    LogStr(" gpuVa=");LogHex(NVX_GART_GPU_VA_BASE+
                             (ULONG64)firstPage*NVX_GART_PAGE_BYTES);
    LogStr(" physical=");LogHex(physical);
    LogStr(" bytes=");LogHex(bytes);
    LogStr(" pteTable=");LogHex(dev->GspGartPtePhysical.QuadPart);
    LogStr(" pte0=");LogHex(entries[firstPage]);
    LogStr(" pte1=");LogHex(pageCount>1?entries[firstPage+1U]:0);
    LogStr(" pte2=");LogHex(pageCount>2?entries[firstPage+2U]:0);
    LogStr(" pte3=");LogHex(pageCount>3?entries[firstPage+3U]:0);
    LogStr(" pte4=");LogHex(pageCount>4?entries[firstPage+4U]:0);
    LogStr(" pte5=");LogHex(pageCount>5?entries[firstPage+5U]:0);
    LogStr(" pte6=");LogHex(pageCount>6?entries[firstPage+6U]:0);
    LogStr(" pte7=");LogHex(pageCount>7?entries[firstPage+7U]:0);
    LogStr(" pteLast=");LogHex(entries[firstPage+(bytes>>12)-1U]);
    LogStr("\n");
    __sync_synchronize();
    /* On an unconfirmed invalidate, the caller must retain the physical
     * backing and VA reservation; stale translations may still reference it. */
    {
        BOOLEAN ok=NvxGspInvalidateVaSpace(dev);
        if(!ok) {
            LogStr("NVX [GART]: TLB invalidate failed mapping page=");
            LogHex(firstPage);LogStr(" count=");LogHex(pageCount);
            LogStr("\n");
        }
        NvxGspUnlockGartPtes();
        return ok;
    }
}

BOOLEAN NvxGspUnmapGartRange(PVOID HwDeviceExtension,ULONG firstPage,
                             ULONG pageCount)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PULONG64 entries;
    ULONG clearBytes;
    if(!dev||!dev->GspGartReady||!dev->GspGartPteVirtual||!pageCount||
       firstPage>=NVX_GART_PAGE_COUNT||
       pageCount>NVX_GART_PAGE_COUNT-firstPage)
        return FALSE;
    if(!NvxGspLockGartPtes()) {
        LogStr("NVX [GART]: page-table lock timeout unmapping page=");
        LogHex(firstPage);LogStr(" count=");LogHex(pageCount);LogStr("\n");
        return FALSE;
    }
    entries=(PULONG64)dev->GspGartPteVirtual;
    clearBytes=pageCount*sizeof(ULONG64);
    VideoPortZeroMemory(entries+firstPage,clearBytes);
    __sync_synchronize();
    {
        BOOLEAN ok=NvxGspInvalidateVaSpace(dev);
        NvxGspUnlockGartPtes();
        return ok;
    }
}

#if NVX_ENABLE_GR_CHANNEL_PROBE
static VOID NvxGspDumpNocatJournal(PNVX_DEVICE_EXTENSION dev)
{
    NV2080_CTRL_NVD_GET_NOCAT_JOURNAL_PARAMS *params;
    const NV2080_CTRL_NVD_GET_NOCAT_JOURNAL_PARAMS *reply;
    ULONG count,i;
    if(sizeof(*params)>sizeof(dev->RpcControlPayload)-24) {
        LogStr("NVX [GR]: NOCAT journal query exceeds the RPC payload buffer.\n");
        return;
    }
    /* The control helper copies parameters before reusing RpcTxRecord to
       construct the framed request, so use that scratch area as our input. */
    params=(NV2080_CTRL_NVD_GET_NOCAT_JOURNAL_PARAMS *)dev->RpcTxRecord;
    VideoPortZeroMemory(params,sizeof(*params));
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,
                      NV2080_CTRL_CMD_NVD_GET_NOCAT_JOURNAL,params,sizeof(*params))) {
        LogStr("NVX [GR]: OpenRM NOCAT journal query unavailable.\n");
        return;
    }
    reply=(const NV2080_CTRL_NVD_GET_NOCAT_JOURNAL_PARAMS *)(dev->RpcRxRecord+104);
    count=reply->nocatRecordCount;
    if(count>NV2080_NOCAT_JOURNAL_MAX_JOURNAL_RECORDS)
        count=NV2080_NOCAT_JOURNAL_MAX_JOURNAL_RECORDS;
    LogStr("NVX [GR]: NOCAT records=");LogHex(reply->nocatRecordCount);
    LogStr(" outstanding=");LogHex(reply->nocatOutstandingRecordCount);LogStr("\n");
    for(i=0;i<count;i++) {
        const NV2080_NOCAT_JOURNAL_ENTRY *entry=&reply->journalRecords[i].nocatJournalEntry;
        char source[NV2080_NOCAT_JOURNAL_MAX_STR_LEN+1];
        char engine[NV2080_NOCAT_JOURNAL_MAX_STR_LEN+1];
        ULONG j;
        VideoPortMoveMemory(source,(PVOID)entry->source,NV2080_NOCAT_JOURNAL_MAX_STR_LEN);
        VideoPortMoveMemory(engine,(PVOID)entry->faultingEngine,NV2080_NOCAT_JOURNAL_MAX_STR_LEN);
        source[NV2080_NOCAT_JOURNAL_MAX_STR_LEN]=0;
        engine[NV2080_NOCAT_JOURNAL_MAX_STR_LEN]=0;
        LogStr("NVX [GR]: NOCAT record=");LogHex(i);LogStr(" type=");
        LogHex(entry->recType);LogStr(" subsystem=");LogHex(entry->subsystem);
        LogStr(" error64=");LogHex(entry->errorCode);
        LogStr(" loadBase=");LogHex(reply->journalRecords[i].loadAddress);
        LogStr(" stateMask=");LogHex(reply->journalRecords[i].stateMask);
        LogStr(" engine=");LogStr(engine);
        LogStr(" source=");LogStr(source);LogStr(" diagBytes=");
        LogHex(entry->diagBufferLen);LogStr("\n");
        for(j=0;j<entry->diagBufferLen/4&&j<16;j++) {
            ULONG word=0;
            VideoPortMoveMemory(&word,(PVOID)(entry->diagBuffer+j*4),sizeof(word));
            LogStr("NVX [GR]: NOCAT diagDword[");LogHex(j);LogStr("]=");
            LogHex(word);LogStr("\n");
        }
    }
}
#endif

static BOOLEAN NvxGspGetName(PNVX_DEVICE_EXTENSION dev)
{
    /* GET_NAME_STRING has a 132-byte fixed parameter struct, no pointers. */
    ULONG payload[39]={0};
    const ULONG *words=(const ULONG *)dev->RpcRxRecord;
    char name[65];
    payload[0]=dev->GspClientHandle;payload[1]=dev->GspSubdeviceHandle;
    payload[2]=0x20800110;payload[4]=132;
    if(!NvxGspCall(dev,76,payload,sizeof(payload))) return FALSE;
    if(words[14]<32+sizeof(payload)||words[20]!=payload[0]||words[21]!=payload[1]||
       words[22]!=payload[2]||words[24]!=132||words[23]) return FALSE;
    for(ULONG i=0;i<64;i++) {
        unsigned char c=dev->RpcRxRecord[108+i];
        name[i]=(c>=32&&c<=126)?c:0;
    }
    name[64]=0;
    LogStr("NVX [RM]: GPU name: ");LogStr(name);LogStr("\n");return TRUE;
}

static BOOLEAN NvxGspRegisterScratch(PNVX_DEVICE_EXTENSION dev)
{
    /* rpc_alloc_memory_v13_01 + IDR=0 PTE descriptor, 4KB page numbers.
       GSP does not implement the local VideoMemory allocator (class 0x40).
       Register OS-owned coherent memory, as OpenRM's ALLOC_MEMORY does. */
    ULONG payload[46]={0};
    ULONG64 physical=dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET;
    if(dev->GspDmaLength<GSP_SCRATCH_OFFSET+GSP_SCRATCH_SIZE||(physical&4095)) return FALSE;
    payload[0]=dev->GspClientHandle;payload[1]=dev->GspDeviceHandle;
    payload[2]=0x81000001;payload[3]=0x81;
    payload[4]=0x40000010; /* noncontiguous PCI pages, NO_MAP */
    payload[8]=GSP_SCRATCH_SIZE;payload[10]=16;payload[12]=16U<<16;
    for(ULONG i=0;i<16;i++) {
        ULONG64 page=(physical>>12)+i;
        payload[14+i*2]=(ULONG)page;payload[15+i*2]=(ULONG)(page>>32);
    }
    if(!NvxGspCall(dev,4,payload,sizeof(payload))) return FALSE;
    dev->GspScratchMemoryHandle=0x81000001;
    LogStr("NVX [RM]: 64KB coherent scratch memory registered.\n");return TRUE;
}

#if NVX_ENABLE_GR_CHANNEL_PROBE
static BOOLEAN NvxGspAllocateGrBacking(PNVX_DEVICE_EXTENSION dev)
{
    ULONG64 physical;
    if(!dev||!dev->DmaAdapter||dev->GspGrContextVirtual) return FALSE;
    dev->GspGrContextLength=GSP_GR_CONTEXT_BYTES;
    dev->GspGrContextVirtual=VideoPortAllocateCommonBuffer(dev,dev->DmaAdapter,
        dev->GspGrContextLength,&dev->GspGrContextPhysical,FALSE,NULL);
    if(!dev->GspGrContextVirtual) {
        dev->GspGrContextLength=0;
        LogStr("NVX [GR]: 12MB context backing allocation failed.\n");
        return FALSE;
    }
    physical=(ULONG64)dev->GspGrContextPhysical.QuadPart;
    if((physical&4095)||physical>0x100000000ULL-GSP_GR_CONTEXT_BYTES) {
        VideoPortFreeCommonBuffer(dev,dev->GspGrContextLength,dev->GspGrContextVirtual,
                                  dev->GspGrContextPhysical,FALSE);
        dev->GspGrContextVirtual=NULL;dev->GspGrContextLength=0;
        LogStr("NVX [GR]: context backing outside aligned 32-bit DMA range.\n");
        return FALSE;
    }
    VideoPortZeroMemory(dev->GspGrContextVirtual,dev->GspGrContextLength);
    __sync_synchronize();
    LogStr("NVX [GR]: reserved zeroed context backing physical=");LogHex(physical);
    LogStr(" bytes=");LogHex(dev->GspGrContextLength);LogStr("\n");
    return TRUE;
}
#endif

static BOOLEAN NvxGspMapScratch(PNVX_DEVICE_EXTENSION dev)
{
    /* GSP rejects MAP_MEMORY_DMA (v75: INVALID_FUNCTION). Use externally
       owned Pascal+ page tables and DMA_SET_PAGE_DIRECTORY, as Nouveau r535.
       The small-page tree maps scratch at 0x10000000; an LPT pool maps the
       firmware-reported free VRAM window at 0x20000000. */
    ULONG setDirectory[8]={0};
    ULONG64 physical=dev->GspDmaPhysical.QuadPart+GSP_PAGE_TABLES_OFFSET;
    PULONG64 tables=(PULONG64)((PUCHAR)dev->GspDmaVirtual+GSP_PAGE_TABLES_OFFSET);
    if(dev->GspDmaLength<GSP_PAGE_TABLES_OFFSET+GSP_PAGE_TABLES_SIZE||
       dev->GspDmaLength<GSP_UPLOAD_OFFSET+GSP_UPLOAD_SIZE||
       (physical&4095)) return FALSE;
    if(!dev->GspGartPteVirtual) {
        dev->GspGartPteLength=NVX_GART_SLICE_COUNT*4096U;
        dev->GspGartPteVirtual=VideoPortAllocateCommonBuffer(dev,
            dev->DmaAdapter,dev->GspGartPteLength,
            &dev->GspGartPtePhysical,FALSE,NULL);
        if(!dev->GspGartPteVirtual||
           (dev->GspGartPtePhysical.QuadPart&0xfff)||
           (ULONG64)dev->GspGartPtePhysical.QuadPart>
               0x100000000ULL-dev->GspGartPteLength) {
            LogStr("NVX [GART]: page-table backing allocation failed or exceeds 32-bit DMA.\n");
            return FALSE;
        }
        VideoPortZeroMemory(dev->GspGartPteVirtual,dev->GspGartPteLength);
    }
    VideoPortZeroMemory(tables,GSP_PAGE_TABLES_SIZE);
    /* PDEs: coherent-system aperture, volatile, next-table address >> 4. */
    tables[0]=((physical+0x1000)>>4)|0xc;
    tables[512]=((physical+0x2000)>>4)|0xc;
    tables[1024]=((physical+0x3000)>>4)|0xc;
    /* PD0 has paired large/small entries; VA>>21 == 128, small is +8. */
    tables[1536+128*2+1]=((physical+0x4000)>>4)|0xc;
    /* Reserve PD0 entries 141..255 for GART at 0x11a00000..0x1fffffff.
       The page-table pool is common system memory and is installed before
       SET_PAGE_DIRECTORY; individual BO PTEs are populated on allocation. */
    for(ULONG slice=0;slice<NVX_GART_SLICE_COUNT;slice++) {
        ULONG pdIndex=(ULONG)(NVX_GART_GPU_VA_BASE>>21)+slice;
        ULONG64 ptePhysical=(ULONG64)dev->GspGartPtePhysical.QuadPart+
                            (ULONG64)slice*4096U;
        if(pdIndex>255U) return FALSE;
        tables[1536+pdIndex*2+1]=(ptePhysical>>4)|0xc;
    }
    for(ULONG i=0;i<16;i++)
        tables[2048+i]=((dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET+i*4096)>>4)|0xd;
#if NVX_ENABLE_GR_CHANNEL_PROBE
    if(dev->GspGrContextVirtual&&dev->GspGrContextLength==GSP_GR_CONTEXT_BYTES&&
       dev->GspDmaLength>=GSP_GR_TEST_PTES_OFFSET+GSP_GR_TEST_PTES_SIZE) {
        ULONG slice;
        ULONG64 aliasPhys=(ULONG64)dev->GspDmaPhysical.QuadPart+GSP_GR_TEST_PTES_OFFSET;
        PUCHAR aliasTables=(PUCHAR)dev->GspDmaVirtual+GSP_GR_TEST_PTES_OFFSET;
        _Static_assert(GSP_GR_TEST_PTES_OFFSET>=GSP_DISPLAY_PUSH_OFFSET+0x3000,
                       "GR alias PTEs must follow display pushbuffers");
        _Static_assert(GSP_GR_TEST_PTES_OFFSET+GSP_GR_TEST_PTES_SIZE<=GSP_SCRATCH_OFFSET,
                       "GR alias PTEs must precede scratch");
        VideoPortZeroMemory(aliasTables,GSP_GR_TEST_PTES_SIZE);
        /* Six 4KB SPTs cover 12MB at 0x10200000; leaves default privileged.
           A separate unprivileged alias is solely for the CE mapping test.
           Build before installing the directory so no TLB update is needed. */
        for(slice=0;slice<6;slice++) {
            PULONG64 spt=(PULONG64)((PUCHAR)tables+0x9000+slice*0x1000);
            PULONG64 aliasSpt=(PULONG64)(aliasTables+slice*0x1000);
            if(!NvxGrBuildPtes((uint64_t *)spt,512,
                               (ULONG64)dev->GspGrContextPhysical.QuadPart+
                                   (ULONG64)slice*0x200000,0x200000,
                                   NVX_ENABLE_GR_UNPRIVILEGED_CONTEXT_PROBE?0:1)||
               !NvxGrBuildPtes((uint64_t *)aliasSpt,512,
                               (ULONG64)dev->GspGrContextPhysical.QuadPart+
                                   (ULONG64)slice*0x200000,0x200000,0)) {
                break;
            }
            tables[1536+(129+slice)*2+1]=((physical+0x9000+slice*0x1000)>>4)|0xc;
            tables[1536+(135+slice)*2+1]=((aliasPhys+slice*0x1000)>>4)|0xc;
        }
        if(slice==6) {
            dev->GspGrContextAliasMapped=TRUE;
            LogStr("NVX [GR]: context backing mapped at GPU VA=");
            LogHex(GSP_GR_CONTEXT_GPU_VA);LogStr(" test alias=");
            LogHex(GSP_GR_CONTEXT_TEST_GPU_VA);
            LogStr(" privileged=");LogHex(NVX_ENABLE_GR_UNPRIVILEGED_CONTEXT_PROBE?0:1);
            LogStr("; no GR promotion submitted.\n");
        } else {
            ULONG i;
            for(i=0;i<12;i++) tables[1536+(129+i)*2+1]=0;
            VideoPortFreeCommonBuffer(dev,dev->GspGrContextLength,dev->GspGrContextVirtual,
                                      dev->GspGrContextPhysical,FALSE);
            dev->GspGrContextVirtual=NULL;dev->GspGrContextLength=0;
            LogStr("NVX [GR]: context PTE construction failed; display path retained.\n");
        }
    }
#endif
    /* VA 0x20000000 selects level-2 index 1, so it needs a second PD0 page.
       Its paired PD0 entries point to 64KB LPTs and a 4KB SPT. Each 256-byte
       LPT covers one 2MB VA slice. Map 128MiB of VRAM for Nouveau screen BOs. */
    tables[1025]=((physical+0x5000)>>4)|0xc;
    for(ULONG slice=0;slice<GSP_VRAM_SLICE_COUNT;slice++) {
        ULONG64 *lpt=(ULONG64 *)((PUCHAR)tables+
                                 GSP_VRAM_LPT_OFFSET+slice*0x100);
        ULONG pdIndex=slice;
        tables[2560+pdIndex*2]=
            ((physical+GSP_VRAM_LPT_OFFSET+slice*0x100)>>4)|0xc;
        tables[2560+pdIndex*2+1]=0;
        for(ULONG page=0;page<32;page++) {
            ULONG64 address=(ULONG64)slice*GSP_VRAM_SLICE_BYTES+
                            (ULONG64)page*0x10000;
            if(address+0xffff<=dev->GspFbFreeLimit)
                lpt[page]=(address>>4)|1;
        }
    }
    /* DMA allocations are only 4KB aligned. 64KB leaves silently discard
       low physical address bits and read unrelated bytes on real XP boots.
       One SPT maps the 64KB staging window used by bounded upload requests. */
    tables[2560+GSP_VRAM_SLICE_COUNT*2]=0;
    tables[2560+GSP_VRAM_SLICE_COUNT*2+1]=
        ((physical+GSP_UPLOAD_PTE_OFFSET)>>4)|0xc;
    for(ULONG page=0;page<16;page++) {
        ULONG64 pagePhysical=dev->GspDmaPhysical.QuadPart+GSP_UPLOAD_OFFSET+
            (ULONG64)page*0x1000;
        tables[GSP_UPLOAD_PTE_OFFSET/sizeof(*tables)+page]=
            (pagePhysical>>4)|0xd;
    }
    dev->GspVramGpuVa=GSP_VRAM_GPU_VA;
    dev->GspVramMapBytes=GSP_VRAM_MAP_BYTES;
    __sync_synchronize();
    setDirectory[0]=(ULONG)physical;setDirectory[1]=(ULONG)(physical>>32);
    setDirectory[2]=4; /* top-level 49-bit directory has four entries */
    setDirectory[3]=1; /* SYSMEM_COH */
    setDirectory[4]=dev->GspVaSpaceHandle;
    if(!NvxGspControl(dev,dev->GspDeviceHandle,NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY,setDirectory,sizeof(setDirectory))) return FALSE;
    VideoPortZeroMemory(&dev->GspGartHeap,sizeof(dev->GspGartHeap));
    dev->GspGartReady=TRUE;
    dev->GspScratchGpuVa=0x10000000;
    LogStr("NVX [RM]: scratch memory mapped at GPU VA 0x10000000.\n");return TRUE;
}

static BOOLEAN NvxGspGetFbLayout(PNVX_DEVICE_EXTENSION dev)
{
    /* Verified against 535 SDK types: GspStaticConfigInfo.fbRegionInfoParams
       is at 840, contains an 8-byte prefix and sixteen 48-byte entries.
       GET_GSP_STATIC_INFO is the same pointer-free RPC used by Nouveau. */
    const ULONG infoOffset=840,entriesOffset=848;
    ULONG request[542]={0}; /* sizeof(GspStaticConfigInfo) = 2168 in SDK 535. */
    const GSP_FW_WPR_META_V1 *wpr=(const void *)((PUCHAR)dev->GspDmaVirtual+GSP_WPR_META_OFFSET);
    ULONG count=0;
    ULONG64 freeBase=0,freeLimit=0;
    dev->GspFbLayoutValid=FALSE;dev->GspFbRegionCount=0;
    dev->GspFbFreeBase=dev->GspFbFreeLimit=0;
    dev->GspBar1PageDirectory=dev->GspBar2PageDirectory=0;
    if(!NvxGspCall(dev,65,request,sizeof(request))||((const ULONG *)dev->RpcRxRecord)[14]<32+sizeof(request)) return FALSE;
    VideoPortMoveMemory(&dev->GspBar1PageDirectory,dev->RpcRxRecord+80+2088,8);
    VideoPortMoveMemory(&dev->GspBar2PageDirectory,dev->RpcRxRecord+80+2096,8);
    LogStr("NVX [FB]: firmware BAR1 page directory=");LogHex(dev->GspBar1PageDirectory);
    LogStr(" BAR2=");LogHex(dev->GspBar2PageDirectory);LogStr("\n");
    VideoPortMoveMemory(&count,dev->RpcRxRecord+80+infoOffset,sizeof(count));
    if(!count||count>16) return FALSE;
    for(ULONG i=0;i<count;i++) {
        NVX_FB_REGION_535 region;
        VideoPortMoveMemory(&region,dev->RpcRxRecord+80+entriesOffset+i*sizeof(region),sizeof(region));
        if(region.base>region.limit||region.limit>=wpr->fbSize) return FALSE;
        LogStr("NVX [FB]: region=");LogHex(i);LogStr(" base=");LogHex(region.base);
        LogStr(" limit=");LogHex(region.limit);LogStr(" reserved=");LogHex(region.reserved);
        LogStr(" protected=");LogHex(region.protectedMemory);LogStr(" iso=");LogHex(region.iso);LogStr("\n");
        if(!freeLimit&&!region.reserved&&!region.protectedMemory&&region.compressed&&region.iso) {
            ULONG64 base=(region.base+0xffffULL)&~0xffffULL,limit=region.limit;
            if(base<wpr->gspFwWprEnd&&limit>=wpr->gspFwWprStart) {
                if(base<wpr->gspFwWprStart) limit=wpr->gspFwWprStart-1;
                else base=(wpr->gspFwWprEnd+0xffffULL)&~0xffffULL;
            }
            if(base<=limit&&limit-base>=0xffff) {freeBase=base;freeLimit=limit;}
        }
    }
    dev->GspFbRegionCount=count;dev->GspFbFreeBase=freeBase;dev->GspFbFreeLimit=freeLimit;
    dev->GspFbLayoutValid=TRUE;
    LogStr("NVX [FB]: available VRAM candidate base=");LogHex(freeBase);
    LogStr(" limit=");LogHex(freeLimit);LogStr("; discovery only, no VRAM writes.\n");
    return TRUE;
}

typedef struct { PNVX_DEVICE_EXTENSION dev; ULONG64 bytes; ULONG reads; } NVX_PRAMIN_CONTEXT;
static int NvxPraminRead64(void *context,uint64_t address,uint64_t *value)
{
    NVX_PRAMIN_CONTEXT *ctx=context;
    PUCHAR bar0=(PUCHAR)ctx->dev->Bar0MmioVirtual;
    ULONG saved,lo,hi;
    if(!value||(address&7)||ctx->bytes<8||address>ctx->bytes-8||
       ctx->dev->Bar0Length<0x710000) return 0;
    saved=VideoPortReadRegisterUlong((PULONG)(bar0+0x1700));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x1700),(ULONG)(address>>16));
    (void)VideoPortReadRegisterUlong((PULONG)(bar0+0x1700));
    lo=VideoPortReadRegisterUlong((PULONG)(bar0+0x700000+(ULONG)(address&0xffff)));
    hi=VideoPortReadRegisterUlong((PULONG)(bar0+0x700004+(ULONG)(address&0xffff)));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x1700),saved);
    (void)VideoPortReadRegisterUlong((PULONG)(bar0+0x1700));
    *value=((uint64_t)hi<<32)|lo;
    if(ctx->reads<12) {
        LogStr("NVX [FBMAP]: PRAMIN[ ");LogHex(address);LogStr(" ]=");LogHex(*value);LogStr("\n");
    }
    ctx->reads++;
    return *value!=UINT64_MAX;
}

BOOLEAN NvxGspDiscoverScanout(PVOID extension,ULONG index)
{
    PNVX_DEVICE_EXTENSION dev=extension;
    const GSP_FW_WPR_META_V1 *wpr;
    NVX_PRAMIN_CONTEXT context;
    ULONG block;
    ULONG64 instance,pdbRaw,pdb,offset,bytes,mapped=0,first=0;
    unsigned bigShift;
    dev->GspScanoutMapped=FALSE;
    if(!dev->GspInitialized||!dev->GspCopyVerified||!dev->GspDmaVirtual||index>=dev->NumModes) return FALSE;
    wpr=(const void *)((PUCHAR)dev->GspDmaVirtual+GSP_WPR_META_OFFSET);
    bytes=(ULONG64)dev->Modes[index].ScreenStride*dev->Modes[index].VisScreenHeight;
    if(!bytes||dev->ModeFramebuffer[index].QuadPart<dev->PhysicalBar1.QuadPart) return FALSE;
    offset=dev->ModeFramebuffer[index].QuadPart-dev->PhysicalBar1.QuadPart;
    if(offset>dev->Bar1Length||bytes>dev->Bar1Length-offset) return FALSE;
    block=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x1704));
    if(!(block&0x80000000U)||(block&0x70000000U)) return FALSE;
    instance=(ULONG64)(block&0x0fffffffU)<<12;
    context.dev=dev;context.bytes=wpr->fbSize;context.reads=0;
    if(!NvxPraminRead64(&context,instance+0x200,&pdbRaw)) return FALSE;
    LogStr("NVX [FBMAP]: BAR1 block=");LogHex(block);LogStr(" instance=");LogHex(instance);
    LogStr(" pdbRaw=");LogHex(pdbRaw);LogStr(" firmware root=");LogHex(dev->GspBar1PageDirectory);LogStr("\n");
    if((pdbRaw&3)||!(pdbRaw&0x400)) return FALSE;
    pdb=pdbRaw&~0xfffULL;bigShift=(pdbRaw&0x800)?16:17;
    while(mapped<bytes) {
        NVX_MMU_MAPPING map;
        ULONG64 chunk;
        int status=NvxMmuTranslateVram(NvxPraminRead64,&context,pdb,offset+mapped,wpr->fbSize,bigShift,&map);
        if(status!=NVX_MMU_OK||!map.writable) {
            LogStr("NVX [FBMAP]: translation unavailable offset=");LogHex(offset+mapped);
            LogStr(" status=");LogHex((ULONG)status);LogStr("\n");return FALSE;
        }
        if(!mapped) first=map.physical;
        else if(map.physical!=first+mapped) {
            LogStr("NVX [FBMAP]: noncontiguous scanout; GPU drawing remains disabled.\n");return FALSE;
        }
        chunk=(1ULL<<map.pageShift)-((offset+mapped)&((1ULL<<map.pageShift)-1));
        if(chunk>bytes-mapped) chunk=bytes-mapped;
        mapped+=chunk;
    }
    if(first<wpr->gspFwWprEnd&&first+bytes>wpr->gspFwWprStart) return FALSE;
    dev->GspScanoutPhysical=first;dev->GspScanoutBytes=bytes;dev->GspScanoutMapped=TRUE;
    LogStr("NVX [FBMAP]: linear writable scanout VRAM=");LogHex(first);
    LogStr(" bytes=");LogHex(bytes);LogStr("; discovery only.\n");return TRUE;
}

static BOOLEAN NvxGspSelectCopyEngine(PNVX_DEVICE_EXTENSION dev)
{
    /* The public GET_CAPS_V2 is host-side. kceGetDeviceCaps calls the GSP
       GET_PHYSICAL_CAPS API with this same 8-byte pointer-free structure. */
    dev->GspCopyEngineType=0;
    for(ULONG engine=9;engine<19;engine++) {
        ULONG caps[2]={engine,0};
        if(!NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_CE_GET_PHYSICAL_CAPS,caps,sizeof(caps))) continue;
        ULONG bits=((const ULONG *)dev->RpcRxRecord)[27]&0xffff;
        LogStr("NVX [COPY]: CE engine=");LogHex(engine);LogStr(" capabilities=");LogHex(bits);LogStr("\n");
        if((bits&0x21)==0x20) {
            dev->GspCopyEngineType=engine;
            LogStr("NVX [COPY]: selected dedicated sysmem CE engine=");LogHex(engine);LogStr("\n");return TRUE;
        }
    }
    LogStr("NVX [COPY]: no dedicated sysmem-capable CE found; channel allocation skipped.\n");return FALSE;
}

static BOOLEAN NvxGspAllocChannel(PNVX_DEVICE_EXTENSION dev)
{
    /* 1. TSG (KEPLER_CHANNEL_GROUP_A = 0xA06C) on Device:
       NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS (20 bytes):
       hObjectError, hObjectEccError, hVASpace, engineType, bIsCallingContextVgpuPlugin.
       Use the selected dedicated CE for the scratch-copy test. 535's GA104
       class table exposes AMPERE_DMA_COPY_B (0xC7B5), not GA100's 0xC6B5. */
    ULONG tsg[5]={0, 0, dev->GspVaSpaceHandle, dev->GspCopyEngineType, 0};
    /* 2. Context Share (FERMI_CONTEXT_SHARE_A = 0x9067) on TSG:
       NV_CTXSHARE_ALLOCATION_PARAMETERS (12 bytes):
       hVASpace, flags, subctxId. */
    ULONG ctxShare[3]={dev->GspVaSpaceHandle, 0, 0}; /* SUBCONTEXT_SYNC = 0, subctxId = 0 */
    /* 3. NV_CHANNEL_ALLOC_PARAMS for 535.113.01: 360 bytes (90 ULONGs).
       AMPERE_CHANNEL_GPFIFO_A (0xC56F) allocated under TSG parent with hContextShare. */
    ULONG chan[90]={0};
    ULONG bindEngine=dev->GspCopyEngineType;
    UCHAR schedEnable[2]={1, 0};
    /* NVC0B5_ALLOCATION_PARAMETERS: version=1, selected NV2080 engine type. */
    ULONG ceParams[2]={1, dev->GspCopyEngineType};
    ULONG64 scratchPhys=dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET;
    ULONG64 instPhys=scratchPhys+0x2000;
    ULONG64 userdPhys=scratchPhys+0x3200; /* USERD slot 1 in its 4KB page */
    ULONG64 mthdbufPhys=scratchPhys+0x4000;
    ULONG mthdSize=0; /* Require the firmware's size; never guess for DMA storage. */
    ULONG ceMthdParam=0;

    /* Query CE fault method buffer size on Subdevice (NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE) */
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE,&ceMthdParam,sizeof(ceMthdParam))) {
        const ULONG *words=(const ULONG *)dev->RpcRxRecord;
        if(words[24]>=4) {
            mthdSize=*(const ULONG *)(dev->RpcRxRecord+104);
            LogStr("NVX [RM]: queried CE fault method buffer size: ");LogHex(mthdSize);LogStr("\n");
        }
    }
    /* GR GPFIFO occupies 0x9000; runtime CE push slots use 0xd000; C797
       push data uses 0xf000. Keep all three allocations disjoint. */
    if(!mthdSize||mthdSize>0x5000||!dev->GspScratchGpuVa) return FALSE;

    /* Allocate TSG on Device */
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0xa06c0001,0xa06c,tsg,sizeof(tsg))) {
        LogStr("NVX [RM]: KEPLER_CHANNEL_GROUP_A allocation failed.\n");
        return FALSE;
    }
    dev->GspChannelGroupHandle=0xa06c0001;
    LogStr("NVX [RM]: KEPLER_CHANNEL_GROUP_A allocated.\n");

    /* Allocate Context Share on TSG */
    if(!NvxGspAlloc(dev,dev->GspChannelGroupHandle,0x90670001,0x9067,ctxShare,sizeof(ctxShare))) {
        LogStr("NVX [RM]: FERMI_CONTEXT_SHARE_A allocation failed.\n");
        return FALSE;
    }
    dev->GspContextShareHandle=0x90670001;
    LogStr("NVX [RM]: FERMI_CONTEXT_SHARE_A allocated.\n");

    chan[2]=(ULONG)dev->GspScratchGpuVa;
    chan[3]=(ULONG)(dev->GspScratchGpuVa>>32);
    chan[4]=1024;       /* gpFifoEntries */
    /* flags: CHANNEL_TYPE_PHYSICAL (0), PRIVILEGED (0x20),
       CHANNEL_USERD_INDEX_PAGE_FIXED (0x00200000), userd_i = 1 (0x100) -> 0x00200120 (ChID = 1).
       GSP-RM client RPC requires PAGE_FIXED for client-allocated USERD. */
    chan[5]=0x00200120;
    chan[6]=dev->GspContextShareHandle; /* hContextShare = Context Share handle under TSG */
    chan[7]=0;                          /* hVASpace = 0 when hContextShare is set! */
    chan[32]=dev->GspCopyEngineType;
    chan[34]=0;                         /* subDeviceId = 0 */

    /* All descriptors describe OS-owned coherent SYSTEM memory.
       In the RM ABI, NV_MEMORY_CACHED=0 and NV_MEMORY_UNCACHED=1.
       Passing NV_MEMORY_CACHED (0) instructs GSP-RM to map system memory as
       GMMU_APERTURE_SYS_COH (coherent PCIe snooping enabled), matching official
       NVIDIA driver (kernel_channel.c) and Nouveau (r535/fifo.c).
       Passing UNCACHED (1) causes GSP-RM to set SYS_NONCOH, generating PCIe
       No-Snoop accesses which fail bare-metal CPU-GPU coherency on USERD. */
    chan[36]=(ULONG)instPhys;
    chan[37]=(ULONG)(instPhys>>32);
    chan[38]=0x1000;
    chan[40]=1;         /* ADDR_SYSMEM, never a VRAM physical offset */
    chan[41]=0;         /* NV_MEMORY_CACHED (coherent sysmem) */

    /* userdMem: slot 1, matching CHANNEL_USERD_INDEX_VALUE. */
    chan[42]=(ULONG)userdPhys;
    chan[43]=(ULONG)(userdPhys>>32);
    chan[44]=0x200;
    chan[46]=1;         /* ADDR_SYSMEM */
    chan[47]=0;         /* NV_MEMORY_CACHED (coherent sysmem) */

    /* ramfcMem aliases the beginning of the instance block. */
    chan[48]=(ULONG)instPhys;
    chan[49]=(ULONG)(instPhys>>32);
    chan[50]=0x200;
    chan[52]=1;         /* ADDR_SYSMEM */
    chan[53]=0;         /* NV_MEMORY_CACHED (coherent sysmem) */

    /* mthdbufMem: at scratch offset 0x4000 (ADDR_SYSMEM = 1, CACHED = 0) */
    chan[54]=(ULONG)mthdbufPhys;
    chan[55]=(ULONG)(mthdbufPhys>>32);
    chan[56]=mthdSize?mthdSize:0x1000;
    chan[58]=1;         /* addressSpace = ADDR_SYSMEM */
    chan[59]=0;         /* cacheAttrib = NV_MEMORY_CACHED */

    /* internalFlags: ADMIN (1) | ERROR_NOTIFIER_TYPE_NONE (4) | ECC_ERROR_NOTIFIER_TYPE_NONE (0x10) */
    chan[61]=0x15;

    /* Allocate channel under TSG (parent = dev->GspChannelGroupHandle) */
    if(!NvxGspAlloc(dev,dev->GspChannelGroupHandle,0xc56f0001,0xc56f,chan,sizeof(chan))) {
        LogStr("NVX [RM]: AMPERE_CHANNEL_GPFIFO_A allocation failed.\n");
        return FALSE;
    }
    dev->GspChannelHandle=0xc56f0001;
    LogStr("NVX [RM]: AMPERE_CHANNEL_GPFIFO_A allocated under TSG.\n");

    /* Bind channel to the capability-selected dedicated CE. */
    if(!NvxGspControl(dev,dev->GspChannelHandle,NVA06F_CTRL_CMD_BIND,&bindEngine,sizeof(bindEngine))) {
        LogStr("NVX [RM]: NVA06F_CTRL_CMD_BIND failed.\n");
        return FALSE;
    }
    LogStr("NVX [RM]: channel bound to dedicated CE engine=");LogHex(bindEngine);LogStr("\n");

    /* Schedule channel */
    if(!NvxGspControl(dev,dev->GspChannelHandle,NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,schedEnable,sizeof(schedEnable))) {
        LogStr("NVX [RM]: NVA06F_CTRL_CMD_GPFIFO_SCHEDULE failed.\n");
        return FALSE;
    }
    LogStr("NVX [RM]: channel scheduled.\n");

    if(!NvxGspAlloc(dev,dev->GspChannelHandle,0xc7b50001,0xc7b5,ceParams,sizeof(ceParams))) {
        LogStr("NVX [RM]: AMPERE_DMA_COPY_B allocation failed.\n");
        return FALSE;
    }
    dev->GspCopyEngineHandle=0xc7b50001;
    LogStr("NVX [RM]: AMPERE_DMA_COPY_B allocated.\n");

    return TRUE;
}

/*
 * Initialize an isolated RM-managed XP x64 graphics channel alongside the
 * known-good CE path. The normal channel setup validates scheduling and a
 * semaphore completion, but does not submit a user draw or expose arbitrary
 * pushbuffer methods. The optional submission probe adds shader workloads.
 */
#if NVX_ENABLE_GR_CHANNEL_PROBE
#define NVX_FAULT_SHADOW_PAGE 0x1000U
#define NVX_FAULT_QUEUE_CAPACITY_OFFSET 0U
#define NVX_FAULT_QUEUE_MSGSIZE_OFFSET 24U
#define NVX_FAULT_QUEUE_GET_OFFSET 64U
#define NVX_FAULT_QUEUE_PUT_OFFSET 128U

static BOOLEAN NvxGspSetupFaultBuffer(PNVX_DEVICE_EXTENSION dev,ULONG expectedSize,ULONG metadata,ULONG classId)
{
    NVB069_CTRL_FAULTBUFFER_GET_SIZE_PARAMS getSize={0};
    NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS *params;
    const NVB069_CTRL_FAULTBUFFER_GET_SIZE_PARAMS *sizeReply;
    ULONG handle=0xc1d00030,dataBytes,pages,i;
    ULONG64 queuePhys,bufferPhys;
    if(!dev||!expectedSize||metadata||!classId||dev->GspFaultBufferHandle||
       dev->GspFaultQueueVirtual||dev->GspFaultShadowVirtual) return FALSE;
    if(classId!=MMU_FAULT_BUFFER||
       !NvxGspAlloc(dev,dev->GspSubdeviceHandle,handle,classId,NULL,0)) {
        LogStr("NVX [GR-FAULT]: NVIDIA C369 object allocation failed class=");LogHex(classId);LogStr("\n");
        return FALSE;
    }
    dev->GspFaultBufferHandle=handle;
    if(NvxGspControl(dev,handle,NVB069_CTRL_CMD_FAULTBUFFER_GET_SIZE,&getSize,sizeof(getSize))) {
        sizeReply=(const NVB069_CTRL_FAULTBUFFER_GET_SIZE_PARAMS *)(dev->RpcRxRecord+104);
        LogStr("NVX [GR-FAULT]: replayable HW ring bytes=");LogHex(sizeReply->faultBufferSize);
        LogStr(" nonreplay shadow bytes=");LogHex(expectedSize);LogStr("\n");
    }
    if(expectedSize>0xffffffffU-(NVX_FAULT_SHADOW_PAGE-1)) return FALSE;
    dataBytes=(expectedSize+NVX_FAULT_SHADOW_PAGE-1)&~(NVX_FAULT_SHADOW_PAGE-1);
    pages=dataBytes/NVX_FAULT_SHADOW_PAGE;
    if(pages>NV2080_CTRL_INTERNAL_GMMU_CLIENT_SHADOW_FAULT_BUFFER_MAX_PAGES) return FALSE;
    /* Mirror Linux RM's two cached allocations: one Queue structure page and
       a separate packet-page allocation, then register their respective DMA
       addresses. */
    dev->GspFaultQueueLength=NVX_FAULT_SHADOW_PAGE;
    dev->GspFaultQueueVirtual=VideoPortAllocateCommonBuffer(dev,dev->DmaAdapter,
        dev->GspFaultQueueLength,&dev->GspFaultQueuePhysical,TRUE,NULL);
    if(!dev->GspFaultQueueVirtual) return FALSE;
    dev->GspFaultShadowLength=dataBytes;
    dev->GspFaultShadowVirtual=VideoPortAllocateCommonBuffer(dev,dev->DmaAdapter,
        dev->GspFaultShadowLength,&dev->GspFaultShadowPhysical,TRUE,NULL);
    if(!dev->GspFaultShadowVirtual) return FALSE;
    queuePhys=(ULONG64)dev->GspFaultQueuePhysical.QuadPart;
    bufferPhys=(ULONG64)dev->GspFaultShadowPhysical.QuadPart;
    if((queuePhys&(NVX_FAULT_SHADOW_PAGE-1))||(bufferPhys&(NVX_FAULT_SHADOW_PAGE-1))||
       queuePhys+dev->GspFaultQueueLength>0x100000000ULL||
       bufferPhys+dataBytes>0x100000000ULL) {
        LogStr("NVX [GR-FAULT]: queue/data DMA allocations not page-aligned below 4GB.\n");
        return FALSE;
    }
    VideoPortZeroMemory(dev->GspFaultQueueVirtual,dev->GspFaultQueueLength);
    VideoPortZeroMemory(dev->GspFaultShadowVirtual,dataBytes);
    {
        volatile ULONG64 *q=(volatile ULONG64 *)dev->GspFaultQueueVirtual;
        q[NVX_FAULT_QUEUE_CAPACITY_OFFSET/8]=expectedSize/32;
        q[NVX_FAULT_QUEUE_MSGSIZE_OFFSET/8]=32;
        q[NVX_FAULT_QUEUE_GET_OFFSET/8]=0;
        q[NVX_FAULT_QUEUE_PUT_OFFSET/8]=0;
    }
    params=(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS *)dev->RpcRxRecord;
    VideoPortZeroMemory(params,sizeof(*params));
    params->shadowFaultBufferQueuePhysAddr=queuePhys;
    params->shadowFaultBufferSize=expectedSize;
    params->shadowFaultBufferMetadataSize=metadata;
    for(i=0;i<pages;i++) params->shadowFaultBufferPteArray[i]=bufferPhys+(ULONG64)i*NVX_FAULT_SHADOW_PAGE;
    params->shadowFaultBufferType=NV2080_CTRL_FAULT_BUFFER_NON_REPLAYABLE;
    dev->GspFaultShadowRegistered=TRUE;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,
        NV2080_CTRL_CMD_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER,
        params,sizeof(*params))) {
        LogStr("NVX [GR-FAULT]: internal shadow queue registration failed/timed out.\n");
        return FALSE;
    }
    LogStr("NVX [GR-FAULT]: C369 object plus internal shadow queue registered; pages=");
    LogHex(pages);LogStr(" queue=");LogHex(queuePhys);LogStr(" packetBuffer=");LogHex(bufferPhys);
    LogStr(" bytes=");LogHex(dataBytes);LogStr("\n");
    return TRUE;
}

static VOID NvxGspDumpFaultShadow(PNVX_DEVICE_EXTENSION dev)
{
    volatile UCHAR *base;
    volatile ULONG64 *q;
    ULONG64 get,put,capacity,count,idx;
    ULONG i;
    if(!dev||!dev->GspFaultQueueVirtual||!dev->GspFaultShadowVirtual||!dev->GspFaultQueueLength) return;
    base=(volatile UCHAR *)dev->GspFaultShadowVirtual;
    q=(volatile ULONG64 *)dev->GspFaultQueueVirtual;
    get=q[NVX_FAULT_QUEUE_GET_OFFSET/8];put=q[NVX_FAULT_QUEUE_PUT_OFFSET/8];
    capacity=q[NVX_FAULT_QUEUE_CAPACITY_OFFSET/8];__sync_synchronize();
    if(!capacity||capacity>0x100000||get>=capacity||put>=capacity) {
        LogStr("NVX [GR-FAULT]: invalid queue get=");LogHex(get);LogStr(" put=");LogHex(put);LogStr(" capacity=");LogHex(capacity);LogStr("\n");return;
    }
    count=(put+capacity-get)%capacity;
    LogStr("NVX [GR-FAULT]: queue get=");LogHex(get);LogStr(" put=");LogHex(put);LogStr(" pending=");LogHex(count);LogStr("\n");
    for(idx=0;idx<count&&idx<4;idx++) {
        ULONG64 slot=(get+idx)%capacity;
        volatile ULONG *packet=(volatile ULONG *)(base+slot*32);
        LogStr("NVX [GR-FAULT]: packet");LogHex(idx);
        for(i=0;i<8;i++){LogStr(" w");LogHex(i);LogStr("=");LogHex(packet[i]);}
        LogStr("\n");
    }
}

BOOLEAN NvxGspUnregisterFaultShadow(PNVX_DEVICE_EXTENSION dev)
{
    NV2080_CTRL_INTERNAL_GMMU_UNREGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS params;
    if(!dev||!dev->GspFaultShadowRegistered) return TRUE;
    params.shadowFaultBufferType=NV2080_CTRL_FAULT_BUFFER_NON_REPLAYABLE;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,
        NV2080_CTRL_CMD_INTERNAL_GMMU_UNREGISTER_CLIENT_SHADOW_FAULT_BUFFER,
        &params,sizeof(params))) return FALSE;
    dev->GspFaultShadowRegistered=FALSE;
    LogStr("NVX [GR-FAULT]: internal shadow queue unregistered.\n");
    return TRUE;
}

static VOID NvxGspLogFaultRingPointers(PNVX_DEVICE_EXTENSION dev)
{
    ULONG type;
    if(!dev||!dev->Bar0MmioVirtual||dev->Bar0Length<4) return;
    for(type=NVB069_CTRL_FAULT_BUFFER_NON_REPLAYABLE;
        type<=NVB069_CTRL_FAULT_BUFFER_REPLAYABLE;type++) {
        ULONG getOffset=0x00B80000U+0x00003008U+type*32U;
        ULONG putOffset=0x00B80000U+0x0000300CU+type*32U;
        if(getOffset>dev->Bar0Length-4||putOffset>dev->Bar0Length-4) {
            LogStr("NVX [GR-FAULT]: documented MMU ring registers outside BAR0; offset=");
            LogHex(getOffset);LogStr(" BAR0 bytes=");LogHex(dev->Bar0Length);LogStr("\n");
            return;
        }
        LogStr("NVX [GR-FAULT]: BAR0 ring type=");LogHex(type);
        LogStr(" GET@");LogHex(getOffset);LogStr("=");
        LogHex(nvx_reg_read32(dev->Bar0MmioVirtual,getOffset));
        LogStr(" PUT@");LogHex(putOffset);LogStr("=");
        LogHex(nvx_reg_read32(dev->Bar0MmioVirtual,putOffset));LogStr("\n");
    }
}

static BOOLEAN NvxGspCreateGraphicsProbe(PNVX_DEVICE_EXTENSION dev)
{
    NV_CHANNEL_ALLOC_PARAMS chan={0};
    NV_CONTEXT_DMA_ALLOCATION_PARAMS errorDma={0};
    NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS tsg={0};
    NV_CTXSHARE_ALLOCATION_PARAMETERS ctxShare={0};
    ULONG bindEngine=NV2080_ENGINE_TYPE_GRAPHICS;
    UCHAR schedEnable[2]={1,0};
    ULONG64 scratchPhys,instPhys,userdPhys,ramfcPhys,methodPhys,fifoGpuVa,scratchEnd;
    NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS supportedClasses={0};
    ULONG classEngineId=0,faultBufferClass=0;
    NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS classEngineParams={0};
    NVB06F_CTRL_GET_ENGINE_CTX_SIZE_PARAMS grCtxSize={0};
    NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS grHwState={0};
    NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS faultStatic={0};
    NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS *grBufferInfo;
    NVX_GR_REQUIREMENT grRequirements[NVX_GR_REQUIREMENT_COUNT]={0};
    NVX_GR_LAYOUT grLayout;
    ULONG grMaxSubcontexts=0,grHeaderSupported=0;
    BOOLEAN grRequirementsValid=FALSE,grHeaderInfoValid=FALSE;
    BOOLEAN grStatusAvailable=FALSE;
    ULONG classCount,classIndex,grClass=0;
    BOOLEAN ampereAFound=FALSE,ampereBFound=FALSE;
    ULONG runlistQuery[124]={0},token=0,runlistBase=0;
    ULONG dbcfg,chcfg,enable,wait,chramBase,fault[26]={0},assertCountBefore;
    ULONG64 pushVa,grFenceVa,grDataVa;
#if NVX_ENABLE_GR_SUBMISSION_PROBE
    ULONG64 grShaderVa,grVertexCbVa,grVertexVa,grTextureVa,grBindlessVa,grTextureDataVa;
    ULONG grShaderOffset,grPixelCount=0,grZeroCount=0,grWhiteCount=0;
    PULONG grVertexCb,grVertex,grTexture,grBindless;
    static const ULONG grTriangleVertices[18]={
        0,0,0,0,0,0x3f800000U,
        0,0x3f800000U,0,0x3f800000U,0,0x3f800000U,
        0x3f800000U,0,0x3f800000U,0,0,0x3f800000U
    };
    static const ULONG grVertexUniforms[8]={64,16,1,1,0,0,0,0};
#endif
    PUCHAR scratch;
    PULONG push;
    volatile ULONG *userd;
    volatile ULONG *grFence;
    volatile ULONG *grData;
    volatile ULONG *grReadback;
    volatile ULONG64 *fifo;
    const ULONG channelHandle=0xc56f0002;
    const ULONG grHandle=0xc6970001;
    const ULONG tsgHandle=0xa06c0002;
    const ULONG ctxShareHandle=0x90670002;
    const ULONG grContextHandle=0x90000002;

    if(!dev||!dev->GspInitialized||!dev->GspDmaVirtual||!dev->GspScratchGpuVa) {
        LogStr("NVX [GR]: probe preconditions failed; graphics remains disabled.\n");
        return FALSE;
    }
    dev->GspGraphicsM2mfObjectHandle=0;
    dev->GspGraphicsCopyObjectHandle=0;
    dev->GspGraphics2dObjectHandle=0;
    dev->GspGraphicsComputeObjectHandle=0;
    dev->GspGraphicsObjectClassEngineId=0;
    dev->GspGraphicsComputeClassEngineId=0;
    dev->GspGraphicsM2mfClassEngineId=0;
    dev->GspGraphicsCopyClassEngineId=0;
    dev->GspGraphics2dClassEngineId=0;
    grBufferInfo=&dev->GrContextInfoParams;
    scratchPhys=(ULONG64)dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET;
    instPhys=scratchPhys+0xa000;
    userdPhys=scratchPhys+0xb400;
    ramfcPhys=scratchPhys+0xb000;
    methodPhys=scratchPhys+0xc000;
    fifoGpuVa=dev->GspScratchGpuVa+0x9000;
    scratchEnd=scratchPhys+GSP_SCRATCH_SIZE;
    if(scratchEnd>(ULONG64)dev->GspDmaPhysical.QuadPart+(ULONG64)dev->GspDmaLength||
       (fifoGpuVa&7)||(instPhys&0xfff)||(methodPhys&0xfff)) {
        LogStr("NVX [GR]: probe preconditions failed; graphics remains disabled.\n");
        return FALSE;
    }

    /* Ask RM which object classes this device firmware actually exposes. */
    if(!NvxGspControl(dev,dev->GspDeviceHandle,NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2,
                      &supportedClasses,sizeof(supportedClasses))) {
        LogStr("NVX [GR]: GPU_GET_CLASSLIST_V2 query failed.\n");
        return FALSE;
    }
    {
        const NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS *reply=
            (const NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS *)(dev->RpcRxRecord+104);
        classCount=reply->numClasses;
        if(classCount>NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE) {
        LogStr("NVX [GR]: RM class-list count invalid: ");LogHex(classCount);LogStr("\n");
        return FALSE;
        }
        for(classIndex=0;classIndex<classCount;classIndex++) {
            if(reply->classList[classIndex]==AMPERE_A) ampereAFound=TRUE;
            if(reply->classList[classIndex]==AMPERE_B) ampereBFound=TRUE;
            if(reply->classList[classIndex]==MAXWELL_FAULT_BUFFER_A||
               reply->classList[classIndex]==MMU_FAULT_BUFFER) {
                if(reply->classList[classIndex]>faultBufferClass) faultBufferClass=reply->classList[classIndex];
            }
        }
    }
    LogStr("NVX [GR]: RM advertised class count=");LogHex(classCount);
    LogStr(" AMPERE_A=");LogHex(ampereAFound?1:0);
    LogStr(" AMPERE_B=");LogHex(ampereBFound?1:0);
    LogStr(" faultBufferClass=");LogHex(faultBufferClass);LogStr("\n");
    /* GA104 (RTX 3070) advertises AMPERE_B; GA100 exposes AMPERE_A. */
    if(ampereBFound) grClass=AMPERE_B;
    else if(ampereAFound) grClass=AMPERE_A;
    if(!grClass) {
        LogStr("NVX [GR]: firmware advertises no Ampere graphics class; skipping channel allocation.\n");
        return FALSE;
    }

    LogStr("NVX [GR]: allocating GR channel with RM-owned context resources.\n");
    /* Follow NVK's NVRM channel setup: allocate a real context-DMA error
       notifier, provide USERD by RM memory handle, and let RM allocate/manage
       instance/RAMFC/method buffers. Supplying physical descriptors here
       bypasses the normal RM ownership path and left GR without a context. */
    errorDma.flags=DRF_DEF(OS03,_FLAGS,_MAPPING,_KERNEL) |
                   DRF_DEF(OS03,_FLAGS,_HASH_TABLE,_DISABLE);
    errorDma.hMemory=dev->GspScratchMemoryHandle;
    errorDma.offset=0xe000;
    /* limit is an inclusive length relative to offset, not an absolute end. */
    errorDma.limit=0xfff;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0x00020003,
                    NV01_CONTEXT_DMA,&errorDma,sizeof(errorDma))) {
        LogStr("NVX [GR]: error notifier context-DMA allocation failed.\n");
        return FALSE;
    }
    LogStr("NVX [GR]: RM error notifier context-DMA allocated.\n");

    chan.gpFifoOffset=fifoGpuVa;
    chan.gpFifoEntries=512;
    chan.flags=0x00200220; /* privileged, fixed USERD page, index 2 */
    /* Use NVK's explicit graphics TSG + synchronized context-share form.
       The previous bare-channel form allocated but GSP-RM never associated
       it with a KernelGraphicsContext (GET_CTX_BUFFER_INFO returned 0x31). */
    tsg.hVASpace=dev->GspVaSpaceHandle;
    tsg.engineType=NV2080_ENGINE_TYPE_GRAPHICS;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,tsgHandle,0xa06c,&tsg,sizeof(tsg))) {
        LogStr("NVX [GR]: graphics TSG allocation failed.\n");
        return FALSE;
    }
    ctxShare.hVASpace=dev->GspVaSpaceHandle;
    ctxShare.flags=NV_CTXSHARE_ALLOCATION_FLAGS_SUBCONTEXT_SYNC;
    ctxShare.subctxId=0;
    if(!NvxGspAlloc(dev,tsgHandle,ctxShareHandle,0x9067,&ctxShare,sizeof(ctxShare))) {
        LogStr("NVX [GR]: synchronized graphics context-share allocation failed.\n");
        return FALSE;
    }
    /* OpenRM normally creates this internal-only resource from the TSG
       constructor. Probe explicit GSP-RM allocation to distinguish a missing
       graphics context from a context-info control limitation. */
    if(NvxGspAlloc(dev,tsgHandle,grContextHandle,KERNEL_GRAPHICS_CONTEXT,NULL,0))
        LogStr("NVX [GR]: explicit kernel graphics-context allocation succeeded.\n");
    else
        LogStr("NVX [GR]: explicit kernel graphics-context allocation was rejected; continuing probe.\n");
    dev->GspGraphicsTsgHandle=tsgHandle;
    dev->GspGraphicsContextShareHandle=ctxShareHandle;
    chan.hContextShare=ctxShareHandle;
    chan.hVASpace=0; /* mutually exclusive with hContextShare */
    chan.engineType=NV2080_ENGINE_TYPE_GRAPHICS;
    chan.subDeviceId=0;
    chan.hObjectError=0x00020003;
    chan.hUserdMemory[0]=dev->GspScratchMemoryHandle;
    chan.userdOffset[0]=0xb400;
    /* CPU-RM normally derives these before its GSP channel-allocation RPC:
       ADMIN | ERROR_NOTIFIER_TYPE_CTXDMA | ECC_ERROR_NOTIFIER_TYPE_NONE. */
    chan.internalFlags=0x19;
    /* Unlike NVK's host-RM API path, this direct GSP-RM RPC must carry the
       physical descriptors consumed by _kchannelDescribeMemDescsFromParams. */
    chan.instanceMem.base=instPhys;
    chan.instanceMem.size=0x1000;
    chan.instanceMem.addressSpace=1; /* ADDR_SYSMEM */
    chan.instanceMem.cacheAttrib=0;  /* coherent system memory */
    chan.userdMem.base=userdPhys;
    chan.userdMem.size=0x200;
    chan.userdMem.addressSpace=1;
    chan.userdMem.cacheAttrib=0;
    /* RM creates separate descriptors for instance and RAMFC backing. Keep
       them disjoint; aliasing RAMFC over the instance block can corrupt saved
       channel pointers after context switches under sustained submissions. */
    chan.ramfcMem.base=ramfcPhys;
    chan.ramfcMem.size=0x200;
    chan.ramfcMem.addressSpace=1;
    chan.ramfcMem.cacheAttrib=0;
    chan.mthdbufMem.base=methodPhys;
    chan.mthdbufMem.size=0x2000;
    chan.mthdbufMem.addressSpace=1;
    chan.mthdbufMem.cacheAttrib=0;

    if(!NvxGspAlloc(dev,tsgHandle,channelHandle,AMPERE_CHANNEL_GPFIFO_A,&chan,sizeof(chan))) {
        LogStr("NVX [GR]: graphics GPFIFO channel allocation failed.\n");
        return FALSE;
    }
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    LogStr("NVX [GR]: channel allocated with RM-owned context memory and USERD handle.\n");
    /* Create the GR object before binding/scheduling the runlist, matching
       OpenRM's kernel_graphics bring-up order. This lets RM attach its GR
       engine context before the channel becomes runnable. */
    if(!NvxGspAlloc(dev,channelHandle,grHandle,grClass,NULL,0)) {
        LogStr("NVX [GR]: Ampere graphics object allocation failed; class=");LogHex(grClass);LogStr("\n");
        return FALSE;
    }
    if(NvxGspAlloc(dev,channelHandle,NVX_RM_OBJECT_NVC0_M2MF,
                   NVX_RM_CLASS_KEPLER_I2M_B,NULL,0)) {
        dev->GspGraphicsM2mfObjectHandle=NVX_RM_OBJECT_NVC0_M2MF;
        LogStr("NVX [GR]: RM P2MF/I2M-B object allocated on graphics channel.\n");
    } else {
        LogStr("NVX [GR]: RM P2MF/I2M-B allocation unavailable.\n");
    }
    if(NvxGspAlloc(dev,channelHandle,NVX_RM_OBJECT_NVC0_COPY,
                   NVX_RM_CLASS_AMPERE_DMA_COPY_B,NULL,0)) {
        dev->GspGraphicsCopyObjectHandle=NVX_RM_OBJECT_NVC0_COPY;
        LogStr("NVX [GR]: Ampere copy object allocated on graphics channel.\n");
    } else {
        LogStr("NVX [GR]: Ampere copy object allocation unavailable.\n");
    }
    if(NvxGspAlloc(dev,channelHandle,NVX_RM_OBJECT_NVC0_2D,
                   NVX_RM_CLASS_FERMI_2D_A,NULL,0)) {
        dev->GspGraphics2dObjectHandle=NVX_RM_OBJECT_NVC0_2D;
        LogStr("NVX [GR]: Fermi 2D object allocated on graphics channel.\n");
    } else {
        LogStr("NVX [GR]: Fermi 2D object allocation unavailable.\n");
    }
    if(NvxGspAlloc(dev,channelHandle,NVX_RM_OBJECT_NVC0_COMPUTE,
                   NVX_RM_CLASS_AMPERE_COMPUTE_B,NULL,0)) {
        dev->GspGraphicsComputeObjectHandle=NVX_RM_OBJECT_NVC0_COMPUTE;
        LogStr("NVX [GR]: Ampere compute object allocated on graphics channel.\n");
    } else {
        LogStr("NVX [GR]: Ampere compute object allocation unavailable.\n");
    }
    if(!NvxGspControl(dev,channelHandle,NVA06F_CTRL_CMD_BIND,&bindEngine,sizeof(bindEngine))) {
        LogStr("NVX [GR]: graphics channel bind failed.\n");
        return FALSE;
    }
    if(!NvxGspControl(dev,channelHandle,NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                      schedEnable,sizeof(schedEnable))) {
        LogStr("NVX [GR]: graphics channel scheduling failed.\n");
        return FALSE;
    }
    LogStr("NVX [GR]: GR object bound and channel scheduled.\n");
    /* Needed to size an NVIDIA-compatible non-replayable fault shadow queue;
       this read-only query does not register a buffer or service faults. */
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,
                     NV2080_CTRL_CMD_INTERNAL_GMMU_GET_STATIC_INFO,
                     &faultStatic,sizeof(faultStatic))) {
        const NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS *reply=
            (const NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS *)(dev->RpcRxRecord+104);
        LogStr("NVX [GR-FAULT]: nonreplay bytes=");LogHex(reply->nonReplayableFaultBufferSize);
        LogStr(" metadata=");LogHex(reply->nonReplayableShadowFaultBufferMetadataSize);
        LogStr(" replay bytes=");LogHex(reply->replayableFaultBufferSize);
        LogStr(" metadata=");LogHex(reply->replayableShadowFaultBufferMetadataSize);LogStr("\n");
#if NVX_ENABLE_GR_SUBMISSION_PROBE
        if(!NvxGspSetupFaultBuffer(dev,reply->nonReplayableFaultBufferSize,
                                   reply->nonReplayableShadowFaultBufferMetadataSize,
                                   faultBufferClass)) {
            LogStr("NVX [GR-FAULT]: GR submit probe stopped because fault reporting is unavailable.\n");
            return FALSE;
        }
#endif
    } else {
        LogStr("NVX [GR-FAULT]: static fault-buffer requirements unavailable.\n");
#if NVX_ENABLE_GR_SUBMISSION_PROBE
        return FALSE;
#endif
    }
    /* This is the same physical-RM query used by OpenRM to learn the
       required GR buffer sizes before client RM allocates them. Reuse the
       device-extension storage to avoid a large VideoPort kernel stack frame. */
    _Static_assert(sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_PARAMS)
                   <= sizeof(dev->GrContextInfoParams), "GR query storage");
    VideoPortZeroMemory(grBufferInfo,
        sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_PARAMS));
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,
                     NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO,
                     grBufferInfo,
                     sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_PARAMS))) {
        const NV2080_CTRL_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_PARAMS *reply=
            (const NV2080_CTRL_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_PARAMS *)
                (dev->RpcRxRecord+104);
        ULONG i;
        for(i=0;i<NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT;i++) {
            grRequirements[i].size=reply->engineContextBuffersInfo[0].engine[i].size;
            grRequirements[i].alignment=reply->engineContextBuffersInfo[0].engine[i].alignment;
            LogStr("NVX [GR]: static buffer index=");LogHex(i);
            LogStr(" size=");LogHex(grRequirements[i].size);
            LogStr(" alignment=");LogHex(grRequirements[i].alignment);
            LogStr("\n");
        }
        grRequirementsValid=TRUE;
    } else {
        LogStr("NVX [GR]: static GR context-buffer requirements unavailable.\n");
    }
    /* NVIDIA's main-context allocation adds one 4KB header per reported
       subcontext when physical RM advertises per-subcontext headers. */
    VideoPortZeroMemory(grBufferInfo,
        sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_PDB_PROPERTIES_PARAMS));
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,
                     NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_PDB_PROPERTIES,
                     grBufferInfo,
                     sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_PDB_PROPERTIES_PARAMS))) {
        const NV2080_CTRL_INTERNAL_STATIC_KGR_GET_PDB_PROPERTIES_PARAMS *reply=
            (const NV2080_CTRL_INTERNAL_STATIC_KGR_GET_PDB_PROPERTIES_PARAMS *)(dev->RpcRxRecord+104);
        grHeaderSupported=reply->pdbTable[0].bPerSubCtxheaderSupported;
        if(grHeaderSupported<=1) grHeaderInfoValid=TRUE;
    }
    if(grHeaderInfoValid&&grHeaderSupported) {
        _Static_assert(sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS)
                       <= sizeof(dev->GrContextInfoParams), "GR info storage");
        VideoPortZeroMemory(grBufferInfo,sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS));
        if(NvxGspControl(dev,dev->GspSubdeviceHandle,
                         NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_INFO,
                         grBufferInfo,sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS))) {
            const NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS *reply=
                (const NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS *)(dev->RpcRxRecord+104);
            grMaxSubcontexts=reply->engineInfo[0].infoList[NV2080_CTRL_GR_INFO_INDEX_MAX_SUBCONTEXT_COUNT].data;
        } else grHeaderInfoValid=FALSE;
    }
    LogStr("NVX [GR]: per-subcontext headers=");LogHex(grHeaderSupported);
    LogStr(" maxSubcontexts=");LogHex(grMaxSubcontexts);
    LogStr(" propertiesValid=");LogHex(grHeaderInfoValid?1:0);LogStr("\n");
    if(grRequirementsValid&&grHeaderInfoValid&&
       NvxGrBuildLayout(grRequirements,grMaxSubcontexts,(int)grHeaderSupported,
                        GSP_GR_CONTEXT_BYTES,&grLayout)) {
        ULONG i;
        LogStr("NVX [GR]: checked context layout bytes=");LogHex(grLayout.bytes);
        LogStr(" buffers=");LogHex(grLayout.count);LogStr("\n");
        for(i=0;i<grLayout.count;i++) {
            LogStr("NVX [GR]: layout id=");LogHex(grLayout.buffers[i].id);
            LogStr(" offset=");LogHex(grLayout.buffers[i].offset);
            LogStr(" size=");LogHex(grLayout.buffers[i].size);
            LogStr(" align=");LogHex(grLayout.buffers[i].alignment);LogStr("\n");
        }
#if NVX_ENABLE_GR_PROMOTE_PROBE
        /* External VASpaces require client RM to initialize physical context
           buffers before UVM binds their VAs. The alias test verified backing
           access, but does not validate these protected GR addresses. */
        if(dev->GspGrContextAliasVerified&&grLayout.count&&grLayout.buffers[0].id==0&&
           grLayout.buffers[0].offset<=dev->GspGrContextLength&&
           grLayout.buffers[0].size<=dev->GspGrContextLength-grLayout.buffers[0].offset) {
            NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS *promote=
                (NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS *)grBufferInfo;
            ULONG changed=0,initCount=0;
            BOOLEAN valid=TRUE;
            _Static_assert(sizeof(NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS)
                           <=sizeof(dev->GrContextInfoParams), "GR promote storage");
            VideoPortZeroMemory(dev->GspGrContextVirtual,dev->GspGrContextLength);
            VideoPortZeroMemory(promote,sizeof(*promote));
            promote->engineType=NV2080_ENGINE_TYPE_GRAPHICS;
            promote->hChanClient=dev->GspClientHandle;
            promote->hObject=channelHandle;
            for(i=0;i<grLayout.count;i++) {
                ULONG id=grLayout.buffers[i].id;
                NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY *entry;
                if(grLayout.buffers[i].offset>dev->GspGrContextLength||
                   grLayout.buffers[i].size>dev->GspGrContextLength-grLayout.buffers[i].offset) {
                    valid=FALSE;break;
                }
                /* Matches kgrctxPrepareInitializeCtxBuffer's initialized IDs. */
                if(id!=0&&id!=1&&id!=2&&id!=8&&id!=9&&id!=10&&id!=11) continue;
                entry=&promote->promoteEntry[initCount++];
                entry->bufferId=(NvU16)id;
                entry->gpuPhysAddr=(ULONG64)dev->GspGrContextPhysical.QuadPart+grLayout.buffers[i].offset;
                entry->size=grLayout.buffers[i].size;
                entry->physAttr=DRF_DEF(2080,_CTRL_GPU_INITIALIZE_CTX,_APERTURE,_NCOH_SYS)|
                                DRF_DEF(2080,_CTRL_GPU_INITIALIZE_CTX,_GPU_CACHEABLE,_NO);
                entry->bInitialize=TRUE;entry->bNonmapped=TRUE;
            }
            promote->entryCount=initCount;
            __sync_synchronize();
            LogStr("NVX [GR-PROMOTE]: requesting physical initialization entries=");
            LogHex(initCount);LogStr("\n");
            if(valid&&initCount&&NvxGspControl(dev,dev->GspSubdeviceHandle,
                            NV2080_CTRL_CMD_GPU_PROMOTE_CTX,promote,sizeof(*promote))) {
                volatile UCHAR *bytes=(volatile UCHAR *)dev->GspGrContextVirtual+
                                       grLayout.buffers[0].offset;
                __sync_synchronize();
                for(ULONG64 n=0;n<grLayout.buffers[0].size;n++) if(bytes[n]) changed++;
                LogStr("NVX [GR-PROMOTE]: physical initialization returned NV_OK; MAIN nonzero bytes=");
                LogHex(changed);LogStr("\n");
                /* Match nvGpuOpsBindChannelResources: VA-only entries, no
                   repeated physical initialization, one per context buffer. */
                VideoPortZeroMemory(promote,sizeof(*promote));
                promote->engineType=NV2080_ENGINE_TYPE_GRAPHICS;
                promote->hChanClient=dev->GspClientHandle;
                promote->hObject=channelHandle;
                promote->entryCount=grLayout.count;
                for(i=0;i<grLayout.count;i++) {
                    promote->promoteEntry[i].bufferId=(NvU16)grLayout.buffers[i].id;
                    promote->promoteEntry[i].gpuVirtAddr=GSP_GR_CONTEXT_GPU_VA+grLayout.buffers[i].offset;
                }
                if(NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
                                 promote,sizeof(*promote))) {
                    dev->GspGrPromotionAccepted=TRUE;
                    LogStr("NVX [GR-PROMOTE]: RM accepted VA bindings; no GR execution verified.\n");
                } else LogStr("NVX [GR-PROMOTE]: VA bindings rejected or unavailable.\n");
            } else LogStr("NVX [GR-PROMOTE]: physical initialization rejected or unavailable.\n");
        } else LogStr("NVX [GR-PROMOTE]: verified backing unavailable; initialization skipped.\n");
#endif
    } else LogStr("NVX [GR]: context layout unavailable or exceeds 12MB; no memory promoted.\n");
#if !NVX_ENABLE_GR_SUBMISSION_PROBE
    LogStr("NVX [GR]: shader triangle probe disabled; running bounded C797 clear validation.\n");
#endif
    if(!dev->GspGrPromotionAccepted||!dev->GspGrContextAliasVerified||
       dev->GspMmuFaultQueued) {
        LogStr("NVX [GR]: context promotion or backing unverified; no GR work submitted.\n");
        return FALSE;
    }
    /* Query RM's own view of context preparation and channel scheduling before
       attempting any packet. These inherited B06F controls are diagnostic;
       they do not synthesize or overwrite GR context state. */
    grCtxSize.engineID=bindEngine;
    if(NvxGspControl(dev,channelHandle,NVB06F_CTRL_CMD_GET_ENGINE_CTX_SIZE,
                     &grCtxSize,sizeof(grCtxSize))) {
        const NVB06F_CTRL_GET_ENGINE_CTX_SIZE_PARAMS *reply=
            (const NVB06F_CTRL_GET_ENGINE_CTX_SIZE_PARAMS *)(dev->RpcRxRecord+104);
        LogStr("NVX [GR]: RM engine context size=");LogHex(reply->size);LogStr(" engine=");LogHex(reply->engineID);LogStr("\n");
    } else {
        LogStr("NVX [GR]: RM engine context size query unavailable.\n");
    }
    if(NvxGspControl(dev,channelHandle,NVB06F_CTRL_CMD_GET_CHANNEL_HW_STATE,
                     &grHwState,sizeof(grHwState))) {
        const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *reply=
            (const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *)(dev->RpcRxRecord+104);
        LogStr("NVX [GR]: RM channel hardware state=");LogHex(reply->state);LogStr("\n");
    } else {
        LogStr("NVX [GR]: RM channel hardware state query unavailable.\n");
    }
    /* The size-only control lacks a client handle and is rejected by the
       firmware resource namespace. Use OpenRM's full info query, which takes
       both the owning client and channel and returns actual buffer metadata. */
    VideoPortZeroMemory(grBufferInfo,sizeof(*grBufferInfo));
    grBufferInfo->hUserClient=dev->GspClientHandle;
    grBufferInfo->hChannel=channelHandle;
    LogStr("NVX [GR]: querying context buffers for userClient=");
    LogHex(grBufferInfo->hUserClient);LogStr(" channel=");
    LogHex(grBufferInfo->hChannel);LogStr("\n");
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO,
                     grBufferInfo,sizeof(*grBufferInfo))) {
        const NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS *reply=
            (const NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS *)(dev->RpcRxRecord+104);
        ULONG infoIndex,infoCount=reply->bufferCount;
        if(infoCount>NV2080_CTRL_GR_MAX_CTX_BUFFER_COUNT)
            infoCount=NV2080_CTRL_GR_MAX_CTX_BUFFER_COUNT;
        LogStr("NVX [GR]: RM context-buffer count=");LogHex(reply->bufferCount);LogStr("\n");
        for(infoIndex=0;infoIndex<infoCount&&infoIndex<12;infoIndex++) {
            LogStr("NVX [GR]: ctxbuf index=");LogHex(infoIndex);
            LogStr(" type=");LogHex(reply->ctxBufferInfo[infoIndex].bufferType);
            LogStr(" size=");LogHex((ULONG)reply->ctxBufferInfo[infoIndex].size);
            LogStr(" aperture=");LogHex(reply->ctxBufferInfo[infoIndex].aperture);LogStr("\n");
        }
    } else {
        LogStr("NVX [GR]: RM context-buffer lookup failed for direct graphics channel.\n");
    }

    /* SET_OBJECT's payload is the RM-encoded classEngineID, not the object
       handle or the class ID alone. Ask RM for the exact opaque encoding. */
    classEngineParams.hObject=grHandle;
    if(!NvxGspControl(dev,channelHandle,NVA06F_CTRL_GET_CLASS_ENGINEID,
                      &classEngineParams,sizeof(classEngineParams))) {
        LogStr("NVX [GR]: GET_CLASS_ENGINEID failed; refusing to guess SET_OBJECT data.\n");
        return FALSE;
    }
    {
        const NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS *reply=
            (const NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS *)(dev->RpcRxRecord+104);
        classEngineId=reply->classEngineID;
        dev->GspGraphicsObjectClassEngineId=classEngineId;
        LogStr("NVX [GR]: object classEngineID=");LogHex(reply->classEngineID);
        LogStr(" classID=");LogHex(reply->classID);
        LogStr(" engineID=");LogHex(reply->engineID);LogStr("\n");
    }
    if(dev->GspGraphicsM2mfObjectHandle&&
       !NvxGspGetClassEngineId(dev,channelHandle,
            dev->GspGraphicsM2mfObjectHandle,
            &dev->GspGraphicsM2mfClassEngineId)) {
        LogStr("NVX [GR]: M2MF classEngineID lookup failed.\n");
    }
    if(dev->GspGraphicsCopyObjectHandle&&
       !NvxGspGetClassEngineId(dev,channelHandle,
            dev->GspGraphicsCopyObjectHandle,
            &dev->GspGraphicsCopyClassEngineId)) {
        LogStr("NVX [GR]: copy classEngineID lookup failed.\n");
    }
    if(dev->GspGraphics2dObjectHandle&&
       !NvxGspGetClassEngineId(dev,channelHandle,
            dev->GspGraphics2dObjectHandle,
            &dev->GspGraphics2dClassEngineId)) {
        LogStr("NVX [GR]: 2D classEngineID lookup failed.\n");
    }
    if(dev->GspGraphicsComputeObjectHandle&&
       !NvxGspGetClassEngineId(dev,channelHandle,
            dev->GspGraphicsComputeObjectHandle,
            &dev->GspGraphicsComputeClassEngineId)) {
        LogStr("NVX [GR]: compute classEngineID lookup failed.\n");
    }

    /* Prove the GR runlist consumes a harmless SET_OBJECT packet. This tests
       command fetch/dispatch only; it does not draw or touch scanout. */
    runlistQuery[0]=NV2080_ENGINE_TYPE_GRAPHICS;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,
                      NV2080_CTRL_CMD_GPU_GET_ENGINE_RUNLIST_PRI_BASE,
                      runlistQuery,sizeof(runlistQuery))) {
        LogStr("NVX [GR]: graphics runlist query failed; channel remains isolated.\n");
        return FALSE;
    }
    runlistBase=((const ULONG *)dev->RpcRxRecord)[26+62];
    if(dev->Bar0Length<0x304||!runlistBase||(runlistBase&0x3ff)||
       runlistBase>dev->Bar0Length-0x304) {
        LogStr("NVX [GR]: invalid graphics runlist base=");LogHex(runlistBase);LogStr("\n");
        return FALSE;
    }
    dbcfg=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+8));
    chcfg=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+4));
    enable=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x300));
    LogStr("NVX [GR]: runlist=");LogHex(runlistBase);LogStr(" dbcfg=");LogHex(dbcfg);
    LogStr(" chcfg=");LogHex(chcfg);LogStr(" enable=");LogHex(enable);LogStr("\n");
    if(dbcfg==0xffffffffU||(dbcfg&0xffff0000U)==0xbadf0000U) {
        LogStr("NVX [GR]: runlist doorbell configuration invalid.\n");
        return FALSE;
    }
    /* This GSP path rejects GET_WORK_SUBMIT_TOKEN. Use the same isolated
       internal runlist doorbell as the earlier probe, and drain notifications
       with a read-only name query instead of the rejected control. */
    if(!NvxGspGetName(dev)) return FALSE;
    assertCountBefore=dev->GspAssertCount;

    userd=(volatile ULONG *)(scratch+0xb400);
    fifo=(volatile ULONG64 *)(scratch+0x9000);
    push=(PULONG)(scratch+0xf000);
    pushVa=dev->GspScratchGpuVa+0xf000;
    /* Keep the semaphore outside USERD: the RM-defined 512-byte USERD block
       occupies scratch +0xb400..+0xb5ff. */
    grFence=(volatile ULONG *)(scratch+0x5000);
    grFenceVa=dev->GspScratchGpuVa+0x5000;
    grData=(volatile ULONG *)(scratch+0xb600);
    grDataVa=dev->GspScratchGpuVa+0xb600;
    /* The spare final scratch page is a CE readback destination after the
       GR draw is fenced. It is outside the FIFO, USERD, semaphore and target. */
    grReadback=(volatile ULONG *)(scratch+0xe000);
    /* The GR context VA is promoted, but its external semaphore access remains
       unverified. Use this already-mapped coherent scratch word to isolate the
       target mapping from the report-semaphore method itself. */
    grFence[0]=0;grFence[1]=0x13579bdf;grFence[2]=0x2468ace0;
    VideoPortZeroMemory((PVOID)grData,64);
    if(userd[0x88/4]||userd[0x8c/4]) {
        LogStr("NVX [GR]: fresh graphics USERD is not empty; submission skipped.\n");
        return FALSE;
    }
    VideoPortZeroMemory((PVOID)(scratch+0x9000),0x1000);
    VideoPortZeroMemory((PVOID)(scratch+0xf000),0x1000);
#if NVX_ENABLE_GR_SUBMISSION_PROBE
    /* Scratch pages 0x7000/0x8000 are mapped by the verified GR scratch VA;
       keep test resources out of the RM-owned promoted-context buffers. */
    grShaderOffset=0x8000;
    if(grShaderOffset>GSP_SCRATCH_SIZE-0x1000) {
        LogStr("NVX [GR]: mapped scratch has no room for triangle resources.\n");
        return FALSE;
    }
    grShaderVa=dev->GspScratchGpuVa+grShaderOffset;
    grVertexCbVa=grShaderVa+0x600;
    grVertexVa=grShaderVa+0x800;
    grTextureVa=grShaderVa+0x900;
    grBindlessVa=grShaderVa+0xa00;
    grTextureDataVa=dev->GspScratchGpuVa+0x7000;
    grVertexCb=(PULONG)(scratch+grShaderOffset+0x600);
    grVertex=(PULONG)(scratch+grShaderOffset+0x800);
    grTexture=(PULONG)(scratch+grShaderOffset+0x900);
    grBindless=(PULONG)(scratch+grShaderOffset+0xa00);
    VideoPortZeroMemory(scratch+grShaderOffset,0x1000);
    VideoPortMoveMemory(scratch+grShaderOffset,
                        (PVOID)NvxAmpereHeadSurfacePrograms,1408);
    VideoPortMoveMemory(grVertexCb,(PVOID)grVertexUniforms,sizeof(grVertexUniforms));
    VideoPortMoveMemory(grVertex,(PVOID)grTriangleVertices,sizeof(grTriangleVertices));
    grBindless[0]=0; /* Primary texture -> header 0. */
    grBindless[1]=255;grBindless[2]=255;grBindless[3]=255;
    grBindless[4]=255;grBindless[5]=255;
    NvxGspBuildProbeTexture(grTexture,grTexture+8,grTextureDataVa);
    ((PULONG)(scratch+0x7000))[0]=0xff00ff00U;
    __sync_synchronize();
#endif
    /* In NVIDIA Host/PBDMA DMA format, method addresses in the packet header
       are dword-addressed (method byte offset >> 2), as seen in NVIDIA's
       __nvPushStart / NVA16F_DMA_METHOD_ADDRESS and nvx_ce.c.
       Previous probes passed raw byte offsets, which truncated/misaddressed
       methods (e.g. 0x1b00/0x1b04 masked to 0xb00/0xb04, causing assertion 0x15BD50C).
       Encode methods properly dword-addressed:
       - SET_OBJECT (0x0000 >> 2 = 0x000)
       - LINE_LENGTH_IN (0x0180 >> 2 = 0x060), 5 words:
           LINE_LENGTH_IN (64), LINE_COUNT (1), OFFSET_OUT_UPPER, OFFSET_OUT, PITCH_OUT (64)
       - LAUNCH_DMA (0x01b0 >> 2 = 0x06c), ONE_INC opcode (5), 17 words:
           LAUNCH_DMA config, followed by 16 inline data dwords to LOAD_INLINE_DATA
       - WAIT_FOR_IDLE (0x0110 >> 2 = 0x044)
       - SET_REPORT_SEMAPHORE_A (0x1b00 >> 2 = 0x6c0), 4 words A/B/C/D */
    {
        ULONG n=0,i;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC56F_SET_OBJECT>>2);
        push[n++]=classEngineId;
        /* Configure I2M destination buffer (64 bytes = 16 dwords) */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(5U<<16)|(NVC797_LINE_LENGTH_IN>>2);
        push[n++]=64;                                    /* LINE_LENGTH_IN: 64 bytes */
        push[n++]=1;                                     /* LINE_COUNT: 1 */
        push[n++]=(ULONG)(grDataVa>>32);                 /* OFFSET_OUT_UPPER */
        push[n++]=(ULONG)grDataVa;                       /* OFFSET_OUT */
        push[n++]=64;                                    /* PITCH_OUT: 64 bytes */
        /* LAUNCH_DMA using ONE_INC (opcode 5) so subsequent data streams into LOAD_INLINE_DATA (0x1b4) */
        push[n++]=(NVC56F_DMA_ONEINCR_OPCODE_VALUE<<29)|(17U<<16)|(NVC797_LAUNCH_DMA>>2);
        push[n++]=DRF_DEF(C797,_LAUNCH_DMA,_DST_MEMORY_LAYOUT,_PITCH)|
                  DRF_DEF(C797,_LAUNCH_DMA,_COMPLETION_TYPE,_FLUSH_DISABLE)|
                  DRF_DEF(C797,_LAUNCH_DMA,_INTERRUPT_TYPE,_NONE)|
                  DRF_DEF(C797,_LAUNCH_DMA,_SYSMEMBAR_DISABLE,_TRUE);
        for(i=0;i<16;i++) {
            push[n++]=0x67723000U+i;
        }

        /* Test SET_COLOR_TARGET_FORMAT(0) -> DISABLED */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_COLOR_TARGET_FORMAT(0)>>2);
        push[n++]=NVC797_SET_COLOR_TARGET_FORMAT_V_DISABLED;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_ZT_SELECT>>2);
        push[n++]=0; /* target count = 0 (no depth/stencil target) */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_VERTEX_STREAM_SUBSTITUTE_A>>2);
        push[n++]=(ULONG)(grDataVa>>32);
        push[n++]=(ULONG)grDataVa;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_POINT_CENTER_MODE>>2);
        push[n++]=NVC797_SET_POINT_CENTER_MODE_V_OGL;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_SAMPLER_BINDING>>2);
        push[n++]=NVC797_SET_SAMPLER_BINDING_V_VIA_HEADER_BINDING;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_VIEWPORT_SCALE_OFFSET>>2);
        push[n++]=0; /* ENABLE = FALSE */

        /* Viewport clip for 16 viewports (each 2 words: HORIZONTAL, VERTICAL) */
        for(i=0;i<16;i++) {
            push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_VIEWPORT_CLIP_HORIZONTAL(i)>>2);
            push[n++]=(32768U<<16); /* X0=0, WIDTH=32768 */
            push[n++]=(32768U<<16); /* Y0=0, HEIGHT=32768 */
        }

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_PROVOKING_VERTEX>>2);
        push[n++]=NVC797_SET_PROVOKING_VERTEX_V_LAST;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_SINGLE_ROP_CONTROL>>2);
        push[n++]=1; /* enable single ROP control */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_SINGLE_CT_WRITE_CONTROL>>2);
        push[n++]=1; /* enable single CT write control */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_BLEND(0)>>2);
        push[n++]=1; /* enable blend on target 0 */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(4U<<16)|(NVC797_SET_BLEND_CONST_RED>>2);
        push[n++]=0x3f800000U; /* 1.0f red */
        push[n++]=0x3f800000U; /* 1.0f green */
        push[n++]=0x3f800000U; /* 1.0f blue */
        push[n++]=0x3f800000U; /* 1.0f alpha */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_BLEND_SEPARATE_FOR_ALPHA>>2);
        push[n++]=0; /* ENABLE = FALSE */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|(NVC797_SET_BLEND_COLOR_OP>>2);
        push[n++]=NVC797_SET_BLEND_COLOR_OP_V_OGL_FUNC_ADD;
        push[n++]=NVC797_SET_BLEND_COLOR_SOURCE_COEFF_V_OGL_ONE;
        push[n++]=NVC797_SET_BLEND_COLOR_DEST_COEFF_V_OGL_ZERO;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_LINE_WIDTH_FLOAT>>2);
        push[n++]=0x3f800000U; /* 1.0f line width */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_LINE_MULTISAMPLE_OVERRIDE>>2);
        push[n++]=0; /* ENABLE = FALSE */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_WINDOW_ORIGIN>>2);
        push[n++]=DRF_DEF(C797,_SET_WINDOW_ORIGIN,_MODE,_UPPER_LEFT)|
                  DRF_DEF(C797,_SET_WINDOW_ORIGIN,_FLIP_Y,_TRUE);

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_ZCULL_BOUNDS>>2);
        push[n++]=DRF_DEF(C797,_SET_ZCULL_BOUNDS,_Z_MIN_UNBOUNDED_ENABLE,_FALSE)|
                  DRF_DEF(C797,_SET_ZCULL_BOUNDS,_Z_MAX_UNBOUNDED_ENABLE,_FALSE);

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_SHADER_EXCEPTIONS>>2);
        push[n++]=0; /* ENABLE = FALSE */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_GO_IDLE_TIMEOUT>>2);
        push[n++]=0x800;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_SPH_VERSION>>2);
        push[n++]=(4U)|(4U<<16); /* CURRENT=4, OLDEST=4 */
        push[n++]=(4U)|(4U<<16); /* CHECK CURRENT=4, OLDEST=4 */

        /* Color target 0 binding: 64x16 ARGB8888 blocklinear (1 GOB = 16 lines) */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_CT_SELECT>>2);
        push[n++]=1; /* target count = 1, target 0 = colorTarget 0 */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(6U<<16)|(NVC797_SET_COLOR_TARGET_A(0)>>2);
        push[n++]=(ULONG)(grDataVa>>32);                                /* A: upper address */
        push[n++]=(ULONG)grDataVa;                                      /* B: lower address */
        push[n++]=64;                                                   /* WIDTH: 64 pixels */
        push[n++]=16;                                                   /* HEIGHT: 16 lines (1 GOB) */
        push[n++]=NVC797_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8;            /* FORMAT: 0xCF (ARGB8888) */
        push[n++]=0;                                                    /* MEMORY: BLOCKLINEAR, 1 GOB height/depth/width */

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_SURFACE_CLIP_HORIZONTAL>>2);
        push[n++]=(64U<<16);                                            /* X=0, WIDTH=64 */
        push[n++]=(16U<<16);                                            /* Y=0, HEIGHT=16 */

        /* Clear to white so a black shader result still proves rasterization. */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(4U<<16)|(NVC797_SET_COLOR_CLEAR_VALUE(0)>>2);
#if NVX_ENABLE_GR_SUBMISSION_PROBE
        push[n++]=0x3f800000U;
        push[n++]=0x3f800000U;
        push[n++]=0x3f800000U;
        push[n++]=0x3f800000U;
#else
        push[n++]=0;
        push[n++]=0;
        push[n++]=0;
        push[n++]=0;
#endif

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_CLEAR_RECT_HORIZONTAL>>2);
        push[n++]=(64U<<16);                                            /* XMIN=0, XMAX=64 */
        push[n++]=(16U<<16);                                            /* YMIN=0, YMAX=16 */

        /* CLEAR_SURFACE: trigger hardware clear of color buffer (RGBA) */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_CLEAR_SURFACE>>2);
        push[n++]=DRF_DEF(C797,_CLEAR_SURFACE,_R_ENABLE,_TRUE)|
                  DRF_DEF(C797,_CLEAR_SURFACE,_G_ENABLE,_TRUE)|
                  DRF_DEF(C797,_CLEAR_SURFACE,_B_ENABLE,_TRUE)|
                  DRF_DEF(C797,_CLEAR_SURFACE,_A_ENABLE,_TRUE);

#if NVX_ENABLE_GR_SUBMISSION_PROBE
        /* Render one triangle using NVIDIA's known-good Ampere head-surface
           vertex and fragment programs. The source is a one-pixel green
           block-linear texture, so the readback can identify rasterized pixels. */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|(NVC797_SET_TEX_SAMPLER_POOL_A>>2);
        push[n++]=(ULONG)(grTextureVa>>32);
        push[n++]=(ULONG)grTextureVa;
        push[n++]=0; /* sampler pool maximum index */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|(NVC797_SET_TEX_HEADER_POOL_A>>2);
        push[n++]=(ULONG)((grTextureVa+32)>>32);
        push[n++]=(ULONG)(grTextureVa+32);
        push[n++]=0; /* header pool maximum index */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_SAMPLER_BINDING>>2);
        push[n++]=NVC797_SET_SAMPLER_BINDING_V_VIA_HEADER_BINDING;

        /* Bind Ampere vertex-B and pixel programs from the first 0x580 bytes
           of the upstream shader blob. */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_PIPELINE_SHADER(1)>>2);
        push[n++]=DRF_DEF(C797,_SET_PIPELINE_SHADER,_ENABLE,_TRUE)|
                  DRF_DEF(C797,_SET_PIPELINE_SHADER,_TYPE,_VERTEX);
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_PIPELINE_REGISTER_COUNT(1)>>2);
        push[n++]=15; push[n++]=0; /* vertex-B registers, vertex bind group */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_PIPELINE_PROGRAM_ADDRESS_A(1)>>2);
        push[n++]=(ULONG)(grShaderVa>>32); push[n++]=(ULONG)grShaderVa;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_PIPELINE_SHADER(5)>>2);
        push[n++]=DRF_DEF(C797,_SET_PIPELINE_SHADER,_ENABLE,_TRUE)|
                  DRF_DEF(C797,_SET_PIPELINE_SHADER,_TYPE,_PIXEL);
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_PIPELINE_REGISTER_COUNT(5)>>2);
        push[n++]=13; push[n++]=4; /* pixel registers, fragment bind group */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_PIPELINE_PROGRAM_ADDRESS_A(5)>>2);
        push[n++]=(ULONG)(grShaderVa>>32); push[n++]=(ULONG)(grShaderVa+0x300);
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_BIND_GROUP_CONSTANT_BUFFER(0)>>2);
        push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_FALSE)|
                  DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,1);
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_BIND_GROUP_CONSTANT_BUFFER(4)>>2);
        push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_FALSE)|
                  DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,1);

        /* Direct, coherent shader constant buffers: VS uniforms at slot 3 and
           the texture binding table at fragment slot 4. */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|(NVC797_SET_CONSTANT_BUFFER_SELECTOR_A>>2);
        push[n++]=256; push[n++]=(ULONG)(grVertexCbVa>>32); push[n++]=(ULONG)grVertexCbVa;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_BIND_GROUP_CONSTANT_BUFFER(0)>>2);
        push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_TRUE)|
                  DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,3);
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|(NVC797_SET_CONSTANT_BUFFER_SELECTOR_A>>2);
        push[n++]=256; push[n++]=(ULONG)(grBindlessVa>>32); push[n++]=(ULONG)grBindlessVa;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_BIND_GROUP_CONSTANT_BUFFER(4)>>2);
        push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_TRUE)|
                  DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,4);

        /* POSITION (attribute 0) and TEXCOORD0 (attribute 8) share a dynamic
           stream containing three XYUVRQ vertices. */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_DA_OUTPUT_ATTRIBUTE_SKIP_MASK_A(0)>>2);
        push[n++]=0xfffffff0U; push[n++]=0xfffffff0U;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_VERTEX_ATTRIBUTE_A(0)>>2);
        push[n++]=1U|(4U<<21)|(7U<<27); /* dynamic stream, R32G32 float */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_SET_VERTEX_ATTRIBUTE_A(8)>>2);
        push[n++]=1U|(8U<<7)|(1U<<21)|(7U<<27); /* offset 8, R32G32B32A32 float */
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|(NVC797_SET_VERTEX_STREAM_A_FORMAT(1)>>2);
        push[n++]=DRF_NUM(C797,_SET_VERTEX_STREAM_A_FORMAT,_STRIDE,24)|
                  DRF_DEF(C797,_SET_VERTEX_STREAM_A_FORMAT,_ENABLE,_TRUE);
        push[n++]=(ULONG)(grVertexVa>>32); push[n++]=(ULONG)grVertexVa;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_VERTEX_STREAM_SIZE_A(1)>>2);
        push[n++]=0; push[n++]=72;

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_OGL_SET_CULL>>2);
        push[n++]=NVC797_OGL_SET_CULL_ENABLE_FALSE;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_BEGIN>>2);
        push[n++]=NVC797_BEGIN_OP_TRIANGLES;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(2U<<16)|(NVC797_SET_VERTEX_ARRAY_START>>2);
        push[n++]=0; push[n++]=3;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_END>>2);
        push[n++]=0;
#endif

        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|(NVC797_WAIT_FOR_IDLE>>2);
        push[n++]=0;
        push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(4U<<16)|(NVC797_SET_REPORT_SEMAPHORE_A>>2);
        push[n++]=(ULONG)(grFenceVa>>32);
        push[n++]=(ULONG)grFenceVa;
        push[n++]=0x47524632;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
        fifo[0]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    }
    __sync_synchronize();
    userd[0x8c/4]=1;
    dev->GspGraphicsGPPut=1;
    __sync_synchronize();
    /* USERD is allocated at fixed index 2 above.  On GA104 the scheduler
       exposes that graphics channel in CHRAM slot 2; ring that channel's
       doorbell rather than borrowing channel 1 (the independent CE channel).
       Keep RM's opaque token authoritative whenever it supplies one. */
    token=(dbcfg&0xffff0000U)|2U;
    dev->GspGraphicsRunlistBase=runlistBase;
    dev->GspGraphicsSubmitToken=token;
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),token);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x90),2);
    LogStr("NVX [GR]: rang channel doorbell token=");LogHex(token);LogStr(" chid=0x2\n");
    LogStr("NVX [GR]: submitted SET_OBJECT, I2M inline-data upload, WAIT_FOR_IDLE and C797 semaphore.\n");
    for(wait=0;wait<100000;wait++) {
        if(grFence[0]==0x47524632) {
            break;
        }
        if(wait&&!(wait%10000)) {
            LogStr("NVX [GR]: waiting seconds=");LogHex(wait/10000);
            LogStr(" GPGet=");LogHex(userd[0x88/4]);
            LogStr(" GPPut=");LogHex(userd[0x8c/4]);
            LogStr(" fence=");LogHex(grFence[0]);LogStr("\n");
        }
        VideoPortStallExecution(100);
    }
    if(userd[0x88/4]==1||grFence[0]==0x47524632) {
        /* Drain asynchronous firmware diagnostics before judging completion. */
        VideoPortStallExecution(100000);
        grStatusAvailable=NvxGspGetName(dev);
        if(dev->GspMmuFaultQueued) {
            NvxGspLogFaultRingPointers(dev);
        }
        if(dev->GspAssertCount!=assertCountBefore) {
            LogStr("NVX [GR]: firmware assertions observed during submission: before=");
            LogHex(assertCountBefore);LogStr(" after=");LogHex(dev->GspAssertCount);LogStr("\n");
            NvxGspDumpNocatJournal(dev);
        } else if(grStatusAvailable&&!dev->GspGraphicsChannelFaulted&&!dev->GspMmuFaultQueued&&
                  userd[0x88/4]==1&&grFence[0]==0x47524632&&
                  grFence[1]==0x13579bdf&&grFence[2]==0x2468ace0) {
            ULONG i,matchCount=0,readbackMatchCount=0;
            dev->GspGraphicsCommandVerified=TRUE;
            for(i=0;i<16;i++) {
                if(grData[i]==0x67723000U+i) matchCount++;
            }
            if(matchCount==16) {
                dev->GspGraphicsDataWriteVerified=TRUE;
                LogStr("NVX [GR]: I2M data write verified; 16 dwords matched in scratch memory.\n");
            } else {
                /* Read the entire 4KB block-linear target through the already
                   verified CE copy path. */
                VideoPortZeroMemory((PVOID)grReadback,4096);
                if(NvxGspSubmitCopy2D(dev,dev->GspScratchGpuVa+0xe000,
                    grDataVa,256,256,256,16)) {
                    for(i=0;i<1024;i++) {
                        ULONG x=i%64U,y=i/64U;
                        ULONG pixel=NvxGspReadBlockLinearPixel((volatile UCHAR *)grReadback,x,y,64);
                        if(pixel==0) readbackMatchCount++;
#if NVX_ENABLE_GR_SUBMISSION_PROBE
                        if(pixel==0xff00ff00U||pixel==0x0000ff00U) grPixelCount++;
                        if(pixel==0xffffffffU) grWhiteCount++;
#endif
                    }
                }
#if NVX_ENABLE_GR_SUBMISSION_PROBE
                grZeroCount=readbackMatchCount;
                if(grPixelCount>=24&&grWhiteCount>0&&grPixelCount+grWhiteCount==1024) {
                    dev->GspGraphicsDataWriteVerified=TRUE;
                    dev->GspGraphicsPixelClearVerified=TRUE;
                    dev->GspGraphicsTriangleVerified=TRUE;
                    LogStr("NVX [GR]: green triangle pixel readback verified; green_pixels=");
                    LogHex(grPixelCount);LogStr(" clear_background=");LogHex(grWhiteCount);LogStr("\n");
                } else if(grZeroCount>=24&&grWhiteCount>0&&grZeroCount+grWhiteCount==1024) {
                    dev->GspGraphicsDataWriteVerified=TRUE;
                    dev->GspGraphicsPixelClearVerified=TRUE;
                    LogStr("NVX [GR]: clear readback contains black and white pixels, but expected green shader output is absent; triangle remains unverified.\n");
                } else if(grWhiteCount==1024) {
                    dev->GspGraphicsDataWriteVerified=TRUE;
                    dev->GspGraphicsPixelClearVerified=TRUE;
                    LogStr("NVX [GR]: 3D CLEAR_SURFACE readback verified; all 1024 target dwords are white.\n");
                    LogStr("NVX [GR]: triangle probe produced no verified colored pixels; green_pixels=");
                    LogHex(grPixelCount);LogStr("\n");
                } else {
                    LogStr("NVX [GR]: triangle readback mismatch; green_pixels=");
                    LogHex(grPixelCount);LogStr(" zero_pixels=");LogHex(grZeroCount);
                    LogStr(" white_pixels=");LogHex(grWhiteCount);LogStr("\n");
                }
#else
                if(readbackMatchCount==1024) {
                    dev->GspGraphicsDataWriteVerified=TRUE;
                    dev->GspGraphicsPixelClearVerified=TRUE;
                    LogStr("NVX [GR]: 3D CLEAR_SURFACE readback verified; all 1024 target dwords are zero.\n");
                } else {
                    LogStr("NVX [GR]: CLEAR_SURFACE readback mismatch; zero_words=");
                    LogHex(readbackMatchCount);LogStr(" expected=0x400\n");
                }
#endif
            }
        }
    }
    /* Compare the pre-submit context query with a second query after GPGet
       advances. Fetching a pushbuffer alone does not prove GR executed it. */
    if(userd[0x88/4]==1) {
        VideoPortZeroMemory(grBufferInfo,sizeof(*grBufferInfo));
        grBufferInfo->hUserClient=dev->GspClientHandle;
        grBufferInfo->hChannel=channelHandle;
        if(NvxGspControl(dev,dev->GspSubdeviceHandle,
                         NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO,
                         grBufferInfo,sizeof(*grBufferInfo))) {
            const NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS *reply=
                (const NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS *)(dev->RpcRxRecord+104);
            ULONG infoIndex,infoCount=reply->bufferCount;
            if(infoCount>NV2080_CTRL_GR_MAX_CTX_BUFFER_COUNT)
                infoCount=NV2080_CTRL_GR_MAX_CTX_BUFFER_COUNT;
            LogStr("NVX [GR]: post-submit context-buffer count=");
            LogHex(reply->bufferCount);LogStr("\n");
            for(infoIndex=0;infoIndex<infoCount&&infoIndex<12;infoIndex++) {
                LogStr("NVX [GR]: post-submit ctxbuf index=");LogHex(infoIndex);
                LogStr(" type=");LogHex(reply->ctxBufferInfo[infoIndex].bufferType);
                LogStr(" size=");LogHex((ULONG)reply->ctxBufferInfo[infoIndex].size);
                LogStr(" aperture=");LogHex(reply->ctxBufferInfo[infoIndex].aperture);
                LogStr("\n");
            }
        } else {
            LogStr("NVX [GR]: post-submit context-buffer lookup still failed.\n");
        }
    }
    if(dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued||
       dev->GspAssertCount!=assertCountBefore)
        dev->GspGraphicsCommandVerified=FALSE;
    if(!dev->GspGraphicsCommandVerified) {
        if(userd[0x88/4]==1)
            LogStr("NVX [GR]: command fetched but probe is unverified; GPGet=");
        else
            LogStr("NVX [GR]: command fetch timed out; GPGet=");
        LogHex(userd[0x88/4]);
        LogStr(" GPPut=");LogHex(userd[0x8c/4]);
        LogStr(" fence=");LogHex(grFence[0]);LogStr("\n");
        chramBase=chcfg&0xfffffff0U;
        if(chramBase&&chramBase<=dev->Bar0Length-16) {
            LogStr("NVX [GR]: CHRAM slots=");
            LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chramBase)));
            LogStr(",");LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chramBase+4)));
            LogStr(",");LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chramBase+8)));
            LogStr(",");LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chramBase+12)));LogStr("\n");
        }
        for(ULONG engine=0;engine<3;engine++) {
            ULONG engineStatus=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x200+engine*0x40));
            ULONG instanceLo=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x208+engine*0x40));
            ULONG instanceHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x20c+engine*0x40));
            LogStr("NVX [GR]: runlist engine=");LogHex(engine);
            LogStr(" status=");LogHex(engineStatus);LogStr(" instance=");LogHex(instanceHi);
            LogHex(instanceLo);LogStr("\n");
        }
        {
            PULONG ramfc=(PULONG)(scratch+0xb000);
            LogStr("NVX [GR]: RAMFC userdLo=");LogHex(ramfc[0x8/4]);
            LogStr(" userdHi=");LogHex(ramfc[0xc/4]);
            LogStr(" gpBaseLo=");LogHex(ramfc[0x48/4]);
            LogStr(" gpBaseHi=");LogHex(ramfc[0x4c/4]);
            LogStr(" chid=");LogHex(ramfc[0xe8/4]);LogStr("\n");
        }
        if(NvxGspControl(dev,channelHandle,NVA06F_CTRL_CMD_GET_MMU_FAULT_INFO,
                         fault,sizeof(fault))) {
            const ULONG *reply=(const ULONG *)dev->RpcRxRecord;
            LogStr("NVX [GR]: MMU fault addrHi=");LogHex(reply[26]);
            LogStr(" addrLo=");LogHex(reply[27]);LogStr(" type=");LogHex(reply[28]);LogStr("\n");
        } else LogStr("NVX [GR]: MMU fault info unavailable.\n");
        schedEnable[0]=0;
        if(!NvxGspControl(dev,channelHandle,NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                          schedEnable,sizeof(schedEnable)))
            LogStr("NVX [GR]: failed to confirm channel unschedule; backing retained.\n");
        return FALSE;
    }

    dev->GspGraphicsTsgHandle=0;
    dev->GspGraphicsContextShareHandle=0;
    dev->GspGraphicsChannelHandle=channelHandle;
    dev->GspGraphicsObjectHandle=grHandle;
    dev->GspGraphicsChannelReady=TRUE;
    if(dev->GspGraphicsTriangleVerified) {
        LogStr("NVX [GR]: GR pipeline triangle draw, pixel readback and semaphore completed successfully.\n");
    } else if(dev->GspGraphicsPixelClearVerified) {
        LogStr("NVX [GR]: GR pipeline 3D rasterizer clear and semaphore release completed successfully.\n");
    } else if(dev->GspGraphicsDataWriteVerified) {
        LogStr("NVX [GR]: GR pipeline data write and semaphore release completed successfully.\n");
    } else {
        LogStr("NVX [GR]: GR semaphore release completed with guards intact; no drawing tested.\n");
    }
    return TRUE;
}
#else
BOOLEAN NvxGspUnregisterFaultShadow(PNVX_DEVICE_EXTENSION dev)
{
    return !dev||!dev->GspFaultShadowRegistered;
}
#endif

/* The user ABI deliberately exposes a validated CLEAR_SURFACE operation,
 * not arbitrary methods on this shared privileged channel. The kernel builds
 * the C797 pushbuffer and owns the semaphore address. */
volatile ULONG NvxGrUserSubmitBusy=0;

#if NVX_ENABLE_GR_SUBMISSION_PROBE
static VOID NvxGspAppendUserTriangle(PNVX_DEVICE_EXTENSION dev,PUCHAR scratch,
                                    PULONG push,PULONG count,
                                    BOOLEAN uploadedVertices,
                                    BOOLEAN depthShader,
                                    BOOLEAN viewportTransform,
                                    ULONG vertexCount,
                                    ULONG targetWidth,ULONG targetHeight)
{
    static const ULONG vertices[18]={
        0,0,0,0,0,0x3f800000U,
        0,0x3f800000U,0,0x3f800000U,0,0x3f800000U,
        0x3f800000U,0,0x3f800000U,0,0,0x3f800000U
    };
    static const ULONG depthVertices[24]={
        0,0,0x3f000000U,0x3f800000U, 0x3f000000U,0x3f800000U,0,0x3f800000U,
        0,0x3f800000U,0x3f000000U,0x3f800000U, 0x3f000000U,0x3f800000U,0,0x3f800000U,
        0x3f800000U,0,0x3f000000U,0x3f800000U, 0x3f000000U,0x3f800000U,0,0x3f800000U
    };
    ULONG uniforms[8]={64,16,1,1,0,0,0,0};
    if(targetWidth>64U||targetHeight>16U) {
        uniforms[0]=1;
        uniforms[1]=1;
    }
    /* Keep depth VS code at a distinct GPU VA from the color VS. Scratch
       +0x1000 is otherwise unused and mapped by the same verified 64KB leaf. */
    ULONG64 shaderVa=dev->GspScratchGpuVa+(depthShader?0x1000:0x8000);
    ULONG64 resourceVa=dev->GspScratchGpuVa+0x8000;
    ULONG64 cbVa=resourceVa+0x600;
    /* +0xa000 is the RM-owned channel instance block. Keep submitted vertex
       data below the diagnostic readback area and the fence word. */
    ULONG64 vertexVa=dev->GspScratchGpuVa+0x6000;
    ULONG64 textureVa=resourceVa+0x900;
    ULONG64 bindlessVa=resourceVa+0xa00;
    ULONG64 textureDataVa=dev->GspScratchGpuVa+0x7000;
    PULONG cb=(PULONG)(scratch+0x8600);
    PULONG vertex=(PULONG)(scratch+0x6000);
    PULONG texture=(PULONG)(scratch+0x8900);
    PULONG bindless=(PULONG)(scratch+0x8a00);
    ULONG n=*count,i;

    VideoPortZeroMemory(scratch+(depthShader?0x1000:0x8000),0x1000);
    VideoPortMoveMemory(scratch+(depthShader?0x1000:0x8000),
                        (PVOID)NvxAmpereHeadSurfacePrograms,1408);
    VideoPortMoveMemory(cb,(PVOID)uniforms,sizeof(uniforms));
    if(uploadedVertices)
        VideoPortMoveMemory(vertex,scratch+0xb800,
                            depthShader?vertexCount*32U:vertexCount*24U);
    else if(depthShader)
        VideoPortMoveMemory(vertex,(PVOID)depthVertices,sizeof(depthVertices));
    else
        VideoPortMoveMemory(vertex,(PVOID)vertices,sizeof(vertices));
    if(depthShader) {
        LogStr("NVX [3D-DEPTH]: upload source XY=");
        LogHex(((PULONG)(scratch+0xb800))[0]);
        LogHex(((PULONG)(scratch+0xb800))[1]);
        LogStr(" generic XY=");
        LogHex(((PULONG)(scratch+0xb800))[4]);
        LogHex(((PULONG)(scratch+0xb800))[5]);
        LogStr("\n");
        /* Preserve the required POSITION_W declaration. Add POSITION_Z to
           the baseline XYW mask, yielding the valid XYZW mask 0xf000. */
        ((PULONG)(scratch+0x1000))[13]|=(1U<<14);
        LogStr("NVX [3D-DEPTH]: shader_va=");LogHex(shaderVa);
        LogStr(" SPH_OMAP_POSITION_XYZW=");
        LogHex(((PULONG)(scratch+0x1000))[13]);
        LogStr(" SPH_IMAP_ATTR0=");
        LogHex(((PULONG)(scratch+0x1000))[6]);LogStr("\n");
        /* The baseline shader already loads generic attribute 8.x/y into
           R8/R9 at +0x00. Emit that aligned pair to clip Z/W immediately
           before EXIT, keeping the original varying and position stores. */
        VideoPortMoveMemory(scratch+0x1220,scratch+0x1210,16);
        VideoPortMoveMemory(scratch+0x1210,scratch+0x1200,16);
        {
            PULONG storeZW=(PULONG)(scratch+0x1200);
            storeZW[0]=0xff007322U;storeZW[1]=0x00007808U;
            storeZW[2]=0x000004ffU;storeZW[3]=0x001fee00U;
        }
        LogStr("NVX [3D-DEPTH]: shader generic ALD64=");
        LogHex(((PULONG)(scratch+0x1000))[0]);
        LogHex(((PULONG)(scratch+0x1000))[1]);
        LogStr(" appended AST64 clip ZW from R8/R9");
        LogStr(" vertex XY=");LogHex(((PULONG)vertex)[0]);
        LogHex(((PULONG)vertex)[1]);LogStr(" generic XY=");
        LogHex(((PULONG)vertex)[4]);LogHex(((PULONG)vertex)[5]);
        LogStr(" vertex position ZW=");LogHex(((PULONG)vertex)[2]);
        LogHex(((PULONG)vertex)[3]);LogStr(" generic ZW=");
        LogHex(((PULONG)vertex)[4]);LogHex(((PULONG)vertex)[5]);
        LogStr(" clipZ_source=attribute8.x(R8)\n");
    }
    bindless[0]=0;bindless[1]=255;bindless[2]=255;
    bindless[3]=255;bindless[4]=255;bindless[5]=255;
    NvxGspBuildProbeTexture(texture,texture+8,textureDataVa);
    __sync_synchronize();

#define NVX_TRI_INCR(_m,_c) (push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|((_c)<<16)|((_m)>>2))
    NVX_TRI_INCR(NVC797_SET_TEX_SAMPLER_POOL_A,3);
    push[n++]=(ULONG)(textureVa>>32);push[n++]=(ULONG)textureVa;push[n++]=0;
    NVX_TRI_INCR(NVC797_SET_TEX_HEADER_POOL_A,3);
    push[n++]=(ULONG)((textureVa+32)>>32);push[n++]=(ULONG)(textureVa+32);push[n++]=0;
    NVX_TRI_INCR(NVC797_SET_SAMPLER_BINDING,1);
    push[n++]=NVC797_SET_SAMPLER_BINDING_V_VIA_HEADER_BINDING;
    NVX_TRI_INCR(NVC797_SET_PIPELINE_SHADER(1),1);
    push[n++]=DRF_DEF(C797,_SET_PIPELINE_SHADER,_ENABLE,_TRUE)|
              DRF_DEF(C797,_SET_PIPELINE_SHADER,_TYPE,_VERTEX);
    NVX_TRI_INCR(NVC797_SET_PIPELINE_REGISTER_COUNT(1),2);
    push[n++]=15;push[n++]=0;
    NVX_TRI_INCR(NVC797_SET_PIPELINE_PROGRAM_ADDRESS_A(1),2);
    push[n++]=(ULONG)(shaderVa>>32);push[n++]=(ULONG)shaderVa;
    NVX_TRI_INCR(NVC797_SET_PIPELINE_SHADER(5),1);
    push[n++]=DRF_DEF(C797,_SET_PIPELINE_SHADER,_ENABLE,_TRUE)|
              DRF_DEF(C797,_SET_PIPELINE_SHADER,_TYPE,_PIXEL);
    NVX_TRI_INCR(NVC797_SET_PIPELINE_REGISTER_COUNT(5),2);
    push[n++]=13;push[n++]=4;
    NVX_TRI_INCR(NVC797_SET_PIPELINE_PROGRAM_ADDRESS_A(5),2);
    push[n++]=(ULONG)(shaderVa>>32);push[n++]=(ULONG)(shaderVa+0x300);
    NVX_TRI_INCR(NVC797_INVALIDATE_SHADER_CACHES,1);
    push[n++]=DRF_DEF(C797,_INVALIDATE_SHADER_CACHES,_INSTRUCTION,_TRUE)|
              DRF_DEF(C797,_INVALIDATE_SHADER_CACHES,_DATA,_TRUE)|
              DRF_DEF(C797,_INVALIDATE_SHADER_CACHES,_CONSTANT,_TRUE)|
              DRF_DEF(C797,_INVALIDATE_SHADER_CACHES,_FLUSH_DATA,_TRUE);
    NVX_TRI_INCR(NVC797_BIND_GROUP_CONSTANT_BUFFER(0),1);
    push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_FALSE)|
              DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,1);
    NVX_TRI_INCR(NVC797_BIND_GROUP_CONSTANT_BUFFER(4),1);
    push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_FALSE)|
              DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,1);
    NVX_TRI_INCR(NVC797_SET_CONSTANT_BUFFER_SELECTOR_A,3);
    push[n++]=256;push[n++]=(ULONG)(cbVa>>32);push[n++]=(ULONG)cbVa;
    NVX_TRI_INCR(NVC797_BIND_GROUP_CONSTANT_BUFFER(0),1);
    push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_TRUE)|
              DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,3);
    NVX_TRI_INCR(NVC797_SET_CONSTANT_BUFFER_SELECTOR_A,3);
    push[n++]=256;push[n++]=(ULONG)(bindlessVa>>32);push[n++]=(ULONG)bindlessVa;
    NVX_TRI_INCR(NVC797_BIND_GROUP_CONSTANT_BUFFER(4),1);
    push[n++]=DRF_DEF(C797,_BIND_GROUP_CONSTANT_BUFFER,_VALID,_TRUE)|
              DRF_NUM(C797,_BIND_GROUP_CONSTANT_BUFFER,_SHADER_SLOT,4);
    NVX_TRI_INCR(NVC797_SET_DA_OUTPUT_ATTRIBUTE_SKIP_MASK_A(0),2);
    push[n++]=0xfffffff0U;push[n++]=0xfffffff0U;
    /* Initialize viewport state for every bounded draw. The channel's
       startup probe configures this once, but relying on that previous state
       made the standalone user operation sensitive to stale/default clips. */
    NVX_TRI_INCR(NVC797_SET_VIEWPORT_SCALE_OFFSET,1);
    /* Preserve-target Mesa batches supply clip-space XY. Fixed/client
       diagnostic draws keep the vendor shader's proven pixel-space path. */
    push[n++]=viewportTransform?
        NVC797_SET_VIEWPORT_SCALE_OFFSET_ENABLE_TRUE:
        NVC797_SET_VIEWPORT_SCALE_OFFSET_ENABLE_FALSE;
    if(viewportTransform) {
        NVX_TRI_INCR(NVC797_SET_VIEWPORT_SCALE_X(0),3);
        push[n++]=NvxGspFloatBits((float)targetWidth*0.5f);
        push[n++]=NvxGspFloatBits((float)targetHeight*0.5f);
        push[n++]=0x3f800000U; /* SCALE_Z = 1.0f */
        NVX_TRI_INCR(NVC797_SET_VIEWPORT_OFFSET_X(0),3);
        push[n++]=NvxGspFloatBits((float)targetWidth*0.5f);
        push[n++]=NvxGspFloatBits((float)targetHeight*0.5f);
        push[n++]=0;          /* OFFSET_Z = 0.0f */
        NVX_TRI_INCR(NVC797_SET_VIEWPORT_COORDINATE_SWIZZLE(0),1);
        push[n++]=0x6420U; /* X=POS_X, Y=POS_Y, Z=POS_Z, W=POS_W */
    }
    NVX_TRI_INCR(NVC797_SET_VIEWPORT_CLIP_CONTROL,1);
    push[n++]=NVC797_SET_VIEWPORT_CLIP_CONTROL_PIXEL_MIN_Z_CLAMP<<3|
              NVC797_SET_VIEWPORT_CLIP_CONTROL_PIXEL_MAX_Z_CLAMP<<4|
              NVC797_SET_VIEWPORT_CLIP_CONTROL_GEOMETRY_CLIP_WZERO_CLIP;
    NVX_TRI_INCR(NVC797_SET_VIEWPORT_Z_CLIP,1);
    push[n++]=NVC797_SET_VIEWPORT_Z_CLIP_RANGE_ZERO_TO_POSITIVE_W;
    for(i=0;i<16;i++) {
        NVX_TRI_INCR(NVC797_SET_VIEWPORT_CLIP_HORIZONTAL(i),4);
        push[n++]=(32768U<<16);push[n++]=(32768U<<16);
        push[n++]=0;push[n++]=0x3f800000U; /* MIN_Z=0.0f, MAX_Z=1.0f */
    }
    NVX_TRI_INCR(NVC797_SET_PROVOKING_VERTEX,1);
    push[n++]=NVC797_SET_PROVOKING_VERTEX_V_LAST;
    NVX_TRI_INCR(NVC797_SET_VERTEX_ATTRIBUTE_A(0),1);
    push[n++]=1U|((depthShader?1U:4U)<<21)|(7U<<27);
    NVX_TRI_INCR(NVC797_SET_VERTEX_ATTRIBUTE_A(8),1);
    push[n++]=1U|((depthShader?16U:8U)<<7)|(1U<<21)|(7U<<27);
    NVX_TRI_INCR(NVC797_SET_VERTEX_STREAM_A_FORMAT(1),3);
    push[n++]=DRF_NUM(C797,_SET_VERTEX_STREAM_A_FORMAT,_STRIDE,
                      depthShader?32:24)|
              DRF_DEF(C797,_SET_VERTEX_STREAM_A_FORMAT,_ENABLE,_TRUE);
    push[n++]=(ULONG)(vertexVa>>32);push[n++]=(ULONG)vertexVa;
    NVX_TRI_INCR(NVC797_SET_VERTEX_STREAM_SIZE_A(1),2);
    push[n++]=0;push[n++]=depthShader?vertexCount*32U:vertexCount*24U;
    NVX_TRI_INCR(NVC797_OGL_SET_CULL,1);push[n++]=NVC797_OGL_SET_CULL_ENABLE_FALSE;
    NVX_TRI_INCR(NVC797_BEGIN,1);push[n++]=NVC797_BEGIN_OP_TRIANGLES;
    NVX_TRI_INCR(NVC797_SET_VERTEX_ARRAY_START,2);
    push[n++]=0;push[n++]=vertexCount;
    NVX_TRI_INCR(NVC797_END,1);push[n++]=0;
#undef NVX_TRI_INCR
    *count=n;
}
#endif

static BOOLEAN NvxGspSubmitUserOperation(PVOID HwDeviceExtension,ULONG64 dstVa,
    ULONG64 depthVa,ULONG color,ULONG64 fenceValue,BOOLEAN drawTriangle,
    BOOLEAN preserveTarget,ULONG depthMode,ULONG depthFormat,
    ULONG depthPitchBytes,ULONG64 vertexVa,
    ULONG vertexCount,ULONG triangleColor,ULONG targetWidth,
    ULONG targetHeight,ULONG targetTileMode,
    ULONG *greenPixels,ULONG *whitePixels)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *userd,*fence;
    volatile ULONG64 *fifo;
    PULONG push;
    ULONG n=0,i,put,wait;
    ULONG gpGetSnapshot,gpPutSnapshot;
    ULONG64 pushVa,fenceVa;
    if(greenPixels) *greenPixels=0;
    if(whitePixels) *whitePixels=0;
    if(!dev||!dev->GspInitialized||!dev->GspGraphicsChannelReady||
       !dev->GspGraphicsCommandVerified||dev->GspGraphicsChannelFaulted||
       dev->GspMmuFaultQueued||
       (drawTriangle&&(vertexCount<3U||vertexCount>NVX_3D_MAX_BATCH_VERTICES||
        vertexCount%3U))||
       !targetWidth||!targetHeight||targetWidth>NVX_3D_MAX_SURFACE_DIMENSION||
       targetHeight>NVX_3D_MAX_SURFACE_DIMENSION||
       (targetTileMode&~0x70U)||
       depthMode>5U||depthFormat>NVX_3D_ZT_FORMAT_ZF32||
       (depthMode&&depthPitchBytes&&
        ((depthPitchBytes&63U)||depthPitchBytes<targetWidth*4U))||
       (depthMode&&((depthVa&0xffffULL)||depthVa<dev->GspVramGpuVa||
        depthVa>=dev->GspVramGpuVa+dev->GspVramMapBytes))||
       (!drawTriangle&&vertexVa)||(vertexVa&3ULL)||
       !fenceValue||fenceValue>0xffffffffULL||
       (dstVa&0xffffULL)||!dev->GspDmaVirtual||!dev->GspGraphicsRunlistBase) {
        if(drawTriangle) LogStr("NVX [3D-TRI]: rejected by channel/state validation.\n");
        return FALSE;
    }
    /* The CE target prefill, uploaded vertex staging and GR draw share this
       scratch allocation. Hold the cross-engine lock across the whole
       operation so a concurrent copy/fill/upload cannot reuse those bytes. */
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) {
        if(drawTriangle) LogStr("NVX [3D-TRI]: shared scratch lock timeout.\n");
        return FALSE;
    }
    /* Keep the rasterizer diagnostic independent of C797 CLEAR_SURFACE,
       which is being tested separately. Seed the bounded target by CE-copying
       a complete coherent 4KB staging page, not with the unverified fill path. */
    if(drawTriangle&&!preserveTarget) {
        PUCHAR prefillScratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
        volatile ULONG *prefillReadback=(volatile ULONG *)(prefillScratch+0xe000);
        ULONG whitePixels=0,rawWhiteWords=0;
        for(i=0;i<1024;i++) prefillReadback[i]=0xffffffffU;
        __sync_synchronize();
        if(!NvxGspSubmitCopy2DLocked(dev,dstVa,dev->GspScratchGpuVa+0xe000,
                               256,256,256,16)) {
            LogStr("NVX [3D-TRI]: CE target prefill copy failed.\n");
            __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
        }
        VideoPortZeroMemory((PVOID)prefillReadback,4096);
        if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xe000,
                               dstVa,256,256,256,16)) {
            LogStr("NVX [3D-TRI]: CE target prefill readback failed.\n");
            __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
        }
        for(i=0;i<1024;i++) {
            ULONG x=i%64U,y=i/64U;
            if(prefillReadback[i]==0xffffffffU) rawWhiteWords++;
            if(NvxGspReadBlockLinearPixel((volatile UCHAR *)prefillReadback,
                                           x,y,64)==0xffffffffU) whitePixels++;
        }
        LogStr("NVX [3D-TRI]: CE prefill raw_white_words=");LogHex(rawWhiteWords);
        LogStr(" logical_white_pixels=");LogHex(whitePixels);LogStr("/0x400\n");
    }
#if !NVX_ENABLE_GR_SUBMISSION_PROBE
    if(drawTriangle) {
        LogStr("NVX [3D-TRI]: submission probe is not built in.\n");
        __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
    }
#endif
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    if(drawTriangle&&vertexVa) {
        /* Accept only uploaded vertex bytes; all GR methods, shaders and
           semaphore addresses remain constructed by this kernel driver. */
        ULONG vertexBytes=depthMode?vertexCount*32U:vertexCount*24U;
        if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xb800,vertexVa,
                               vertexBytes,vertexBytes,vertexBytes,1)) {
            if(depthMode==5U) {
                LogStr("NVX [3D-DEPTH]: vertex CE copy failed.");
                LogStr(" src=");LogHex(vertexVa);LogStr(" bytes=");
                LogHex(vertexBytes);LogStr(" fence=");LogHex(fenceValue);LogStr("\n");
            }
            __sync_lock_release(&NvxGrUserSubmitBusy);
            return FALSE;
        }
        __sync_synchronize();
        for(i=0;i<(depthMode?vertexCount*8U:vertexCount*6U);i++) {
            ULONG component=((volatile ULONG *)(scratch+0xb800))[i];
            ULONG componentInVertex=i%(depthMode?8U:6U);
            BOOLEAN positionXY=(componentInVertex<2U);
            /* Permit coordinates in [-1.0f, +1.0f] for positionXY (supporting
               both Mesa's divided clip coordinates and client normalized [0,1]
               coordinates); depth and generic attributes stay in [0.0f, 1.0f]. */
            if((positionXY &&
                !(component<=0x3f800000U ||
                  (component>=0x80000000U && component<=0xbf800000U))) ||
               (!positionXY&&component>0x3f800000U)) {
                LogStr("NVX [3D-TRI]: uploaded vertex component outside bounded range index=");
                LogHex(i);LogStr(" bits=");LogHex(component);LogStr(".\n");
                __sync_lock_release(&NvxGrUserSubmitBusy);
                return FALSE;
            }
        }
    }
    userd=(volatile ULONG *)(scratch+0xb400);
    fence=(volatile ULONG *)(scratch+0x5000);
    fifo=(volatile ULONG64 *)(scratch+0x9000);
    push=(PULONG)(scratch+0xf000);
    pushVa=dev->GspScratchGpuVa+0xf000;
    fenceVa=dev->GspScratchGpuVa+0x5000;
    fence[0]=0;fence[1]=0;fence[2]=0;fence[3]=0;
    fence[4]=0;fence[5]=0;fence[6]=0;fence[7]=0;
    fence[8]=0;fence[9]=0;fence[10]=0;fence[11]=0;
    fence[12]=0;fence[13]=0;fence[14]=0;fence[15]=0;
    __sync_synchronize();
#define NVX_GR_INCR(_m,_c) (push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|((_c)<<16)|((_m)>>2))
#define NVX_GR_CHECKPOINT(_offset) do { \
    if(drawTriangle) { \
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4); \
        push[n++]=(ULONG)((fenceVa+(_offset))>>32); \
        push[n++]=(ULONG)(fenceVa+(_offset)); \
        push[n++]=(ULONG)fenceValue; \
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)| \
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)| \
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)| \
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD); \
    } \
} while(0)
    NVX_GR_INCR(NVC56F_SET_OBJECT,1);push[n++]=0xC797;
    NVX_GR_CHECKPOINT(40);
    NVX_GR_INCR(NVC797_SET_COLOR_TARGET_FORMAT(0),1);
    push[n++]=NVC797_SET_COLOR_TARGET_FORMAT_V_DISABLED;
    NVX_GR_CHECKPOINT(44);
    NVX_GR_INCR(NVC797_SET_ZT_SELECT,1);push[n++]=0;
    NVX_GR_CHECKPOINT(48);
    NVX_GR_INCR(NVC797_SET_POINT_CENTER_MODE,1);
    push[n++]=NVC797_SET_POINT_CENTER_MODE_V_OGL;
    NVX_GR_CHECKPOINT(52);
    NVX_GR_INCR(NVC797_SET_SAMPLER_BINDING,1);
    push[n++]=NVC797_SET_SAMPLER_BINDING_V_VIA_HEADER_BINDING;
    NVX_GR_CHECKPOINT(56);
    NVX_GR_INCR(NVC797_SET_VIEWPORT_SCALE_OFFSET,1);push[n++]=0;
    NVX_GR_CHECKPOINT(60);
    for(i=0;i<16;i++) {
        NVX_GR_INCR(NVC797_SET_VIEWPORT_CLIP_HORIZONTAL(i),2);
        push[n++]=(32768U<<16);push[n++]=(32768U<<16);
        if(i==7U&&drawTriangle) {
            NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
            push[n++]=(ULONG)((fenceVa+32)>>32);push[n++]=(ULONG)(fenceVa+32);
            push[n++]=(ULONG)fenceValue;
            push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                      DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                      DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                      DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
        }
    }
    if(drawTriangle) {
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+28)>>32);push[n++]=(ULONG)(fenceVa+28);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    }
    NVX_GR_INCR(NVC797_SET_PROVOKING_VERTEX,1);
    push[n++]=NVC797_SET_PROVOKING_VERTEX_V_LAST;
    NVX_GR_INCR(NVC797_SET_SINGLE_ROP_CONTROL,1);push[n++]=1;
    NVX_GR_INCR(NVC797_SET_SINGLE_CT_WRITE_CONTROL,1);push[n++]=1;
    NVX_GR_INCR(NVC797_SET_BLEND(0),1);push[n++]=1;
    NVX_GR_INCR(NVC797_SET_BLEND_CONST_RED,4);
    push[n++]=0;push[n++]=0;push[n++]=0;push[n++]=0;
    NVX_GR_INCR(NVC797_SET_BLEND_SEPARATE_FOR_ALPHA,1);push[n++]=0;
    NVX_GR_INCR(NVC797_SET_BLEND_COLOR_OP,3);
    push[n++]=NVC797_SET_BLEND_COLOR_OP_V_OGL_FUNC_ADD;
    push[n++]=NVC797_SET_BLEND_COLOR_SOURCE_COEFF_V_OGL_ONE;
    push[n++]=NVC797_SET_BLEND_COLOR_DEST_COEFF_V_OGL_ZERO;
    NVX_GR_INCR(NVC797_SET_LINE_WIDTH_FLOAT,1);push[n++]=0x3f800000U;
    NVX_GR_INCR(NVC797_SET_LINE_MULTISAMPLE_OVERRIDE,1);push[n++]=0;
    if(drawTriangle) {
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+24)>>32);push[n++]=(ULONG)(fenceVa+24);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    }
    NVX_GR_INCR(NVC797_SET_WINDOW_ORIGIN,1);
    push[n++]=DRF_DEF(C797,_SET_WINDOW_ORIGIN,_MODE,_UPPER_LEFT)|
              DRF_DEF(C797,_SET_WINDOW_ORIGIN,_FLIP_Y,_TRUE);
    NVX_GR_INCR(NVC797_SET_ZCULL_BOUNDS,1);
    push[n++]=DRF_DEF(C797,_SET_ZCULL_BOUNDS,_Z_MIN_UNBOUNDED_ENABLE,_FALSE)|
              DRF_DEF(C797,_SET_ZCULL_BOUNDS,_Z_MAX_UNBOUNDED_ENABLE,_FALSE);
    if(depthMode) {
        NVX_GR_INCR(NVC797_SET_ZT_READ_ONLY,1);
        push[n++]=NVC797_SET_ZT_READ_ONLY_ENABLE_Z_FALSE|
                  NVC797_SET_ZT_READ_ONLY_ENABLE_STENCIL_FALSE;
        NVX_GR_INCR(NVC797_SET_DEPTH_BOUNDS_TEST,1);
        push[n++]=NVC797_SET_DEPTH_BOUNDS_TEST_ENABLE_FALSE;
        NVX_GR_INCR(NVC797_SET_ZCULL,1);
        /* The bounded probe has no Z-cull backing storage; keep the
           hierarchical Z engine disabled while ordinary Z testing runs. */
        push[n++]=DRF_DEF(C797,_SET_ZCULL,_Z_ENABLE,_FALSE)|
                  DRF_DEF(C797,_SET_ZCULL,_STENCIL_ENABLE,_FALSE);
        NVX_GR_INCR(NVC797_SET_ACTIVE_ZCULL_REGION,1);
        push[n++]=0x3f; /* NVK's no-zcull active-region sentinel. */
        NVX_GR_INCR(NVC797_SET_STENCIL_TEST,1);
        push[n++]=NVC797_SET_STENCIL_TEST_ENABLE_FALSE;
    }
    NVX_GR_INCR(NVC797_SET_SHADER_EXCEPTIONS,1);push[n++]=0;
    NVX_GR_INCR(NVC797_SET_GO_IDLE_TIMEOUT,1);push[n++]=0x800;
    NVX_GR_INCR(NVC797_SET_SPH_VERSION,2);
    push[n++]=(4U)|(4U<<16);push[n++]=(4U)|(4U<<16);
    /* Probe completion of common state separately from target binding. */
    if(drawTriangle) {
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+20)>>32);push[n++]=(ULONG)(fenceVa+20);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    }
    NVX_GR_INCR(NVC797_SET_CT_SELECT,1);push[n++]=1;
    NVX_GR_INCR(NVC797_SET_COLOR_TARGET_A(0),6);
    push[n++]=(ULONG)(dstVa>>32);push[n++]=(ULONG)dstVa;
    push[n++]=targetWidth;push[n++]=targetHeight;
    push[n++]=NVC797_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8;
    push[n++]=targetTileMode;
    /* Separate a rejected color-target binding from later clipping/state. */
    if(drawTriangle) {
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+16)>>32);push[n++]=(ULONG)(fenceVa+16);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    }
    NVX_GR_INCR(NVC797_SET_SURFACE_CLIP_HORIZONTAL,2);
    push[n++]=(targetWidth<<16);push[n++]=(targetHeight<<16);
    /* Checkpoint after common state and color target binding, before zeta
       state is applied. */
    if(drawTriangle) {
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+12)>>32);push[n++]=(ULONG)(fenceVa+12);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    }
    if(depthMode) {
        NVX_GR_INCR(NVC797_SET_ZT_A,2);
        push[n++]=(ULONG)(depthVa>>32);push[n++]=(ULONG)depthVa;
        NVX_GR_INCR(NVC797_SET_ZT_FORMAT,1);
        push[n++]=depthFormat==NVX_3D_ZT_FORMAT_X8Z24?
                      NVC797_SET_ZT_FORMAT_V_X8Z24:
                  depthFormat==NVX_3D_ZT_FORMAT_S8Z24?
                      NVC797_SET_ZT_FORMAT_V_S8Z24:
                  depthFormat==NVX_3D_ZT_FORMAT_ZF32?
                      NVC797_SET_ZT_FORMAT_V_ZF32:
                      NVC797_SET_ZT_FORMAT_V_Z24S8;
        LogStr("NVX [3D-DEPTH]: bind format=");LogHex(push[n-1]);
        LogStr(" target=");LogHex(depthVa);
        LogStr(" PTE kind=");LogHex(depthFormat==NVX_3D_ZT_FORMAT_Z24S8?5U:
            depthFormat==NVX_3D_ZT_FORMAT_ZF32?6U:3U);
        LogStr(" zcull=disabled\n");
        /* NVK requires zeta tiling y_log2 == 0 on Maxwell and later. */
        NVX_GR_INCR(NVC797_SET_ZT_BLOCK_SIZE,1);push[n++]=0;
        {
            ULONG depthPitch=depthPitchBytes?depthPitchBytes:256U;
            ULONG depthRows=(targetHeight+7U)&~7U;
            NVX_GR_INCR(NVC797_SET_ZT_ARRAY_PITCH,1);
            push[n++]=(depthPitch*depthRows)/4U;
        }
        /* Match NVC0/NVK: bind address/format/tiling, select the target,
           then program its stride-based extent. */
        NVX_GR_INCR(NVC797_SET_ZT_SELECT,1);push[n++]=1;
        NVX_GR_INCR(NVC797_SET_ZT_SIZE_A,3);
        push[n++]=(depthPitchBytes?depthPitchBytes:256U)/4U;
        push[n++]=targetHeight;push[n++]=1;
        NVX_GR_INCR(NVC797_SET_ZT_LAYER,1);push[n++]=0;
        /* NVK explicitly disables sparse zeta addressing on Maxwell and
           later. Set the same state instead of relying on class defaults. */
        NVX_GR_INCR(NVC797_SET_ZT_SPARSE,1);
        push[n++]=NVC797_SET_ZT_SPARSE_ENABLE_FALSE;
        NVX_GR_INCR(NVC797_SET_Z_COMPRESSION,1);push[n++]=0;
        NVX_GR_INCR(NVC797_SET_STENCIL_COMPRESSION,1);
        push[n++]=NVC797_SET_STENCIL_COMPRESSION_ENABLE_FALSE;
        NVX_GR_INCR(NVC797_SET_DEPTH_TEST,1);
        push[n++]=depthMode>=3?
            NVC797_SET_DEPTH_TEST_ENABLE_TRUE:
            NVC797_SET_DEPTH_TEST_ENABLE_FALSE;
        NVX_GR_INCR(NVC797_SET_DEPTH_WRITE,1);
        push[n++]=depthMode>=5?
            NVC797_SET_DEPTH_WRITE_ENABLE_TRUE:
            NVC797_SET_DEPTH_WRITE_ENABLE_FALSE;
        NVX_GR_INCR(NVC797_SET_DEPTH_FUNC,1);
        push[n++]=NVC797_SET_DEPTH_FUNC_V_OGL_LESS;
    } else {
        NVX_GR_INCR(NVC797_SET_DEPTH_TEST,1);
        push[n++]=NVC797_SET_DEPTH_TEST_ENABLE_FALSE;
        NVX_GR_INCR(NVC797_SET_DEPTH_WRITE,1);
        push[n++]=NVC797_SET_DEPTH_WRITE_ENABLE_FALSE;
    }
    if(depthMode==1) {
        NVX_GR_INCR(NVC797_SET_Z_CLEAR_VALUE,1);push[n++]=0x3f800000U;
        NVX_GR_INCR(NVC797_SET_CLEAR_RECT_HORIZONTAL,2);
        push[n++]=(targetWidth<<16);push[n++]=(targetHeight<<16);
        NVX_GR_INCR(NVC797_CLEAR_SURFACE,1);
        push[n++]=NVC797_CLEAR_SURFACE_Z_ENABLE_TRUE;
    } else if(!drawTriangle) {
        NVX_GR_INCR(NVC797_SET_COLOR_CLEAR_VALUE(0),4);
        push[n++]=NvxUnorm8ToFloatBits((color>>16)&0xffU);
        push[n++]=NvxUnorm8ToFloatBits((color>>8)&0xffU);
        push[n++]=NvxUnorm8ToFloatBits(color&0xffU);
        push[n++]=NvxUnorm8ToFloatBits((color>>24)&0xffU);
        NVX_GR_INCR(NVC797_SET_CLEAR_RECT_HORIZONTAL,2);
        push[n++]=(targetWidth<<16);push[n++]=(targetHeight<<16);
        NVX_GR_INCR(NVC797_CLEAR_SURFACE,1);
        push[n++]=DRF_DEF(C797,_CLEAR_SURFACE,_R_ENABLE,_TRUE)|
                  DRF_DEF(C797,_CLEAR_SURFACE,_G_ENABLE,_TRUE)|
                  DRF_DEF(C797,_CLEAR_SURFACE,_B_ENABLE,_TRUE)|
                  DRF_DEF(C797,_CLEAR_SURFACE,_A_ENABLE,_TRUE);
    }
#if NVX_ENABLE_GR_SUBMISSION_PROBE
    if(drawTriangle) {
        /* The probe sampler is A8B8G8R8, while the public ABI's Color is
           packed AARRGGBB. Swap red/blue before writing the texel so the
           fragment shader observes the caller's logical RGBA color. */
        ((PULONG)(scratch+0x7000))[0]=
            (triangleColor&0xff00ff00U)|
            ((triangleColor&0x000000ffU)<<16)|
            ((triangleColor&0x00ff0000U)>>16);
        /* The diagnostic shader samples this one-texel resource at a stable
           GPU VA. Invalidate cached texel data after changing its color. */
        NVX_GR_INCR(NVC797_INVALIDATE_TEXTURE_DATA_CACHE,1);
        push[n++]=NVC797_INVALIDATE_TEXTURE_DATA_CACHE_LINES_ALL;
        /* Checkpoint after target/depth/shader resource state setup, before
           the vertex shader and rasterizer methods are appended. */
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+8)>>32);push[n++]=(ULONG)(fenceVa+8);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
        NvxGspAppendUserTriangle(dev,scratch,push,&n,vertexVa!=0,
                                 depthMode!=0,
                                 depthMode!=0||preserveTarget,
                                 vertexCount,targetWidth,targetHeight);
    }
#endif
    /* Diagnostic checkpoint: mark completion of the draw before idle wait. */
    if(drawTriangle) {
        NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
        push[n++]=(ULONG)((fenceVa+4)>>32);push[n++]=(ULONG)(fenceVa+4);
        push[n++]=(ULONG)fenceValue;
        push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
                  DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    }
    NVX_GR_INCR(NVC797_WAIT_FOR_IDLE,1);push[n++]=0;
    NVX_GR_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
    push[n++]=(ULONG)(fenceVa>>32);push[n++]=(ULONG)fenceVa;
    push[n++]=(ULONG)fenceValue;
    push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
              DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
              DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
              DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
#undef NVX_GR_CHECKPOINT
#undef NVX_GR_INCR
    if(n>4096U) {
        __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
    }
    /* The RM channel was allocated with gpFifoEntries=512. USERD pointers
       must wrap at that allocation's entry count. */
    put=dev->GspGraphicsGPPut%512;
    fifo[put]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();
    dev->GspGraphicsGPPut=(put+1)%512;
    userd[0x8c/4]=dev->GspGraphicsGPPut;
    __sync_synchronize();
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),
                                     dev->GspGraphicsSubmitToken);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
        dev->GspGraphicsRunlistBase+0x90),2);
    dev->Nvx3dLastFence=fenceValue;
    /* A valid GPGet is always an index in the RM-allocated graphics ring.
       Treat an out-of-range value as a channel fault immediately; waiting
       through the generic 10-second fence budget otherwise freezes WGL after
       the USERD state is already unusable. Keep a one-second bound for a
       valid but genuinely slow submission. */
    for(wait=0;wait<10000;wait++) {
        if(fence[0]==(ULONG)fenceValue) {
            __sync_synchronize();dev->Nvx3dCompletedFence=fenceValue;
            if(drawTriangle&&
               (fence[1]!=(ULONG)fenceValue||fence[2]!=(ULONG)fenceValue||
                fence[3]!=(ULONG)fenceValue||fence[4]!=(ULONG)fenceValue||
                fence[5]!=(ULONG)fenceValue||
                fence[7]!=(ULONG)fenceValue||fence[8]!=(ULONG)fenceValue||
                fence[10]!=(ULONG)fenceValue||
                fence[11]!=(ULONG)fenceValue||fence[12]!=(ULONG)fenceValue||
                fence[13]!=(ULONG)fenceValue||fence[14]!=(ULONG)fenceValue||
                fence[15]!=(ULONG)fenceValue)) {
                LogStr("NVX [3D-GR]: successful draw missed checkpoint(s) value=");
                LogHex(fenceValue);LogStr(" markers=");
                LogHex(fence[15]);LogStr(",");LogHex(fence[14]);
                LogStr(",");LogHex(fence[13]);LogStr(",");LogHex(fence[12]);
                LogStr(",");LogHex(fence[11]);LogStr(",");LogHex(fence[10]);
                LogStr(",");
                LogHex(fence[8]);
                LogStr(",");LogHex(fence[7]);
                LogStr(",");LogHex(fence[5]);LogStr(",");LogHex(fence[4]);
                LogStr(",");LogHex(fence[3]);LogStr(",");LogHex(fence[2]);
                LogStr(",");LogHex(fence[1]);LogStr("\n");
            }
            if(drawTriangle&&depthMode&&targetWidth==64U&&targetHeight==16U) {
                volatile ULONG *colorReadback=(volatile ULONG *)(scratch+0xe000);
                volatile ULONG *depthReadback=(volatile ULONG *)(scratch+0x6000);
                ULONG matching=0,white=0,depthWrite=0;
                ULONG rawOne=0,rawZero=0,rawQuarter=0,rawHalf=0;
                ULONG rawThreeQuarter=0;
                ULONG depthMin=0xffffffffU,depthMax=0,firstDepth=0;
                if(NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xe000,
                                      dstVa,256,256,256,16)) {
                    for(i=0;i<NVX_3D_READBACK_PIXELS;i++) {
                        ULONG pixel=NvxGspReadBlockLinearPixel(
                            (volatile UCHAR *)colorReadback,i%64U,i/64U,64);
                        if((pixel&0x00ffffffU)==(triangleColor&0x00ffffffU))
                            matching++;
                        if(pixel==0xffffffffU) white++;
                    }
                }
                if(NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0x6000,
                                      depthVa,256,256,256,16)) {
                    for(i=0;i<NVX_3D_READBACK_PIXELS;i++) {
                        ULONG raw=depthReadback[i];
                        if(raw==0x3f800000U) rawOne++;
                        else if(raw==0) rawZero++;
                        else if(raw==0x3e800000U) rawQuarter++;
                        else if(raw==0x3f000000U) rawHalf++;
                        else if(raw==0x3f400000U) rawThreeQuarter++;
                    }
                    for(i=0;i<NVX_3D_READBACK_PIXELS;i++) {
                        ULONG value=NvxGspReadDepthBlockLinearPixel(
                            (volatile UCHAR *)depthReadback,i%64U,i/64U,64);
                        if(value!=0x3f800000U) {
                            if(!depthWrite) firstDepth=value;
                            if(value<depthMin) depthMin=value;
                            if(value>depthMax) depthMax=value;
                            depthWrite++;
                        }
                    }
                }
                LogStr("NVX [3D-DEPTH]: input_z=");
                LogHex(((volatile ULONG *)(scratch+0xb800))[2]);LogStr(",");
                LogHex(((volatile ULONG *)(scratch+0xb800))[10]);LogStr(",");
                LogHex(((volatile ULONG *)(scratch+0xb800))[18]);
                LogStr(" raw_words one=");LogHex(rawOne);
                LogStr(" zero=");LogHex(rawZero);
                LogStr(" quarter=");LogHex(rawQuarter);
                LogStr(" half=");LogHex(rawHalf);
                LogStr(" three_quarter=");LogHex(rawThreeQuarter);
                LogStr(" depth_first=");LogHex(firstDepth);
                LogStr(" depth_min=");LogHex(depthMin);
                LogStr(" depth_max=");LogHex(depthMax);LogStr("\n");
                LogStr("NVX [3D-DEPTH]: draw color=");LogHex(triangleColor);
                LogStr(" color_pixels=");LogHex(matching);
                LogStr(" white_pixels=");LogHex(white);
                LogStr(" changed_depth_pixels=");LogHex(depthWrite);
                LogStr(" center_color=");LogHex(NvxGspReadBlockLinearPixel(
                    (volatile UCHAR *)colorReadback,32,8,64));
                LogStr(" center_depth=");LogHex(NvxGspReadDepthBlockLinearPixel(
                    (volatile UCHAR *)depthReadback,32,8,64));LogStr("\n");
            }
            if(drawTriangle&&!preserveTarget) {
                volatile ULONG *readback=(volatile ULONG *)(scratch+0xe000);
                ULONG drawn=0,drawnNoAlpha=0,white=0,black=0,other=0;
                ULONG firstOther=0,firstOtherIndex=0xffffffffU;
                ULONG drawnMinX=64,drawnMinY=16,drawnMaxX=0,drawnMaxY=0;
                VideoPortZeroMemory((PVOID)readback,4096);
                if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xe000,
                                       dstVa,256,256,256,16)) {
                    LogStr("NVX [3D-TRI]: CE target readback submission failed.\n");
                    __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
                }
                for(i=0;i<1024;i++) {
                    ULONG x=i%64U,y=i/64U;
                    ULONG pixel=NvxGspReadBlockLinearPixel((volatile UCHAR *)readback,x,y,64);
                    if(pixel==triangleColor) drawn++;
                    if((pixel&0x00ffffffU)==(triangleColor&0x00ffffffU)&&
                       pixel!=triangleColor) {
                        drawnNoAlpha++;
                        if(x<drawnMinX) drawnMinX=x;
                        if(y<drawnMinY) drawnMinY=y;
                        if(x>drawnMaxX) drawnMaxX=x;
                        if(y>drawnMaxY) drawnMaxY=y;
                    }
                    if(pixel==0xffffffffU) white++;
                    if(pixel==0) black++;
                    if(pixel!=triangleColor&&
                       (pixel&0x00ffffffU)!=(triangleColor&0x00ffffffU)&&
                       pixel!=0xffffffffU&&pixel!=0) {
                        if(firstOtherIndex==0xffffffffU) {
                            firstOther=pixel;firstOtherIndex=i;
                        }
                        other++;
                    }
                }
                if(greenPixels) *greenPixels=drawn+drawnNoAlpha;
                if(whitePixels) *whitePixels=white;
                /* This head-surface fragment program writes RGB but leaves
                   alpha zero on this target format. Accept either observed
                   alpha encoding while still requiring a mixed full-target
                   triangle/background partition. */
                if(drawn+drawnNoAlpha<24||!white||
                   drawn+drawnNoAlpha+white!=1024) {
                    LogStr("NVX [3D-TRI]: pixel readback mismatch drawn=");LogHex(drawn);
                    LogStr(" drawn_no_alpha=");LogHex(drawnNoAlpha);
                    LogStr(" white=");LogHex(white);LogStr(" black=");LogHex(black);
                    LogStr(" other=");LogHex(other);
                    LogStr(" first_other=");LogHex(firstOther);
                    LogStr("@pixel");LogHex(firstOtherIndex);
                    LogStr(" first=");
                    LogHex(NvxGspReadBlockLinearPixel((volatile UCHAR *)readback,0,0,64));
                    LogStr(" mid=");LogHex(NvxGspReadBlockLinearPixel((volatile UCHAR *)readback,32,8,64));
                    LogStr(" last=");LogHex(NvxGspReadBlockLinearPixel((volatile UCHAR *)readback,63,15,64));
                    LogStr(" draw_bounds=");LogHex(drawnMinX);LogStr(",");
                    LogHex(drawnMinY);LogStr("-");LogHex(drawnMaxX);LogStr(",");
                    LogHex(drawnMaxY);
                    LogStr("\n");
                    __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
                }
                dev->GspGraphicsTriangleVerified=TRUE;
            }
            if(!drawTriangle&&whitePixels) {
                volatile ULONG *readback=(volatile ULONG *)(scratch+0xe000);
                ULONG matches=0,black=0,other=0,expected=color;
                if(targetWidth==64U&&targetHeight==16U) {
                    VideoPortZeroMemory((PVOID)readback,4096);
                    if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xe000,
                                           dstVa,256,256,256,16)) {
                        LogStr("NVX [3D-CLEAR]: CE target readback submission failed.\n");
                        __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
                    }
                    for(i=0;i<1024;i++) {
                        ULONG x=i%64U,y=i/64U;
                        ULONG pixel=NvxGspReadBlockLinearPixel(
                            (volatile UCHAR *)readback,x,y,64);
                        if(pixel==expected) matches++;
                        else if(pixel==0) black++;
                        else other++;
                    }
                    *whitePixels=matches;
                    LogStr("NVX [3D-CLEAR]: expected=");LogHex(expected);
                    LogStr(" matching_pixels=");LogHex(matches);
                    LogStr(" black=");LogHex(black);LogStr(" other=");LogHex(other);
                    LogStr(" samples=");
                    LogHex(NvxGspReadBlockLinearPixel(
                        (volatile UCHAR *)readback,0,0,64));
                    LogStr(",");LogHex(NvxGspReadBlockLinearPixel(
                        (volatile UCHAR *)readback,7,7,64));
                    LogStr(",");LogHex(NvxGspReadBlockLinearPixel(
                        (volatile UCHAR *)readback,8,0,64));
                    LogStr(",");LogHex(NvxGspReadBlockLinearPixel(
                        (volatile UCHAR *)readback,0,8,64));
                    LogStr(",");LogHex(NvxGspReadBlockLinearPixel(
                        (volatile UCHAR *)readback,63,15,64));
                    LogStr("\n");
                } else {
                    static const ULONG sampleXY[5][2]={{0,0},{1,0},{0,1},
                        {0xffffffffU,0xffffffffU},{0xfffffffeU,0xfffffffeU}};
                    for(i=0;i<5;i++) {
                        ULONG x=sampleXY[i][0],y=sampleXY[i][1];
                        ULONG offset,pixel;
                        if(i>=3) {
                            x=targetWidth>(i==3?1U:2U)?
                                targetWidth-(i==3?1U:2U):0;
                            y=targetHeight>(i==3?1U:2U)?
                                targetHeight-(i==3?1U:2U):0;
                        }
                        offset=NvxGspBlockLinearPixelOffsetEx(
                            x,y,targetWidth,targetTileMode);
                        VideoPortZeroMemory((PVOID)readback,4);
                        if(!NvxGspSubmitCopy2DLocked(dev,
                                dev->GspScratchGpuVa+0xe000,
                                dstVa+offset,4,4,4,1)) {
                            LogStr("NVX [3D-SURFACE]: sample readback failed.\n");
                            __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
                        }
                        pixel=readback[0];
                        if(pixel!=expected) {
                            LogStr("NVX [3D-SURFACE]: clear sample mismatch x=");
                            LogHex(x);LogStr(" y=");LogHex(y);
                            LogStr(" expected=");LogHex(expected);
                            LogStr(" actual=");LogHex(pixel);LogStr("\n");
                            __sync_lock_release(&NvxGrUserSubmitBusy);return FALSE;
                        }
                        matches++;
                    }
                    *whitePixels=matches;
                    LogStr("NVX [3D-SURFACE]: C797 block-linear clear verified at five corners/edge samples; width=");
                    LogHex(targetWidth);LogStr(" height=");LogHex(targetHeight);
                    LogStr(" tileMode=");LogHex(targetTileMode);LogStr("\n");
                }
            }
            __sync_lock_release(&NvxGrUserSubmitBusy);return TRUE;
        }
        if(userd[0x88/4]>=512U) {
            LogStr("NVX [3D-GR]: invalid USERD GPGet during fence wait=");
            LogHex(userd[0x88/4]);LogStr(" GPPut=");
            LogHex(userd[0x8c/4]);LogStr("\n");
            break;
        }
        VideoPortStallExecution(100);
    }
    {
        volatile ULONG64 *grFifo=(volatile ULONG64 *)(scratch+0x9000);
        ULONG64 entry,entryPushVa;
        ULONG entryWords,entryIndex,submittedIndex;
        /* Capture the hardware-side PBDMA state while the channel is known to
           be stalled; USERD alone cannot distinguish a bad pointer from a
           command/method fault. This helper only reads BAR0 registers. */
        NvxGspLogPbdmaFault(dev);
        gpGetSnapshot=userd[0x88/4];
        gpPutSnapshot=userd[0x8c/4];
        LogStr("NVX [3D-GR]: semaphore timeout fence=");LogHex(fence[0]);
        LogStr(" bind_checkpoint=");LogHex(fence[10]);
        LogStr(" format_checkpoint=");LogHex(fence[11]);
        LogStr(" zselect_checkpoint=");LogHex(fence[12]);
        LogStr(" point_checkpoint=");LogHex(fence[13]);
        LogStr(" sampler_checkpoint=");LogHex(fence[14]);
        LogStr(" viewport_scale_checkpoint=");LogHex(fence[15]);
        LogStr(" viewport_mid_checkpoint=");LogHex(fence[8]);
        LogStr(" viewport_checkpoint=");LogHex(fence[7]);
        LogStr(" pretarget_checkpoint=");LogHex(fence[5]);
        LogStr(" target_checkpoint=");LogHex(fence[4]);
        LogStr(" color_checkpoint=");LogHex(fence[3]);
        LogStr(" state_checkpoint=");LogHex(fence[2]);
        LogStr(" draw_checkpoint=");LogHex(fence[1]);
        LogStr(" expected=");LogHex(fenceValue);LogStr(" GPGet=");LogHex(gpGetSnapshot);
        LogStr(" GPPut=");LogHex(gpPutSnapshot);LogStr("\n");
        entryIndex=gpGetSnapshot%512U;
        submittedIndex=(gpPutSnapshot+511U)%512U;
        entry=grFifo[entryIndex];
        entryPushVa=((entry&0xffffffffULL)&~3ULL)|(((entry>>32)&0xffULL)<<32);
        entryWords=(ULONG)((entry>>42)&0x3fffffULL);
        LogStr("NVX [3D-GR]: stalled GPFIFO index=");LogHex(entryIndex);
        LogStr(" entry=");LogHex(entry);LogStr(" pushVA=");LogHex(entryPushVa);
        LogStr(" words=");LogHex(entryWords);
        if(entryPushVa>=dev->GspScratchGpuVa&&
           entryPushVa+entryWords*4ULL<=dev->GspScratchGpuVa+GSP_SCRATCH_SIZE) {
            PULONG stuckPush=(PULONG)(scratch+(entryPushVa-dev->GspScratchGpuVa));
            LogStr(" firstMethods=");
            for(i=0;i<entryWords&&i<8;i++) {LogHex(stuckPush[i]);LogStr(" ");}
        }
        LogStr("\n");
        entry=grFifo[submittedIndex];
        entryPushVa=((entry&0xffffffffULL)&~3ULL)|(((entry>>32)&0xffULL)<<32);
        entryWords=(ULONG)((entry>>42)&0x3fffffULL);
        LogStr("NVX [3D-GR]: submitted GPFIFO index=");LogHex(submittedIndex);
        LogStr(" entry=");LogHex(entry);LogStr(" pushVA=");LogHex(entryPushVa);
        LogStr(" words=");LogHex(entryWords);
        if(entryPushVa>=dev->GspScratchGpuVa&&
           entryPushVa+entryWords*4ULL<=dev->GspScratchGpuVa+GSP_SCRATCH_SIZE&&
           entryWords>=16U) {
            PULONG stuckPush=(PULONG)(scratch+(entryPushVa-dev->GspScratchGpuVa));
            LogStr(" tailMethods=");
            for(i=entryWords-16U;i<entryWords;i++) {
                LogHex(stuckPush[i]);LogStr(" ");
            }
            LogStr("\nNVX [3D-GR]: pending pushbuffer=");
            for(i=0;i<entryWords;i++) {LogHex(stuckPush[i]);LogStr(" ");}
        }
        LogStr("\n");
        if(drawTriangle&&depthMode&&vertexCount<=NVX_3D_MAX_BATCH_VERTICES) {
            PULONG source=(PULONG)(scratch+0xb800);
            PULONG copied=(PULONG)(scratch+0x6000);
            LogStr("NVX [3D-GR]: depth vertices count=");LogHex(vertexCount);
            LogStr(" source=");
            for(i=0;i<vertexCount*8U;i++) {LogHex(source[i]);LogStr(" ");}
            LogStr(" copied=");
            for(i=0;i<vertexCount*8U;i++) {LogHex(copied[i]);LogStr(" ");}
            LogStr("\n");
        }
    }
    LogStr("NVX [3D-SUBMIT]: semaphore timeout draw=");LogHex(drawTriangle);
    LogStr(" preserve=");LogHex(preserveTarget);
    LogStr(" depthMode=");LogHex(depthMode);
    LogStr(" target=");LogHex(targetWidth);LogStr("x");LogHex(targetHeight);
    LogStr(" tileMode=");LogHex(targetTileMode);
    LogStr(" color=");LogHex(triangleColor);
    LogStr(" fence=");LogHex(fence[0]);LogStr(" expected=");LogHex(fenceValue);
    LogStr(" GPGet=");LogHex(userd[0x88/4]);
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\n");
    if(depthMode) {
        LogStr("NVX [3D-DEPTH]: semaphore timeout depthMode=");LogHex(depthMode);
        LogStr(" fence=");LogHex(fence[0]);LogStr(" expected=");LogHex(fenceValue);
        LogStr(" GPGet=");LogHex(userd[0x88/4]);
        LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\n");
    }
    dev->GspGraphicsChannelFaulted=TRUE;
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return FALSE;
}

BOOLEAN NvxGspSubmitUserClear(PVOID HwDeviceExtension,ULONG64 dstVa,
                              ULONG color,ULONG64 fenceValue,
                              ULONG *matchingPixels)
{
    return NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,0,color,
                                     fenceValue,FALSE,FALSE,0,NVX_3D_ZT_FORMAT_Z24S8,
                                     0,0,0,0,64U,16U,0,NULL,matchingPixels);
}

BOOLEAN NvxGspSubmitUserColorTargetClear(PVOID HwDeviceExtension,
    ULONG64 dstVa,ULONG width,ULONG height,ULONG tileMode,ULONG color,
    ULONG64 fenceValue,ULONG *matchingSamples)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    ULONG offset=0,wait;
    if(matchingSamples) *matchingSamples=0;
    if(!dev||!dstVa||(dstVa&0xffffULL)||!fenceValue||
       fenceValue>0xffffffffULL||
       !Nvx3dComputeColorSurfaceLayout(width,height,&layout)||
       layout.TileMode!=tileMode) return FALSE;

    /* A uniform pixel value is invariant under the target's block-linear
       swizzle. Fill the complete padded allocation through the verified CE
       path instead of invoking C797 CLEAR_SURFACE on every glClear. Besides
       avoiding repeated large rasterizer clears, this removes the five
       per-clear sample readbacks previously used to validate that method. */
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) return FALSE;
    while(offset<layout.Bytes) {
        ULONG chunk=layout.Bytes-offset;
        if(chunk>0x01000000U) chunk=0x01000000U;
        chunk&=~3U;
        if(!chunk||!NvxGspSubmitFillLocked(dev,dstVa+offset,color,chunk)) {
            __sync_lock_release(&NvxGrUserSubmitBusy);
            return FALSE;
        }
        offset+=chunk;
    }
    dev->Nvx3dCompletedFence=fenceValue;
    __sync_synchronize();
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return TRUE;
}

/* Translate the narrowly validated Nouveau/NVC0 clear packet into the
   miniport-owned C797 command stream. The user packet is never submitted to
   the hardware GPFIFO. */
BOOLEAN NvxGspSubmitNvc0ColorTargetClear(PVOID HwDeviceExtension,
    ULONG64 dstVa,ULONG width,ULONG height,ULONG tileMode,ULONG color,
    ULONG64 fenceValue)
{
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    if(!Nvx3dComputeColorSurfaceLayout(width,height,&layout) ||
       layout.TileMode!=tileMode || layout.Bytes>NVX_3D_MAX_BUFFER_BYTES)
        return FALSE;
    return NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,0,color,
        fenceValue,FALSE,FALSE,0,NVX_3D_ZT_FORMAT_Z24S8,0,0,0,0,
        width,height,tileMode,NULL,NULL);
}

/* Submit the one narrowly validated NVC0 clear packet directly. The caller
 * has already matched its target address/layout and exact 28-dword method
 * profile against an owned BO. The kernel adds SET_OBJECT, idle, and release
 * semaphore methods; no caller-supplied method outside that profile reaches
 * the channel. */
BOOLEAN NvxGspSubmitNvc0NativeClear(PVOID HwDeviceExtension,
    const uint32_t *nvc0Dwords,ULONG64 fenceValue)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *userd,*fence;
    volatile ULONG64 *fifo;
    PULONG push;
    ULONG n=0,i,put,wait;
    ULONG64 pushVa,fenceVa;

    if(!dev||!nvc0Dwords||!dev->GspInitialized||
       !dev->GspGraphicsChannelReady||!dev->GspGraphicsCommandVerified||
       dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued||
       !dev->GspDmaVirtual||!dev->GspGraphicsRunlistBase||
       !fenceValue||fenceValue>0xffffffffULL)
        return FALSE;

    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) return FALSE;

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    userd=(volatile ULONG *)(scratch+0xb400);
    fence=(volatile ULONG *)(scratch+0x5000);
    fifo=(volatile ULONG64 *)(scratch+0x9000);
    push=(PULONG)(scratch+0xf000);
    pushVa=dev->GspScratchGpuVa+0xf000;
    fenceVa=dev->GspScratchGpuVa+0x5000;
    for(i=0;i<16;i++) fence[i]=0;
    __sync_synchronize();

#define NVX_NATIVE_INCR(_m,_c) \
    (push[n++]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|((_c)<<16)|((_m)>>2))
    NVX_NATIVE_INCR(NVC56F_SET_OBJECT,1);
    push[n++]=0xC797U;
    for(i=0;i<NVX_3D_NVC0_CLEAR_DWORDS;i++)
        push[n++]=nvc0Dwords[i];
    NVX_NATIVE_INCR(NVC797_WAIT_FOR_IDLE,1);
    push[n++]=0;
    NVX_NATIVE_INCR(NVC797_SET_REPORT_SEMAPHORE_A,4);
    push[n++]=(ULONG)((fenceVa>>32)&0xffffffffU);
    push[n++]=(ULONG)(fenceVa&0xffffffffU);
    push[n++]=(ULONG)fenceValue;
    push[n++]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
              DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
              DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
              DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
#undef NVX_NATIVE_INCR

    if(n>NVX_3D_MAX_PUSH_DWORDS) {
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    put=dev->GspGraphicsGPPut%512U;
    fifo[put]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|
              ((ULONG64)n<<42);
    __sync_synchronize();
    dev->GspGraphicsGPPut=(put+1U)%512U;
    dev->Nvx3dLastFence=fenceValue;
    userd[0x8c/4]=dev->GspGraphicsGPPut;
    __sync_synchronize();
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                                     0x30090),dev->GspGraphicsSubmitToken);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                              dev->GspGraphicsRunlistBase+0x90),2);

    for(wait=0;wait<10000;wait++) {
        if(fence[0]==(ULONG)fenceValue) {
            __sync_synchronize();
            dev->Nvx3dCompletedFence=fenceValue;
            __sync_lock_release(&NvxGrUserSubmitBusy);
            return TRUE;
        }
        if(dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued)
            break;
        VideoPortStallExecution(100);
    }
    LogStr("NVX [NVC0-NATIVE]: validated clear stream did not complete; fence=");
    LogHex(fenceValue);LogStr(" observed=");LogHex(fence[0]);LogStr("\n");
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return FALSE;
}

static BOOLEAN NvxGspFindNvc0Draw(const ULONG *dwords,ULONG dwordCount,
    ULONG *primitive,ULONG *vertexStart,ULONG *vertexCount)
{
    ULONG cursor=0,found=0;
    if(!dwords||!primitive||!vertexStart||!vertexCount) return FALSE;
    *primitive=0xffffffffU;*vertexStart=0xffffffffU;*vertexCount=0;
    while(cursor<dwordCount) {
        ULONG header=dwords[cursor],type=header&0xe0000000U;
        ULONG count,method,subchannel,j;
        if(type==0x80000000U) { cursor++;continue; }
        if(type!=0x20000000U&&type!=0x60000000U&&type!=0xa0000000U)
            break;
        count=(header>>16)&0x1fffU;
        method=(header&0x1fffU)<<2;
        subchannel=(header>>13)&7U;
        if(!count||count>dwordCount-cursor-1U) break;
        if(subchannel==0U) {
            for(j=0;j<count;j++) {
                ULONG current=method;
                if(type==0x20000000U) current+=j*4U;
                else if(type==0xa0000000U&&j) current+=4U;
                if(current==0x1618U) {
                    *primitive=dwords[cursor+1U+j];found=1U;
                } else if(current==0x0d74U) {
                    *vertexStart=dwords[cursor+1U+j];
                } else if(current==0x0d78U) {
                    *vertexCount=dwords[cursor+1U+j];
                }
            }
        }
        cursor+=count+1U;
    }
    return found?TRUE:FALSE;
}

BOOLEAN NvxGspSubmitNvc0Batch(PVOID HwDeviceExtension,
    const uint32_t *nvc0Dwords,ULONG dwordCount,
    PVOID pushVirtual,ULONG64 pushGpuVa,ULONG pushBytes,ULONG64 fenceValue)
{
    static ULONG successTraceCount=0;
    static ULONG drawTraceCount=0;
    static ULONG cubeDrawTraceCount=0;
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *userd,*fence;
    volatile ULONG64 *fifo;
    PULONG push;
    ULONG put,wait,tailCount=7U,pushBytesNeeded,totalDwords,i;
    ULONG64 fenceVa,pushVa,fifoEntry;
    if(!dev||!nvc0Dwords||!dwordCount||dwordCount>NVX_3D_MAX_PUSH_DWORDS||
       !pushVirtual||!pushGpuVa||!pushBytes||!dev->GspInitialized||
       !dev->GspGraphicsChannelReady||!dev->GspGraphicsCommandVerified||
       dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued||
       !dev->GspDmaVirtual||!dev->GspGraphicsRunlistBase||
       !fenceValue||fenceValue>0xffffffffULL||
       dwordCount>(0xffffffffU/sizeof(ULONG))-tailCount)
        return FALSE;
    pushBytesNeeded=(dwordCount+tailCount)*sizeof(ULONG);
    if(pushBytesNeeded>pushBytes) return FALSE;
    for(wait=0;wait<20000U;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10U);
    }
    if(wait==20000U) return FALSE;

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    userd=(volatile ULONG *)(scratch+0xb400);
    fence=(volatile ULONG *)(scratch+0x5000);
    fifo=(volatile ULONG64 *)(scratch+0x9000);
    push=(PULONG)pushVirtual;
    pushVa=pushGpuVa;
    fenceVa=dev->GspScratchGpuVa+0x5000;
    VideoPortZeroMemory((PVOID)fence,16U*sizeof(ULONG));
    VideoPortMoveMemory(push,(PVOID)nvc0Dwords,
                        dwordCount*sizeof(ULONG));
    __sync_synchronize();
    push[dwordCount+0]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|
            (NVC797_WAIT_FOR_IDLE>>2);
    push[dwordCount+1]=0U;
    push[dwordCount+2]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(4U<<16)|
            (NVC797_SET_REPORT_SEMAPHORE_A>>2);
    push[dwordCount+3]=(ULONG)(fenceVa>>32);
    push[dwordCount+4]=(ULONG)fenceVa;
    push[dwordCount+5]=(ULONG)fenceValue;
    push[dwordCount+6]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
            DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
            DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
            DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    totalDwords=dwordCount+tailCount;
    __sync_synchronize();

    put=dev->GspGraphicsGPPut%512U;
    fifoEntry=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|
              ((ULONG64)totalDwords<<42);
    fifo[put]=fifoEntry;
    __sync_synchronize();
    dev->GspGraphicsGPPut=(put+1U)%512U;
    dev->Nvx3dLastFence=fenceValue;
    userd[0x8c/4]=dev->GspGraphicsGPPut;
    __sync_synchronize();
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                                     0x30090),dev->GspGraphicsSubmitToken);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                              dev->GspGraphicsRunlistBase+0x90),2U);
    for(wait=0;wait<10000U;wait++) {
        if(fence[0]==(ULONG)fenceValue) {
            __sync_synchronize();
            dev->Nvx3dCompletedFence=fenceValue;
            if(drawTraceCount<8U) {
                ULONG primitive,vertexStart,vertexCount;
                if(NvxGspFindNvc0Draw(nvc0Dwords,dwordCount,&primitive,
                                      &vertexStart,&vertexCount)) {
                    LogStr("NVX [NVC0-DRAW]: completed index=");
                    LogHex(drawTraceCount++);LogStr(" sourceVA=");
                    LogHex(pushGpuVa);LogStr(" dwords=");LogHex(dwordCount);
                    LogStr(" primitive=");LogHex(primitive);
                    LogStr(" start=");LogHex(vertexStart);
                    LogStr(" count=");LogHex(vertexCount);
                    LogStr(" fence=");LogHex(fenceValue);
                    LogStr(" observed=");LogHex(fence[0]);LogStr("\n");
                }
            }
            if(cubeDrawTraceCount<4U) {
                ULONG primitive,vertexStart,vertexCount;
                if(NvxGspFindNvc0Draw(nvc0Dwords,dwordCount,&primitive,
                                      &vertexStart,&vertexCount)&&
                   primitive==7U&&vertexCount==24U) {
                    LogStr("NVX [NVC0-CUBE-DRAW]: completed index=");
                    LogHex(cubeDrawTraceCount++);LogStr(" sourceVA=");
                    LogHex(pushGpuVa);LogStr(" start=");LogHex(vertexStart);
                    LogStr(" count=");LogHex(vertexCount);
                    LogStr(" fence=");LogHex(fenceValue);
                    LogStr(" observed=");LogHex(fence[0]);LogStr("\n");
                }
            }
            if(successTraceCount==0) {
                ULONG traceIndex;
                successTraceCount++;
                LogStr("NVX [NVC0-BATCH]: first completion sourceVA=");
                LogHex(pushGpuVa);LogStr(" submittedVA=");LogHex(pushVa);
                LogStr(" dwords=");LogHex(totalDwords);
                LogStr(" gpGet=");LogHex(userd[0x88/4]);
                LogStr(" gpPut=");LogHex(userd[0x8c/4]);
                LogStr(" fence=");LogHex(fence[0]);LogStr("\n");
                for(traceIndex=0;traceIndex<totalDwords;traceIndex++) {
                    if((traceIndex&7U)==0) {
                        LogStr("NVX [NVC0-BATCH]: stream[");
                        LogHex(traceIndex);LogStr("]");
                    }
                    LogStr(" ");LogHex(push[traceIndex]);
                    if((traceIndex&7U)==7U||traceIndex+1U==totalDwords)
                        LogStr("\n");
                }
                NvxGspLogPbdmaFault(dev);
            }
            __sync_lock_release(&NvxGrUserSubmitBusy);
            return TRUE;
        }
        if(dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued) break;
        VideoPortStallExecution(100U);
    }
    if(fence[0]!=(ULONG)fenceValue&&!dev->GspGraphicsChannelFaulted&&
       !dev->GspMmuFaultQueued) {
        ULONG64 visibleEntry=fifo[put];
        LogStr("NVX [NVC0-BATCH]: retry doorbell fifoIndex=");LogHex(put);
        LogStr(" expectedEntry=");LogHex(fifoEntry);
        LogStr(" visibleEntry=");LogHex(visibleEntry);
        LogStr(" USERD_GET=");LogHex(userd[0x88/4]);
        LogStr(" USERD_PUT=");LogHex(userd[0x8c/4]);LogStr("\n");
        __sync_synchronize();
        if(dev->Bar0Length>=0x30094)
            VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                                         0x30090),dev->GspGraphicsSubmitToken);
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
                                  dev->GspGraphicsRunlistBase+0x90),2U);
        for(wait=0;wait<10000U;wait++) {
            if(fence[0]==(ULONG)fenceValue) {
                __sync_synchronize();
                dev->Nvx3dCompletedFence=fenceValue;
                LogStr("NVX [NVC0-BATCH]: second doorbell completed fence=");
                LogHex(fenceValue);LogStr(" wait=" );LogHex(wait);LogStr("\n");
                __sync_lock_release(&NvxGrUserSubmitBusy);
                return TRUE;
            }
            if(dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued) break;
            VideoPortStallExecution(100U);
        }
    }
    LogStr("NVX [NVC0-BATCH]: timeout state fifoIndex=");LogHex(put);
    LogStr(" fifoEntry=");LogHex(fifoEntry);LogStr(" pushVa=");LogHex(pushVa);
    LogStr(" dwords=");LogHex(totalDwords);LogStr(" pushBytes=");LogHex(pushBytes);
    LogStr(" userdGet=");LogHex(userd[0x88/4]);
    LogStr(" userdPut=");LogHex(userd[0x8c/4]);
    LogStr(" expectedPut=");LogHex(dev->GspGraphicsGPPut);LogStr(" fence=");
    LogHex(fenceValue);LogStr(" observed=");LogHex(fence[0]);LogStr("\n");
    /* Capture the exact translated stream that faulted. Earlier diagnostics
       only logged its first eight words and depended on a bounded Mesa-side
       trace, which often stopped before the failing draw. */
    for(i=0;i<totalDwords&&i<256U;i++) {
        if((i&7U)==0) {
            LogStr("NVX [NVC0-BATCH]: failed-stream[");
            LogHex(i);LogStr("]");
        }
        LogStr(" ");LogHex(push[i]);
        if((i&7U)==7U||i+1U==totalDwords||i+1U==256U)
            LogStr("\n");
    }
    for(i=0;i<tailCount;i++) {
        LogStr("NVX [NVC0-BATCH]: tail[");LogHex(i);LogStr("]=");
        LogHex(push[dwordCount+i]);LogStr("\n");
    }
    NvxGspLogPbdmaFault(dev);
    if(dev->GspGraphicsRunlistBase&&dev->Bar0Length>=
       dev->GspGraphicsRunlistBase+0x10U) {
        ULONG channelConfig=VideoPortReadRegisterUlong((PULONG)
            ((PUCHAR)dev->Bar0MmioVirtual+dev->GspGraphicsRunlistBase+4U));
        ULONG chramBase=channelConfig&0xfffffff0U;
        ULONG channelId=dev->GspGraphicsSubmitToken&0x7ffU;
        ULONG chramOffset=chramBase+channelId*4U;
        if(chramOffset<=dev->Bar0Length-4U) {
            ULONG channelState=VideoPortReadRegisterUlong((PULONG)
                ((PUCHAR)dev->Bar0MmioVirtual+chramOffset));
            LogStr("NVX [CHRAM]: config=");LogHex(channelConfig);
            LogStr(" base=");LogHex(chramBase);LogStr(" chid=");LogHex(channelId);
            LogStr(" state=");LogHex(channelState);LogStr("\n");
        }
    }
    if(dev->Bar0Length>=0x400140) {
        ULONG graphIntr=VideoPortReadRegisterUlong((PULONG)
            ((PUCHAR)dev->Bar0MmioVirtual+0x400100));
        ULONG graphTrap=VideoPortReadRegisterUlong((PULONG)
            ((PUCHAR)dev->Bar0MmioVirtual+0x400108));
        ULONG graphTrapEnable=VideoPortReadRegisterUlong((PULONG)
            ((PUCHAR)dev->Bar0MmioVirtual+0x400138));
        ULONG graphIntrEnable=VideoPortReadRegisterUlong((PULONG)
            ((PUCHAR)dev->Bar0MmioVirtual+0x40013c));
        LogStr("NVX [PGRAPH]: INTR=");LogHex(graphIntr);
        LogStr(" TRAP=");LogHex(graphTrap);
        LogStr(" TRAP_EN=");LogHex(graphTrapEnable);
        LogStr(" INTR_EN=");LogHex(graphIntrEnable);LogStr("\n");
    }
    dev->GspGraphicsChannelFaulted=TRUE;
    LogStr("NVX [NVC0-BATCH]: submission did not complete; fence=");
    LogHex(fenceValue);LogStr(" observed=");LogHex(fence[0]);LogStr("\n");
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return FALSE;
}

BOOLEAN NvxGspSubmitUserColorTargetTriangles(PVOID HwDeviceExtension,
    ULONG64 dstVa,ULONG width,ULONG height,ULONG tileMode,ULONG64 vertexVa,
    ULONG vertexCount,ULONG color,ULONG64 fenceValue,BOOLEAN verifyFullTarget)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *readback;
    static const ULONG sampleXY[5][2]={{0,0},{1,0},{0,1},
        {0xffffffffU,0xffffffffU},{0xfffffffeU,0xfffffffeU}};
    ULONG i,offset,pixel;
    if(!vertexVa||vertexCount<3U||vertexCount>NVX_3D_MAX_BATCH_VERTICES||
       vertexCount%3U) return FALSE;
    if(!NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,0,0,
        fenceValue,TRUE,TRUE,0,NVX_3D_ZT_FORMAT_Z24S8,0,vertexVa,
        vertexCount,color,width,height,tileMode,NULL,NULL)) return FALSE;
    if(!verifyFullTarget) return TRUE;
    if(!dev||!dev->GspDmaVirtual) return FALSE;
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    readback=(volatile ULONG *)(scratch+0xe000);
    for(i=0;i<5;i++) {
        ULONG x=sampleXY[i][0],y=sampleXY[i][1];
        if(i>=3) {
            x=width>(i==3?1U:2U)?width-(i==3?1U:2U):0;
            y=height>(i==3?1U:2U)?height-(i==3?1U:2U):0;
        }
        offset=Nvx3dTuringColorPixelOffset(x,y,width,tileMode);
        VideoPortZeroMemory((PVOID)readback,4);
        if(!NvxGspSubmitCopy2D(dev,dev->GspScratchGpuVa+0xe000,
                               dstVa+offset,4,4,4,1)) {
            LogStr("NVX [3D-SURFACE-DRAW]: sample copy failed.\n");
            return FALSE;
        }
        pixel=readback[0];
        if((pixel&0x00ffffffU)!=(color&0x00ffffffU)) {
            LogStr("NVX [3D-SURFACE-DRAW]: pixel mismatch x=");LogHex(x);
            LogStr(" y=");LogHex(y);LogStr(" expected=");
            LogHex(color&0x00ffffffU);LogStr(" actual=");
            LogHex(pixel);LogStr("\n");
            return FALSE;
        }
    }
    LogStr("NVX [3D-SURFACE-DRAW]: Turing target pixel samples verified.\n");
    return TRUE;
}

BOOLEAN NvxGspSubmitUserColorTargetDepthClear(PVOID HwDeviceExtension,
    ULONG64 depthVa,ULONG bytes,ULONG64 fenceValue)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    ULONG offset=0;
    if(!dev||!depthVa||(depthVa&0xffffULL)||!bytes||(bytes&3U)||
       !fenceValue||fenceValue>0xffffffffULL) return FALSE;
    while(offset<bytes) {
        ULONG chunk=bytes-offset;
        if(chunk>0x01000000U) chunk=0x01000000U;
        chunk&=~3U;
        if(!chunk||!NvxGspSubmitFill(dev,depthVa+offset,0x3f800000U,chunk))
            return FALSE;
        offset+=chunk;
    }
    dev->Nvx3dCompletedFence=fenceValue;
    __sync_synchronize();
    return TRUE;
}

BOOLEAN NvxGspSubmitUserColorTargetDepthTriangles(PVOID HwDeviceExtension,
    ULONG64 dstVa,ULONG width,ULONG height,ULONG tileMode,ULONG64 depthVa,
    ULONG depthPitchBytes,ULONG64 vertexVa,ULONG vertexCount,ULONG color,
    ULONG depthMode,ULONG64 fenceValue)
{
    if(!vertexVa||vertexCount<3U||vertexCount>NVX_3D_MAX_BATCH_VERTICES||
       vertexCount%3U||!depthVa||!depthPitchBytes||
       (depthMode!=2U&&depthMode!=3U&&depthMode!=5U)) return FALSE;
    return NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,depthVa,0,
        fenceValue,TRUE,TRUE,depthMode,NVX_3D_ZT_FORMAT_ZF32,depthPitchBytes,
        vertexVa,vertexCount,color,width,height,tileMode,NULL,NULL);
}

BOOLEAN NvxGspSubmitUserTriangle(PVOID HwDeviceExtension,ULONG64 dstVa,
                                 ULONG64 fenceValue,ULONG *greenPixels,
                                 ULONG *whitePixels)
{
    return NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,0,0,fenceValue,
                                     TRUE,FALSE,0,NVX_3D_ZT_FORMAT_Z24S8,
                                     0,0,3U,0xff00ff00U,64U,16U,0,
                                     greenPixels,whitePixels);
}

BOOLEAN NvxGspSubmitUserTriangleBuffer(PVOID HwDeviceExtension,ULONG64 dstVa,
    ULONG64 depthVa,ULONG64 vertexVa,ULONG color,ULONG vertexCount,
    BOOLEAN preserveTarget,BOOLEAN depthTest,
    ULONG depthFormat,
    ULONG64 fenceValue,
    ULONG *drawPixels,
    ULONG *whitePixels)
{
    if(!vertexVa) return FALSE;
    return NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,depthVa,0,fenceValue,
                                     TRUE,preserveTarget,depthTest?5:0,depthFormat,0,
                                     vertexVa,depthTest?3U:vertexCount,color,
                                     64U,16U,0,drawPixels,whitePixels);
}

BOOLEAN NvxGspSubmitUserDepthClear(PVOID HwDeviceExtension,ULONG64 depthVa,
                                   BOOLEAN zf32,ULONG64 fenceValue)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *depthInit;
    volatile ULONG *depthReadback;
    ULONG wait,i,mismatches=0;
    if(!dev||!fenceValue||fenceValue>0xffffffffULL||(depthVa&0xffffULL)||
       !dev->GspDmaVirtual||!dev->GspCopyVerified||dev->GspChannelFaulted||
       dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued) return FALSE;
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) return FALSE;
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    depthInit=(volatile ULONG *)(scratch+0xe000);
    depthReadback=(volatile ULONG *)(scratch+0x6000);
    /* Stencil is disabled by the bounded probe, so initialize the complete
       64x16 zeta surface with CE rather than using the C797 clear method. */
    for(i=0;i<NVX_3D_READBACK_PIXELS;i++)
        depthInit[i]=zf32?0x3f800000U:0xffffffffU;
    __sync_synchronize();
    if(!NvxGspSubmitCopy2DLocked(dev,
            depthVa,
            dev->GspScratchGpuVa+0xe000,256,256,256,16)) {
        LogStr("NVX [3D-DEPTH]: clear upload CE copy failed.");
        LogStr(" depth=");LogHex(depthVa);LogStr(" fence=");LogHex(fenceValue);LogStr("\n");
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    VideoPortZeroMemory((PVOID)depthReadback,4096);
    if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0x6000,
            depthVa,256,256,256,16)) {
        LogStr("NVX [3D-DEPTH]: clear readback CE copy failed.");
        LogStr(" depth=");LogHex(depthVa);LogStr(" fence=");LogHex(fenceValue);LogStr("\n");
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    for(i=0;i<NVX_3D_READBACK_PIXELS;i++)
        if(depthReadback[i]!=(zf32?0x3f800000U:0xffffffffU)) mismatches++;
    LogStr("NVX [3D-DEPTH]: direct-page clear readback mismatches=");LogHex(mismatches);
    LogStr(" sample=");LogHex(depthReadback[0]);LogStr("\n");
    if(mismatches) {
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    __sync_synchronize();
    dev->Nvx3dCompletedFence=fenceValue;
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return TRUE;
}

BOOLEAN NvxGspRunZf32Diagnostic(PVOID HwDeviceExtension,ULONG64 dstVa,
                                ULONG64 depthVa,ULONG64 vertexVa,
                                ULONG64 fenceValue)
{
    ULONG drawn=0,white=0;
    PUCHAR scratch;
    volatile ULONG *readback;
    ULONG i,green=0,red=0,black=0,whitePixels=0,other=0;
    ULONG minX=64,minY=16,maxX=0,maxY=0,firstOther=0;
    LogStr("NVX [3D-DEPTH]: starting isolated ZF32/kind-6 diagnostic.\n");
    /* Depth draws preserve color, so initialize color to white; an all-black
       target with LESS would reject every positive depth value. */
    if(!NvxGspSubmitFill(HwDeviceExtension,dstVa,0xffffffffU,65536U)) {
        LogStr("NVX [3D-DEPTH]: ZF32 color-target prefill failed.\n");
        return FALSE;
    }
    if(!NvxGspSubmitUserOperation(HwDeviceExtension,dstVa,depthVa,0,
        fenceValue,TRUE,TRUE,5,NVX_3D_ZT_FORMAT_ZF32,0,vertexVa,
        3U,0xff00ff00U,64U,16U,0,&drawn,&white)) return FALSE;
    {
        PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
        scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
        readback=(volatile ULONG *)(scratch+0xe000);
        VideoPortZeroMemory((PVOID)readback,4096);
        if(!NvxGspSubmitCopy2D(dev,dev->GspScratchGpuVa+0xe000,dstVa,
                               256,256,256,16)) return FALSE;
        for(i=0;i<NVX_3D_READBACK_PIXELS;i++) {
            ULONG pixel=NvxGspReadBlockLinearPixel((volatile UCHAR *)readback,
                                                   i%64U,i/64U,64);
            if(pixel==0xffffffffU) whitePixels++;
            else if((pixel&0x00ffffffU)==0x0000ff00U) green++;
            else if((pixel&0x00ffffffU)==0x00ff0000U) red++;
            else if((pixel&0x00ffffffU)==0) black++;
            else {if(!other) firstOther=pixel;other++;}
            if(pixel!=0xffffffffU) {
                ULONG x=i%64U,y=i/64U;
                if(x<minX) minX=x;
                if(x>maxX) maxX=x;
                if(y<minY) minY=y;
                if(y>maxY) maxY=y;
            }
        }
        LogStr("NVX [3D-DEPTH]: ZF32 readback green=");LogHex(green);
        LogStr(" red=");LogHex(red);LogStr(" black=");LogHex(black);
        LogStr(" white=");LogHex(whitePixels);LogStr(" other=");LogHex(other);
        LogStr(" firstOther=");LogHex(firstOther);
        LogStr(" bounds=");LogHex(minX);LogStr(",");LogHex(minY);
        LogStr("-");LogHex(maxX);LogStr(",");LogHex(maxY);
        LogStr(" center=");LogHex(NvxGspReadBlockLinearPixel(
            (volatile UCHAR *)readback,32,8,64));LogStr("\n");
    }
    return TRUE;
}

BOOLEAN NvxGspReadUserSurface(PVOID HwDeviceExtension,ULONG64 srcVa,
                              PULONG output,ULONG outputPixels)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *readback;
    ULONG wait,x,y;

    if(!dev||!output||outputPixels!=NVX_3D_READBACK_PIXELS||
       !dev->GspInitialized||!dev->GspGraphicsChannelReady||
       dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued||
       !dev->GspDmaVirtual||!dev->GspScratchGpuVa||
       (srcVa&0xffffULL)) return FALSE;
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) return FALSE;

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    readback=(volatile ULONG *)(scratch+0xe000);
    VideoPortZeroMemory((PVOID)readback,NVX_3D_READBACK_BYTES);
    if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xe000,srcVa,
                           256,256,256,16)) {
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    __sync_synchronize();
    for(y=0;y<NVX_3D_TRIANGLE_HEIGHT;y++) {
        for(x=0;x<NVX_3D_TRIANGLE_WIDTH;x++) {
            output[y*NVX_3D_TRIANGLE_WIDTH+x]=
                NvxGspReadBlockLinearPixel((volatile UCHAR *)readback,
                                            x,y,NVX_3D_TRIANGLE_WIDTH);
        }
    }
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return TRUE;
}

static BOOLEAN NvxGspUserAllocationContains(PNVX_DEVICE_EXTENSION dev,
                                            ULONG64 gpuVa,ULONG bytes,
                                            BOOLEAN gartOnly)
{
    ULONG i;
    if(!dev||!gpuVa||!bytes||gpuVa>~0ULL-bytes) return FALSE;
    for(i=0;i<NVX_3D_USER_BUFFER_SLOTS;i++) {
        const NVX_3D_USER_BUFFER_ENTRY *entry=&dev->Nvx3dBuffers[i];
        BOOLEAN gart=(entry->Usage&NVX_3D_USAGE_GART_BUFFER)!=0;
        if(!entry->Id||!entry->Busy||(gartOnly&&!gart)||
           (gart&&(!entry->SystemVirtual||!entry->GartPageCount))||
           gpuVa<entry->GpuVa||entry->GpuVa>~0ULL-entry->Bytes||
           gpuVa+bytes>entry->GpuVa+entry->Bytes)
            continue;
        return TRUE;
    }
    return FALSE;
}

/* Read one dword-aligned raw BO range through the CE. The 4 KiB staging
   window is private miniport DMA memory; caller pointers are never mapped
   into the GPU address space. Larger BO maps must be assembled in chunks. */
BOOLEAN NvxGspReadUserBuffer(PVOID HwDeviceExtension,ULONG64 srcVa,
                             PVOID output,ULONG bytes)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    ULONG wait;

    if(!dev||!output||!bytes||bytes>NVX_3D_MAX_DOWNLOAD_BYTES||
       (bytes&3U)||(srcVa&3ULL)||
       !dev->GspInitialized||!dev->GspCopyVerified||
       dev->GspChannelFaulted||dev->GspMmuFaultQueued||
       !dev->GspDmaVirtual||!dev->GspScratchGpuVa)
        return FALSE;
    if(!NvxGspUserAllocationContains(dev,srcVa,bytes,FALSE))
        return FALSE;

    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) return FALSE;

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET+0xe000;
    VideoPortZeroMemory(scratch,NVX_3D_MAX_DOWNLOAD_BYTES);
    if(!NvxGspSubmitCopy2DLocked(dev,dev->GspScratchGpuVa+0xe000,
                                 srcVa,bytes,bytes,bytes,1U)) {
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    __sync_synchronize();
    VideoPortMoveMemory(output,scratch,bytes);
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return TRUE;
}

BOOLEAN NvxGspWaitUserFence(PVOID HwDeviceExtension,ULONG64 fenceValue,
                            ULONG timeoutMs)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    ULONG elapsed=0;
    if(!dev||!fenceValue||fenceValue>dev->Nvx3dLastFence||timeoutMs>30000)
        return FALSE;
    while(elapsed<=timeoutMs) {
        if(dev->Nvx3dCompletedFence>=fenceValue) return TRUE;
        if(dev->GspGraphicsChannelFaulted||dev->GspMmuFaultQueued) return FALSE;
        VideoPortStallExecution(1000);elapsed++;
    }
    return FALSE;
}

/* Modular components matching OpenRM architecture */
#include "nvx_ce.c"
#include "nvx_disp_rpc.c"
#include "nvx_gsp_boot.c"
