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
#include "nvx_openrm_shim.h"
#include <stddef.h>
#include "bootloader_blob.h"
#include "booter_load_blob.h"

#ifndef NVX_ENABLE_GR_CHANNEL_PROBE
#define NVX_ENABLE_GR_CHANNEL_PROBE 0
#endif

/* ABI checks deliberately target firmware 535, not the current OpenRM ABI. */
_Static_assert(sizeof(GspFwWprMeta) == 256, "WPR ABI");
_Static_assert(offsetof(GspFwWprMeta, verified) == 248, "WPR verified ABI");
_Static_assert(sizeof(GSP_ARGUMENTS_CACHED) == 80, "535 RM args ABI");
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
    for(i=0;i<NVX_PPBDMA_COUNT;i++) {
        ULONG intrOffset=NVX_PPBDMA_INTR_0(i);
        ULONG intr1Offset=NVX_PPBDMA_INTR_1(i);
        ULONG methodOffset=NVX_PPBDMA_METHOD0(i);
        ULONG dataOffset=NVX_PPBDMA_DATA0(i);
        ULONG targetOffset=NVX_PPBDMA_TARGET(i);
        ULONG gpBaseOffset=NVX_PPBDMA_GP_BASE(i);
        ULONG gpBaseHiOffset=NVX_PPBDMA_GP_BASE_HI(i);
        ULONG gpGetOffset=NVX_PPBDMA_GP_GET(i);
        ULONG gpPutOffset=NVX_PPBDMA_GP_PUT(i);
        ULONG intr,intr1,method,data,target,gpBase,gpBaseHi,gpGet,gpPut;
        if(intrOffset>dev->Bar0Length-4||intr1Offset>dev->Bar0Length-4||
           methodOffset>dev->Bar0Length-4||dataOffset>dev->Bar0Length-4||
           targetOffset>dev->Bar0Length-4||gpBaseOffset>dev->Bar0Length-4||
           gpBaseHiOffset>dev->Bar0Length-4||gpGetOffset>dev->Bar0Length-4||
           gpPutOffset>dev->Bar0Length-4) break;
        intr=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+intrOffset));
        if((intr&0xffff0000U)==0xbadf0000U) {
            LogStr("NVX [PBDMA]: BAR0 register window is unreadable at index=");
            LogHex(i);LogStr(" offset=");LogHex(intrOffset);LogStr(" value=");LogHex(intr);LogStr("\n");
            break;
        }
        intr1=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+intr1Offset));
        method=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+methodOffset));
        data=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+dataOffset));
        target=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+targetOffset));
        gpBase=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpBaseOffset));
        gpBaseHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpBaseHiOffset));
        gpGet=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpGetOffset));
        gpPut=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+gpPutOffset));
        LogStr("NVX [PBDMA]: index=");LogHex(i);LogStr(" INTR0=");LogHex(intr);
        LogStr(" INTR1=");LogHex(intr1);LogStr(" TARGET=");LogHex(target);
        LogStr(" METHOD0=");LogHex(method);LogStr(" DATA0=");LogHex(data);
        LogStr(" GP_BASE=");LogHex(gpBase);LogStr(" GP_BASE_HI=");LogHex(gpBaseHi);
        LogStr(" GP_GET=");LogHex(gpGet);LogStr(" GP_PUT=");LogHex(gpPut);LogStr("\n");
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
        if(!result && (function==0x101b||function==0x100c||function==0x1004||function==0x1006||function==0x100f)) {
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
    if(bytes>8168) return FALSE;
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
    VideoPortZeroMemory(tables,GSP_PAGE_TABLES_SIZE);
    /* PDEs: coherent-system aperture, volatile, next-table address >> 4. */
    tables[0]=((physical+0x1000)>>4)|0xc;
    tables[512]=((physical+0x2000)>>4)|0xc;
    tables[1024]=((physical+0x3000)>>4)|0xc;
    /* PD0 has paired large/small entries; VA>>21 == 128, small is +8. */
    tables[1536+128*2+1]=((physical+0x4000)>>4)|0xc;
    for(ULONG i=0;i<16;i++)
        tables[2048+i]=((dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET+i*4096)>>4)|0xd;
    /* VA 0x20000000 selects level-2 index 1, so it needs a second PD0 page.
       Its paired PD0 entries point to a 64KB LPT and 4KB SPT. Each 256-byte
       LPT covers one 2MB VA slice. Map VRAM physical 0..64MB in CE VA space. */
    tables[1025]=((physical+0x5000)>>4)|0xc;
    for(ULONG slice=0;slice<32;slice++) {
        ULONG64 *lpt=(ULONG64 *)((PUCHAR)tables+0x6000+slice*0x100);
        ULONG pdIndex=slice;
        tables[2560+pdIndex*2]=((physical+0x6000+slice*0x100)>>4)|0xc;
        tables[2560+pdIndex*2+1]=0;
        for(ULONG page=0;page<32;page++) {
            ULONG64 address=(ULONG64)slice*0x200000+(ULONG64)page*0x10000;
            if(address+0xffff<=dev->GspFbFreeLimit)
                lpt[page]=(address>>4)|1;
        }
    }
    /* DMA allocations are only 4KB aligned. 64KB leaves silently discard
       low physical address bits and read unrelated bytes on real XP boots.
       One SPT maps the 64KB staging window used by bounded upload requests. */
    tables[2560+32*2]=0;
    tables[2560+32*2+1]=((physical+0x8000)>>4)|0xc;
    for(ULONG page=0;page<16;page++) {
        ULONG64 pagePhysical=dev->GspDmaPhysical.QuadPart+GSP_UPLOAD_OFFSET+
            (ULONG64)page*0x1000;
        tables[4096+page]=(pagePhysical>>4)|0xd;
    }
    dev->GspVramGpuVa=0x20000000ULL;
    dev->GspVramMapBytes=0x4000000ULL;
    __sync_synchronize();
    setDirectory[0]=(ULONG)physical;setDirectory[1]=(ULONG)(physical>>32);
    setDirectory[2]=4; /* top-level 49-bit directory has four entries */
    setDirectory[3]=1; /* SYSMEM_COH */
    setDirectory[4]=dev->GspVaSpaceHandle;
    if(!NvxGspControl(dev,dev->GspDeviceHandle,NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY,setDirectory,sizeof(setDirectory))) return FALSE;
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
    /* Reserve [0x9000,0xd000) separately for commands/data/completion. */
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
 * Opt-in XP x64 graphics-engine bring-up probe. This creates a distinct GR
 * TSG/channel and advertised Ampere graphics object alongside the known-good CE path. It does
 * not submit a draw, change scanout, or make OpenGL available yet. Keep the
 * allocation behind NVX_ENABLE_GR_CHANNEL_PROBE until the guest log confirms
 * the 535 GSP-RM ABI and resource setup on hardware.
 */
#if NVX_ENABLE_GR_CHANNEL_PROBE
static BOOLEAN NvxGspCreateGraphicsProbe(PNVX_DEVICE_EXTENSION dev)
{
    NV_CHANNEL_ALLOC_PARAMS chan={0};
    NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS tsg={0};
    NV_CTXSHARE_ALLOCATION_PARAMETERS ctxShare={0};
    ULONG bindEngine=NV2080_ENGINE_TYPE_GRAPHICS;
    UCHAR schedEnable[2]={1,0};
    ULONG64 scratchPhys,instPhys,ramfcPhys,userdPhys,methodPhys,fifoGpuVa,scratchEnd;
    NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS supportedClasses={0};
    ULONG classEngineId=0;
    NV906F_CTRL_GET_CLASS_ENGINEID_PARAMS classEngineParams={0};
    NVB06F_CTRL_GET_ENGINE_CTX_SIZE_PARAMS grCtxSize={0};
    NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS grHwState={0};
    NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS *grBufferInfo;
    ULONG classCount,classIndex,grClass=0;
    BOOLEAN ampereAFound=FALSE,ampereBFound=FALSE;
    ULONG runlistQuery[124]={0},token=0,runlistBase=0,submitToken=0;
    ULONG dbcfg,chcfg,enable,wait,chramBase,fault[26]={0};
    ULONG64 pushVa;
    PUCHAR scratch;
    PULONG push;
    volatile ULONG *userd;
    volatile ULONG64 *fifo;
    const ULONG tsgHandle=0xa06c0002;
    const ULONG ctxShareHandle=0x90670002;
    const ULONG channelHandle=0xc56f0002;
    const ULONG grHandle=0xc6970001;

    if(!dev||!dev->GspInitialized||!dev->GspDmaVirtual||!dev->GspScratchGpuVa) {
        LogStr("NVX [GR]: probe preconditions failed; graphics remains disabled.\n");
        return FALSE;
    }
    grBufferInfo=&dev->GrContextInfoParams;
    scratchPhys=(ULONG64)dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET;
    instPhys=scratchPhys+0xa000;
    /* RAMFC is the leading region of the instance block; OpenRM's channel
       path and our known-good CE channel both describe it at instPhys. */
    ramfcPhys=instPhys;
    userdPhys=scratchPhys+0xb400; /* USERD index 2 */
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
        }
    }
    LogStr("NVX [GR]: RM advertised class count=");LogHex(classCount);
    LogStr(" AMPERE_A=");LogHex(ampereAFound?1:0);
    LogStr(" AMPERE_B=");LogHex(ampereBFound?1:0);LogStr("\n");
    /* GA104 (RTX 3070) advertises AMPERE_B; GA100 exposes AMPERE_A. */
    if(ampereBFound) grClass=AMPERE_B;
    else if(ampereAFound) grClass=AMPERE_A;
    if(!grClass) {
        LogStr("NVX [GR]: firmware advertises no Ampere graphics class; skipping channel allocation.\n");
        return FALSE;
    }

    LogStr("NVX [GR]: creating isolated GR-engine TSG/context-share/channel.\n");
    /* Keep GR's channel group and context share separate from the working CE
       path. RM uses the TSG engine type to select the engine context. */
    tsg.hVASpace=dev->GspVaSpaceHandle;
    tsg.engineType=NV2080_ENGINE_TYPE_GRAPHICS;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,tsgHandle,0xa06c,&tsg,sizeof(tsg))) {
        LogStr("NVX [GR]: graphics channel group allocation failed.\n");
        return FALSE;
    }
    ctxShare.hVASpace=dev->GspVaSpaceHandle;
    ctxShare.flags=NV_CTXSHARE_ALLOCATION_FLAGS_SUBCONTEXT_SYNC;
    ctxShare.subctxId=0;
    if(!NvxGspAlloc(dev,tsgHandle,ctxShareHandle,0x9067,&ctxShare,sizeof(ctxShare))) {
        LogStr("NVX [GR]: graphics context-share allocation failed.\n");
        return FALSE;
    }
    chan.gpFifoOffset=fifoGpuVa;
    chan.gpFifoEntries=256;
    chan.flags=0x00200220; /* privileged, fixed USERD page 2, slot 2 */
    chan.hContextShare=ctxShareHandle;
    chan.hVASpace=0; /* Context-share owns the channel VASpace. */
    chan.engineType=NV2080_ENGINE_TYPE_GRAPHICS;
    chan.subDeviceId=0;
    chan.instanceMem.base=instPhys;
    chan.instanceMem.size=0x1000;
    chan.instanceMem.addressSpace=1; /* ADDR_SYSMEM */
    chan.instanceMem.cacheAttrib=0;  /* coherent system memory */
    chan.userdMem.base=userdPhys;
    chan.userdMem.size=0x200;
    chan.userdMem.addressSpace=1;
    chan.userdMem.cacheAttrib=0;
    chan.ramfcMem.base=ramfcPhys;
    chan.ramfcMem.size=0x200;
    chan.ramfcMem.addressSpace=1;
    chan.ramfcMem.cacheAttrib=0;
    chan.mthdbufMem.base=methodPhys;
    chan.mthdbufMem.size=0x2000;
    chan.mthdbufMem.addressSpace=1;
    chan.mthdbufMem.cacheAttrib=0;
    chan.internalFlags=0x15;

    if(!NvxGspAlloc(dev,tsgHandle,channelHandle,AMPERE_CHANNEL_GPFIFO_A,&chan,sizeof(chan))) {
        LogStr("NVX [GR]: graphics GPFIFO channel allocation failed.\n");
        return FALSE;
    }
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    LogStr("NVX [GR]: RAMFC_TARGET after channel allocation=");
    LogHex(*(volatile ULONG *)(scratch+0xa000+NVX_RAMFC_TARGET_OFFSET));LogStr("\n");
    /* Create the GR object before binding/scheduling the runlist, matching
       OpenRM's kernel_graphics bring-up order. This lets RM attach its GR
       engine context before the channel becomes runnable. */
    if(!NvxGspAlloc(dev,channelHandle,grHandle,grClass,NULL,0)) {
        LogStr("NVX [GR]: Ampere graphics object allocation failed; class=");LogHex(grClass);LogStr("\n");
        return FALSE;
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
    LogStr("NVX [GR]: RAMFC_TARGET after GR object/bind/schedule=");
    LogHex(*(volatile ULONG *)(scratch+0xa000+NVX_RAMFC_TARGET_OFFSET));LogStr("\n");
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
        LogStr("NVX [GR]: RM context-buffer info query unavailable.\n");
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
        LogStr("NVX [GR]: object classEngineID=");LogHex(reply->classEngineID);
        LogStr(" classID=");LogHex(reply->classID);
        LogStr(" engineID=");LogHex(reply->engineID);LogStr("\n");
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
    if(NvxGspControl(dev,channelHandle,NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
                     &submitToken,sizeof(submitToken))) {
        submitToken=((const ULONG *)dev->RpcRxRecord)[26];
    }
    LogStr("NVX [GR]: RM work-submit token=");LogHex(submitToken);LogStr("\n");

    userd=(volatile ULONG *)(scratch+0xb400);
    fifo=(volatile ULONG64 *)(scratch+0x9000);
    push=(PULONG)(scratch+0xf000);
    pushVa=dev->GspScratchGpuVa+0xf000;
    if(userd[0x88/4]||userd[0x8c/4]) {
        LogStr("NVX [GR]: fresh graphics USERD is not empty; submission skipped.\n");
        return FALSE;
    }
    VideoPortZeroMemory((PVOID)(scratch+0x9000),0x1000);
    VideoPortZeroMemory((PVOID)(scratch+0xf000),0x1000);
    /* Use the Ampere GPFIFO method header and RM subchannel (0). The former
       CE-derived packet selected subchannel 4, which GR rejects with RC 0x20. */
    push[0]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|NVC56F_SET_OBJECT;
    push[1]=classEngineId;
    fifo[0]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|(2ULL<<42);
    __sync_synchronize();
    userd[0x8c/4]=1;
    __sync_synchronize();
    /* USERD is allocated at fixed index 2 above.  On GA104 the scheduler
       exposes that graphics channel in CHRAM slot 2; ring that channel's
       doorbell rather than borrowing channel 1 (the independent CE channel).
       Keep RM's opaque token authoritative whenever it supplies one. */
    token=submitToken?submitToken:((dbcfg&0xffff0000U)|2U);
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),token);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x90),2);
    LogStr("NVX [GR]: rang channel doorbell token=");LogHex(token);LogStr(" chid=0x2\n");
    LogStr("NVX [GR]: submitted guarded SET_OBJECT packet; waiting for GR GPGet.\n");
    for(wait=0;wait<100000;wait++) {
        if(userd[0x88/4]==1) {
            break;
        }
        if(wait&&!(wait%10000)) {
            LogStr("NVX [GR]: waiting seconds=");LogHex(wait/10000);
            LogStr(" GPGet=");LogHex(userd[0x88/4]);
            LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\n");
        }
        VideoPortStallExecution(100);
    }
    if(userd[0x88/4]==1) {
        /* Let FECS report a rejected object bind, then consume the event via
           a benign RPC before advertising the GR channel as healthy. */
        VideoPortStallExecution(100000);
        NvxGspControl(dev,channelHandle,NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
                      &submitToken,sizeof(submitToken));
        if(!dev->GspGraphicsChannelFaulted) {
            dev->GspGraphicsCommandVerified=TRUE;
            LogStr("NVX [GR]: graphics runlist consumed SET_OBJECT without an RC fault.\n");
        }
    }
    if(!dev->GspGraphicsCommandVerified) {
        if(userd[0x88/4]==1)
            LogStr("NVX [GR]: command fetched but rejected or channel faulted; GPGet=");
        else
            LogStr("NVX [GR]: command fetch timed out; GPGet=");
        LogHex(userd[0x88/4]);
        LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\n");
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
            PULONG ramfc=(PULONG)(scratch+0xa000);
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
        return FALSE;
    }

    dev->GspGraphicsTsgHandle=0;
    dev->GspGraphicsContextShareHandle=0;
    dev->GspGraphicsChannelHandle=channelHandle;
    dev->GspGraphicsObjectHandle=grHandle;
    dev->GspGraphicsChannelReady=TRUE;
    LogStr("NVX [GR]: Ampere graphics object allocated; safe method consumed, no draw submitted.\n");
    return TRUE;
}
#endif

/* Modular components matching OpenRM architecture */
#include "nvx_ce.c"
#include "nvx_disp_rpc.c"
#include "nvx_gsp_boot.c"
