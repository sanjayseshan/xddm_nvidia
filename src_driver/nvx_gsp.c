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
#include <stddef.h>
#include "bootloader_blob.h"
#include "booter_load_blob.h"

/* ABI checks deliberately target firmware 535, not the current OpenRM ABI. */
_Static_assert(sizeof(GSP_FW_WPR_META_V1) == 256, "WPR ABI");
_Static_assert(offsetof(GSP_FW_WPR_META_V1, Verified) == 248, "WPR verified ABI");
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

/* One synchronous post-init RPC. Unexpected events remain unconsumed. */
static BOOLEAN NvxGspCall(PNVX_DEVICE_EXTENSION dev,ULONG expected,
                         const void *payload,ULONG bytes)
{
    volatile NVX_MSGQ_HEADER *cmd=dev->CmdQueueVirtual,*stat=dev->StatQueueVirtual;
    ULONG write=cmd->WritePtr,next=(write+1)%63;
    if(!dev->GspInitialized||write>=63||stat->ReadPtr>=63||next==stat->ReadPtr) return FALSE;
    if(!NvxEncodeRpc((PUCHAR)dev->CmdQueueVirtual+4096+write*4096,
        4096,expected,dev->RpcTxSequence,payload,bytes)) return FALSE;
    __sync_synchronize();cmd->WritePtr=next;__sync_synchronize();
    dev->RpcTxSequence++;
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x110c00),0);
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
        if(!result && (function==0x101b||function==0x100c||function==0x1004)) {
            const ULONG *words=(const ULONG *)dev->RpcRxRecord;
            if(function==0x101b) {
                if(words[14]<36) return FALSE;
                dev->GspAssertCount=words[20];
                LogStr("NVX [RM]: firmware assertion count=");LogHex(words[20]);LogStr("\n");
            } else if(function==0x1004) {
                if(words[14]<52) return FALSE;
                LogStr("NVX [RM]: channel RC fault engine=");LogHex(words[20]);
                LogStr(" chid=");LogHex(words[21]);LogStr(" type=");LogHex(words[22]);
                LogStr(" scope=");LogHex(words[23]);LogStr("\n");
                if(words[21]==1) dev->GspChannelFaulted=TRUE;
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
    ULONG payload[522]={0};
    const ULONG *words=(const ULONG *)dev->RpcRxRecord;
    if(bytes>2064) return FALSE;
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
       LPT covers one 2MB VA slice. Map VRAM physical 0..32MB in CE VA space. */
    tables[1025]=((physical+0x5000)>>4)|0xc;
    for(ULONG slice=0;slice<16;slice++) {
        ULONG64 *lpt=(ULONG64 *)((PUCHAR)tables+0x6000+slice*0x100);
        ULONG pdIndex=slice;
        tables[2560+pdIndex*2]=((physical+0x6000+slice*0x100)>>4)|0xc;
        tables[2560+pdIndex*2+1]=0;
        for(ULONG page=0;page<32;page++)
            lpt[page]=((((ULONG64)slice*0x200000+(ULONG64)page*0x10000)>>4)|1);
    }
    /* DMA allocations are only 4KB aligned. 64KB leaves silently discard
       low physical address bits and read unrelated bytes on real XP boots.
       One SPT maps the 64KB staging window used by bounded upload requests. */
    tables[2560+16*2]=0;
    tables[2560+16*2+1]=((physical+0x7000)>>4)|0xc;
    for(ULONG page=0;page<16;page++) {
        ULONG64 pagePhysical=dev->GspDmaPhysical.QuadPart+GSP_UPLOAD_OFFSET+
            (ULONG64)page*0x1000;
        tables[3584+page]=(pagePhysical>>4)|0xd;
    }
    dev->GspVramGpuVa=0x20000000ULL;
    dev->GspVramMapBytes=0x2000000ULL;
    __sync_synchronize();
    setDirectory[0]=(ULONG)physical;setDirectory[1]=(ULONG)(physical>>32);
    setDirectory[2]=4; /* top-level 49-bit directory has four entries */
    setDirectory[3]=1; /* SYSMEM_COH */
    setDirectory[4]=dev->GspVaSpaceHandle;
    if(!NvxGspControl(dev,dev->GspDeviceHandle,0x801813,setDirectory,sizeof(setDirectory))) return FALSE;
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
        if(region.base>region.limit||region.limit>=wpr->FbSize) return FALSE;
        LogStr("NVX [FB]: region=");LogHex(i);LogStr(" base=");LogHex(region.base);
        LogStr(" limit=");LogHex(region.limit);LogStr(" reserved=");LogHex(region.reserved);
        LogStr(" protected=");LogHex(region.protectedMemory);LogStr(" iso=");LogHex(region.iso);LogStr("\n");
        if(!freeLimit&&!region.reserved&&!region.protectedMemory&&region.compressed&&region.iso) {
            ULONG64 base=(region.base+0xffffULL)&~0xffffULL,limit=region.limit;
            if(base<wpr->GspFwWprEnd&&limit>=wpr->GspFwWprStart) {
                if(base<wpr->GspFwWprStart) limit=wpr->GspFwWprStart-1;
                else base=(wpr->GspFwWprEnd+0xffffULL)&~0xffffULL;
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
    context.dev=dev;context.bytes=wpr->FbSize;context.reads=0;
    if(!NvxPraminRead64(&context,instance+0x200,&pdbRaw)) return FALSE;
    LogStr("NVX [FBMAP]: BAR1 block=");LogHex(block);LogStr(" instance=");LogHex(instance);
    LogStr(" pdbRaw=");LogHex(pdbRaw);LogStr(" firmware root=");LogHex(dev->GspBar1PageDirectory);LogStr("\n");
    if((pdbRaw&3)||!(pdbRaw&0x400)) return FALSE;
    pdb=pdbRaw&~0xfffULL;bigShift=(pdbRaw&0x800)?16:17;
    while(mapped<bytes) {
        NVX_MMU_MAPPING map;
        ULONG64 chunk;
        int status=NvxMmuTranslateVram(NvxPraminRead64,&context,pdb,offset+mapped,wpr->FbSize,bigShift,&map);
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
    if(first<wpr->GspFwWprEnd&&first+bytes>wpr->GspFwWprStart) return FALSE;
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
        if(!NvxGspControl(dev,dev->GspSubdeviceHandle,0x20802a07,caps,sizeof(caps))) continue;
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
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,0x20802a08,&ceMthdParam,sizeof(ceMthdParam))) {
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

    /* All descriptors describe OS-owned coherent SYSTEM memory. */
    chan[36]=(ULONG)instPhys;
    chan[37]=(ULONG)(instPhys>>32);
    chan[38]=0x1000;
    chan[40]=1;         /* ADDR_SYSMEM, never a VRAM physical offset */
    chan[41]=1;         /* cacheAttrib = CACHED */

    /* userdMem: slot 1, matching CHANNEL_USERD_INDEX_VALUE. */
    chan[42]=(ULONG)userdPhys;
    chan[43]=(ULONG)(userdPhys>>32);
    chan[44]=0x200;
    chan[46]=1;
    chan[47]=1;         /* cacheAttrib = CACHED */

    /* ramfcMem aliases the beginning of the instance block. */
    chan[48]=(ULONG)instPhys;
    chan[49]=(ULONG)(instPhys>>32);
    chan[50]=0x200;
    chan[52]=1;
    chan[53]=1;         /* cacheAttrib = CACHED */

    /* mthdbufMem: at scratch offset 0x4000 (ADDR_SYSMEM = 1, UNCACHED = 0) */
    chan[54]=(ULONG)mthdbufPhys;
    chan[55]=(ULONG)(mthdbufPhys>>32);
    chan[56]=mthdSize?mthdSize:0x1000;
    chan[58]=1;         /* addressSpace = ADDR_SYSMEM */
    chan[59]=0;         /* cacheAttrib = UNCACHED */

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
    if(!NvxGspControl(dev,dev->GspChannelHandle,0xa06f0104,&bindEngine,sizeof(bindEngine))) {
        LogStr("NVX [RM]: NVA06F_CTRL_CMD_BIND failed.\n");
        return FALSE;
    }
    LogStr("NVX [RM]: channel bound to dedicated CE engine=");LogHex(bindEngine);LogStr("\n");

    /* Schedule channel */
    if(!NvxGspControl(dev,dev->GspChannelHandle,0xa06f0103,schedEnable,sizeof(schedEnable))) {
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

static BOOLEAN NvxGspCopySmokeTest(PNVX_DEVICE_EXTENSION dev)
{
    /* C56F/C7B5 535 ABI. Scratch layout: GPFIFO 0..1fff, instance 2000,
       USERD 3200, CE fault buffer 4000..8fff, push 9000, source a000,
       destination b000, completion c000. No scanout/VRAM is touched. */
    PUCHAR scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    volatile ULONG *userd=(volatile ULONG *)(scratch+0x3200);
    volatile ULONG *dst=(volatile ULONG *)(scratch+0xb000);
    volatile ULONG *fence=(volatile ULONG *)(scratch+0xc000);
    volatile ULONG *backup=(volatile ULONG *)(scratch+0xd000);
    volatile ULONG *restored=(volatile ULONG *)(scratch+0xe000);
    PULONG src=(PULONG)(scratch+0xa000),push=(PULONG)(scratch+0x9000);
    volatile ULONG64 *fifo=(volatile ULONG64 *)scratch;
    ULONG token=0,n=0,runlistBase=0,runlistQuery[124]={0}; /* two arrays of 62 engines */
    ULONG64 va=dev->GspScratchGpuVa,pushVa=va+0x9000;
    ULONG64 vramVa=dev->GspVramGpuVa+0x1000000ULL; /* 16MB, inside free ISO region */
    const ULONG done=0x91c0ffee,guard=0xf17e55aa;
    dev->GspCopyVerified=FALSE;
    dev->GspChannelFaulted=FALSE;
    if(dev->Bar0Length<0x30094||!dev->GspCopyEngineHandle||!va) return FALSE;
    if(!NvxGspControl(dev,dev->GspChannelHandle,0xc36f0108,&token,sizeof(token))) {
        LogStr("NVX [COPY]: work-submit token unavailable; no commands submitted.\n");
        return FALSE;
    }
    token=((const ULONG *)dev->RpcRxRecord)[26];
    LogStr("NVX [COPY]: RM work-submit token=");LogHex(token);LogStr("\n");
    /* GSP's 535 kfifoGenerateWorkSubmitToken_GA100() generates an internal
       token but returns its initially-zero external value. Nouveau uses
       the physical runlist doorbell selector and the client-fixed ChID.
       Our PAGE_FIXED flags reserve USERD page 0 / slot 1, i.e. ChID 1. */
    runlistQuery[0]=dev->GspCopyEngineType;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,0x20800179,runlistQuery,sizeof(runlistQuery))) {
        LogStr("NVX [COPY]: selected CE runlist query failed; no commands submitted.\n");return FALSE;
    }
    {
        ULONG base=((const ULONG *)dev->RpcRxRecord)[26+62];
        ULONG dbcfg,chcfg,enable;
        /* GA104's COPY2 runlist is 0xc00400: 1KB windows, not 4KB pages. */
        if(!base||(base&0x3ff)||dev->Bar0Length<0x304||base>dev->Bar0Length-0x304) {
            LogStr("NVX [COPY]: invalid CE runlist base=");LogHex(base);LogStr("\n");return FALSE;
        }
        dbcfg=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+base+8));
        chcfg=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+base+4));
        enable=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+base+0x300));
        if(dbcfg==0xffffffffU||(dbcfg&0xffff0000U)==0xbadf0000U) return FALSE;
        token=(dbcfg&0xffff0000U)|1;
        runlistBase=base;
        dev->GspRunlistBase=base;
        dev->GspSubmitToken=token;
        dev->GspGPPut=1;
        dev->GspNextFence=0x91c0ffefU;
        LogStr("NVX [COPY]: physical runlist=");LogHex(base);LogStr(" dbcfg=");LogHex(dbcfg);
        LogStr(" chcfg=");LogHex(chcfg);LogStr(" enable=");LogHex(enable);
        LogStr(" token=");LogHex(token);LogStr("\n");
        {
            ULONG chram=chcfg&0xfffffff0U;
            if(chram&&chram<=dev->Bar0Length-8) {
                LogStr("NVX [COPY]: CHRAM slot0=");
                LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chram)));
                LogStr(" slot1=");
                LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chram+4)));
                LogStr("\n");
            }
        }
    }
    if(dev->GspChannelFaulted) return FALSE;
    if(userd[0x88/4]||userd[0x8c/4]) {
        LogStr("NVX [COPY]: fresh channel has nonzero GP pointers; submission skipped.\n");
        return FALSE;
    }
    if(!dev->GspFbLayoutValid||dev->GspFbFreeBase>0x1000000ULL||
       dev->GspFbFreeLimit<0x10000ffULL||vramVa<dev->GspVramGpuVa||
       0x1000000ULL+256>dev->GspVramMapBytes) {
        LogStr("NVX [COPY]: no firmware-confirmed free VRAM probe page; VRAM test skipped.\n");return FALSE;
    }
    for(ULONG i=0;i<64;i++) {
        src[i]=0x6d3a5000U^(i*0x10203U);dst[i]=~src[i];
        backup[i]=0;restored[i]=0;
    }
    src[-1]=src[64]=guard;dst[-1]=dst[64]=guard;
    backup[-1]=backup[64]=guard;restored[-1]=restored[64]=guard;
    fence[-1]=guard;fence[0]=0;fence[1]=guard;
    /* Subchannel 4 works for dedicated CE and GRCE on the graphics runlist.
       NVIDIA UVM notes that subchannel 0 fails on GRCE. SET_OBJECT uses class. */
    push[n++]=0x20018000;push[n++]=0xc7b5;
    /* Preserve the free offscreen VRAM bytes, write a pattern, read it back,
       restore the original bytes, then verify restoration. */
#define NVX_APPEND_COPY(_src,_dst) do { \
    ULONG64 _s=(_src),_d=(_dst); \
    push[n++]=0x20048100; \
    push[n++]=(ULONG)(_s>>32);push[n++]=(ULONG)_s; \
    push[n++]=(ULONG)(_d>>32);push[n++]=(ULONG)_d; \
    push[n++]=0x20048104; \
    push[n++]=256;push[n++]=256;push[n++]=256;push[n++]=1; \
    push[n++]=0x200180c0;push[n++]=0x04000186; \
} while(0)
    NVX_APPEND_COPY(vramVa,va+0xd000);
    NVX_APPEND_COPY(va+0xa000,vramVa);
    NVX_APPEND_COPY(vramVa,va+0xb000);
    NVX_APPEND_COPY(va+0xd000,vramVa);
    NVX_APPEND_COPY(vramVa,va+0xe000);
#undef NVX_APPEND_COPY
    push[n++]=0x20038090; /* SET_SEMAPHORE_A/B/PAYLOAD */
    push[n++]=(ULONG)((va+0xc000)>>32);push[n++]=(ULONG)(va+0xc000);push[n++]=done;
    push[n++]=0x200180c0;push[n++]=0x0400000c; /* flushed one-word semaphore release */
    fifo[0]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();userd[0x8c/4]=1;__sync_synchronize();
    /* Ampere USERMODE window is BAR0+0x30000, NOT Volta's 0x810000. */
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),token);
    /* Match ga100_chan_start(): an initial per-runlist notification is
       needed when starting the client-allocated channel (GFID 0, ChID 1).
       Re-notifying the same published GPPut does not enqueue extra work. */
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x90),1);
    LogStr("NVX [COPY]: submitted guarded VRAM write/read/restore roundtrip and completion release.\n");
    for(ULONG wait=0;wait<20000;wait++) {
        if(fence[0]==done) {
            __sync_synchronize();
            for(ULONG i=0;i<64;i++) {
                ULONG expected=0x6d3a5000U^(i*0x10203U);
                if(dst[i]!=expected||src[i]!=expected||restored[i]!=backup[i]) {
                    LogStr("NVX [COPY]: completion arrived but data mismatch at word=");LogHex(i);LogStr("\n");
                    return FALSE;
                }
            }
            if(src[-1]!=guard||src[64]!=guard||dst[-1]!=guard||dst[64]!=guard||
               backup[-1]!=guard||backup[64]!=guard||restored[-1]!=guard||restored[64]!=guard||
               fence[-1]!=guard||fence[1]!=guard) {
                LogStr("NVX [COPY]: completion arrived but guard corrupted.\n");return FALSE;
            }
            dev->GspCopyVerified=TRUE;
            LogStr("NVX [COPY]: GPU copy completed and verified; guarded VRAM roundtrip and restore succeeded; desktop drawing remains software.\n");
            return TRUE;
        }
        VideoPortStallExecution(100);
    }
    LogStr("NVX [COPY]: completion timeout; GPGet=");LogHex(userd[0x88/4]);
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr(" fence=");LogHex(fence[0]);LogStr("\n");
    {
        PULONG ramfc=(PULONG)(scratch+0x2000);
        ULONG fault[26]={0};
        LogStr("NVX [COPY]: RAMFC userdLo=");LogHex(ramfc[0x8/4]);
        LogStr(" userdHi=");LogHex(ramfc[0xc/4]);LogStr(" gpBaseLo=");LogHex(ramfc[0x48/4]);
        LogStr(" gpBaseHi=");LogHex(ramfc[0x4c/4]);LogStr(" chid=");LogHex(ramfc[0xe8/4]);LogStr("\n");
        if(NvxGspControl(dev,dev->GspChannelHandle,0xa06f0107,fault,sizeof(fault))) {
            const ULONG *reply=(const ULONG *)dev->RpcRxRecord;
            LogStr("NVX [COPY]: MMU fault addrHi=");LogHex(reply[26]);
            LogStr(" addrLo=");LogHex(reply[27]);LogStr(" type=");LogHex(reply[28]);LogStr("\n");
        } else LogStr("NVX [COPY]: MMU fault-info control unavailable.\n");
    }
    return FALSE;
}

BOOLEAN NvxGspSubmitCopy(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG64 srcVa,ULONG bytes)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *userd;
    volatile ULONG *fence;
    PULONG push;
    volatile ULONG64 *fifo;
    ULONG slot,n=0,fenceVal;
    ULONG64 va,pushVa;

    if(!dev||!dev->GspInitialized||!dev->GspCopyVerified||dev->GspChannelFaulted) return FALSE;
    if(!bytes||bytes>0x1000000) return FALSE; /* Up to 16MB per submit */

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    userd=(volatile ULONG *)(scratch+0x3200);
    fence=(volatile ULONG *)(scratch+0xc000);
    fifo=(volatile ULONG64 *)scratch;
    va=dev->GspScratchGpuVa;

    slot=dev->GspGPPut%16;
    push=(PULONG)(scratch+0x9000+slot*0x100);
    pushVa=va+0x9000+slot*0x100;
    fenceVal=dev->GspNextFence++;
    if(!fenceVal) fenceVal=dev->GspNextFence++;

    push[n++]=0x20018000;push[n++]=0xc7b5; /* SET_OBJECT AMPERE_DMA_COPY_B */
    push[n++]=0x20048100; /* OFFSET_IN/OUT_UPPER/LOWER */
    push[n++]=(ULONG)(srcVa>>32);push[n++]=(ULONG)srcVa;
    push[n++]=(ULONG)(dstVa>>32);push[n++]=(ULONG)dstVa;
    push[n++]=0x20048104; /* PITCH_IN/OUT, LINE_LENGTH, LINE_COUNT */
    push[n++]=bytes;push[n++]=bytes;push[n++]=bytes;push[n++]=1;
    push[n++]=0x200180c0;push[n++]=0x04000186; /* LAUNCH_DMA */
    push[n++]=0x20038090; /* SET_SEMAPHORE_A/B/PAYLOAD */
    push[n++]=(ULONG)((va+0xc000)>>32);push[n++]=(ULONG)(va+0xc000);push[n++]=fenceVal;
    push[n++]=0x200180c0;push[n++]=0x0400000c; /* flushed release */

    fifo[dev->GspGPPut%1024]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();
    dev->GspGPPut = (dev->GspGPPut + 1) % 1024;
    userd[0x8c/4] = dev->GspGPPut;
    __sync_synchronize();

    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),dev->GspSubmitToken);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+dev->GspRunlistBase+0x90),1);

    for(ULONG wait=0;wait<20000;wait++) {
        if(fence[0]==fenceVal) {
            __sync_synchronize();
            return TRUE;
        }
        VideoPortStallExecution(10);
    }
    LogStr("NVX [COPY-Q]: submit timeout! fenceVal=");LogHex(fenceVal);
    LogStr(" fence=");LogHex(fence[0]);LogStr(" GPGet=");LogHex(userd[0x88/4]);
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\n");
    dev->GspChannelFaulted=TRUE;
    return FALSE;
}

BOOLEAN NvxGspSubmitFill(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG color,ULONG bytes)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *userd;
    volatile ULONG *fence;
    PULONG push;
    volatile ULONG64 *fifo;
    ULONG slot,n=0,fenceVal;
    ULONG64 va,pushVa;

    if(!dev||!dev->GspInitialized||!dev->GspCopyVerified||dev->GspChannelFaulted) return FALSE;
    if(!bytes||bytes>0x1000000||(bytes&3)) return FALSE;

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    userd=(volatile ULONG *)(scratch+0x3200);
    fence=(volatile ULONG *)(scratch+0xc000);
    fifo=(volatile ULONG64 *)scratch;
    va=dev->GspScratchGpuVa;

    slot=dev->GspGPPut%16;
    push=(PULONG)(scratch+0x9000+slot*0x100);
    pushVa=va+0x9000+slot*0x100;
    fenceVal=dev->GspNextFence++;
    if(!fenceVal) fenceVal=dev->GspNextFence++;

    push[n++]=0x20018000;push[n++]=0xc7b5; /* SET_OBJECT AMPERE_DMA_COPY_B */
    push[n++]=0x200281c1; /* SET_REMAP_CONST_B (0x704), SET_REMAP_COMPONENTS (0x708) */
    push[n++]=color;
    push[n++]=0x00030005; /* DST_X CONST_B, 4-byte component */
    push[n++]=0x20028102; /* OFFSET_OUT_UPPER/LOWER */
    push[n++]=(ULONG)(dstVa>>32);push[n++]=(ULONG)dstVa;
    push[n++]=0x20018106; /* LINE_LENGTH_IN (dwords) */
    push[n++]=bytes/4;
    push[n++]=0x200180c0;push[n++]=0x04000586; /* LAUNCH_DMA with REMAP_ENABLE */
    push[n++]=0x20038090; /* SET_SEMAPHORE_A/B/PAYLOAD */
    push[n++]=(ULONG)((va+0xc000)>>32);push[n++]=(ULONG)(va+0xc000);push[n++]=fenceVal;
    push[n++]=0x200180c0;push[n++]=0x0400000c; /* flushed release */

    fifo[dev->GspGPPut%1024]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();
    dev->GspGPPut = (dev->GspGPPut + 1) % 1024;
    userd[0x8c/4] = dev->GspGPPut;
    __sync_synchronize();

    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),dev->GspSubmitToken);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+dev->GspRunlistBase+0x90),1);

    for(ULONG wait=0;wait<20000;wait++) {
        if(fence[0]==fenceVal) {
            __sync_synchronize();
            return TRUE;
        }
        VideoPortStallExecution(10);
    }
    LogStr("NVX [FILL-Q]: submit timeout! fenceVal=");LogHex(fenceVal);
    LogStr(" fence=");LogHex(fence[0]);LogStr(" GPGet=");LogHex(userd[0x88/4]);
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\n");
    dev->GspChannelFaulted=TRUE;
    return FALSE;
}

BOOLEAN NvxGspUpload(PVOID HwDeviceExtension,ULONG64 dstVa,const void *src,ULONG bytes)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    if(!dev||!src||!bytes||bytes>GSP_UPLOAD_MAX_BYTES||
       !dev->GspInitialized||!dev->GspCopyVerified||dev->GspChannelFaulted||
       !dev->GspDmaVirtual||!dev->GspVramGpuVa) return FALSE;
    if(NvxMmuCheckRange(dev->GspVramGpuVa,dev->GspVramMapBytes,dstVa,bytes)!=NVX_MMU_OK)
        return FALSE;
    VideoPortMoveMemory((PUCHAR)dev->GspDmaVirtual+GSP_UPLOAD_OFFSET,(PVOID)src,bytes);
    __sync_synchronize();
    if(!NvxGspSubmitCopy(HwDeviceExtension,dstVa,GSP_UPLOAD_GPU_VA,bytes))
        return FALSE;
    /* Verify the real staging path, not just the separately mapped scratch. */
    {
        static ULONG verifiedUploads=0;
        if(verifiedUploads<3) {
            ULONG count=bytes<256?bytes:256, mismatches=0;
            PUCHAR readback=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET+0xa000;
            if(!NvxGspSubmitCopy(HwDeviceExtension,dev->GspScratchGpuVa+0xa000,dstVa,count))
                return FALSE;
            for(ULONG i=0;i<count;i++)
                if(readback[i]!=((const UCHAR *)src)[i]) mismatches++;
            verifiedUploads++;
            LogStr("NVX [UPLOAD-CHECK]: GPU readback bytes=");LogHex(count);
            LogStr(" mismatches=");LogHex(mismatches);LogStr("\n");
            if(mismatches) return FALSE;
        }
    }
    return TRUE;
}

/* Display DMA uses physical push buffers and a display-owned instance block,
 * independently of the CE virtual-address space. RM 535 ABI, checked below. */
typedef struct {
    ULONG64 physical,size;
    ULONG addressSpace,cache;
} NVX_DISPLAY_INSTANCE_535;
typedef struct {
    ULONG addressSpace,pad;
    ULONG64 physical,limit;
    ULONG cache,hclass,instance;
    UCHAR valid,padding[3];
} NVX_DISPLAY_PUSH_535;
typedef struct {
    ULONG instance,buffer,notify,offset;
    ULONG64 control;
    ULONG flags,pad;
} NVX_DISPLAY_CHANNEL_535;
_Static_assert(sizeof(NVX_DISPLAY_INSTANCE_535)==24,"535 display instance ABI");
_Static_assert(sizeof(NVX_DISPLAY_PUSH_535)==40,"535 display push ABI");
_Static_assert(offsetof(NVX_DISPLAY_PUSH_535,physical)==8,"535 display push alignment");
_Static_assert(offsetof(NVX_DISPLAY_PUSH_535,valid)==36,"535 display push valid");
_Static_assert(sizeof(NVX_DISPLAY_CHANNEL_535)==32,"535 display DMA ABI");

static BOOLEAN NvxNativeFreeObject(PNVX_DEVICE_EXTENSION dev,ULONG parent,ULONG handle)
{
    ULONG release[4]={dev->GspClientHandle,parent,handle,0};
    const ULONG *reply=(const ULONG *)dev->RpcRxRecord;
    return NvxGspCall(dev,10,release,sizeof(release)) && reply[14]>=48 &&
        reply[20]==release[0] && reply[21]==parent && reply[22]==handle && !reply[23];
}

static VOID NvxNativeLogDisplayStatus(PNVX_DEVICE_EXTENSION dev,const char *phase)
{
    PUCHAR bar0=dev->Bar0MmioVirtual;
    static const ULONG offsets[]={0x610010,0x610014,0x610020,0x610024,0x610030,
                                  0x610078,0x610080,0x610084,0x611cf0,0x611db0};
    static const char *names[]={"inst-ctrl","inst-base","intr0","intr1","super",
                                "awake","core-exc","core-data","ctrl-mask","ctrl-enable"};
    if(!bar0 || dev->Bar0Length<0x611db4) return;
    LogStr("NVX [NATIVE]: display status ");LogStr(phase);LogStr(":");
    for(ULONG i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        LogStr(" ");LogStr(names[i]);LogStr("=");
        LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+offsets[i])));
    }
    LogStr("\n");
}

/* Nouveau's GV100+ display init wakes the engine through 0x610078 bit 0
   before publishing instance memory. Leave interrupt masks/enable untouched. */
static BOOLEAN NvxNativeWakeDisplayEngine(PNVX_DEVICE_EXTENSION dev)
{
    PUCHAR bar0=dev->Bar0MmioVirtual;
    PULONG awake;
    ULONG before,after;
    if(!bar0 || dev->Bar0Length<0x61007c) return FALSE;
    awake=(PULONG)(bar0+0x610078);
    before=VideoPortReadRegisterUlong(awake);
    LogStr("NVX [NATIVE]: display wake bit before=");LogHex(before);
    if(before==0xffffffff || before==0xbadf5040) {
        LogStr("; register unavailable, no write.\n");return FALSE;
    }
    if(!(before&1)) VideoPortWriteRegisterUlong(awake,before|1);
    __sync_synchronize();
    after=VideoPortReadRegisterUlong(awake);
    LogStr(" after=");LogHex(after);LogStr("\n");
    return (after&1)!=0;
}

/* Establish hardware completion before attempting any native head update. */
static BOOLEAN NvxNativeVerifyNotifier(PNVX_DEVICE_EXTENSION dev)
{
    const ULONG handle=0xd1500001,descriptorOffset=0x2000;
    ULONG descriptor[6]={0},n=4;
    uint32_t header;
    uint32_t entry[2],slot;
    ULONG64 physical=dev->GspDmaPhysical.QuadPart+GSP_DISPLAY_PUSH_OFFSET+0x3000;
    ULONG64 first=physical>>8,last=(physical+0xfff)>>8;
    ULONG64 instanceVa=dev->GspVramGpuVa+dev->NativeDisplayInstance;
    PULONG push=(PULONG)((PUCHAR)dev->GspDmaVirtual+GSP_DISPLAY_PUSH_OFFSET);
    volatile ULONG *notify=(volatile ULONG *)((PUCHAR)dev->GspDmaVirtual+GSP_DISPLAY_PUSH_OFFSET+0x3000);
    PUCHAR bar0=dev->Bar0MmioVirtual;
    if(dev->GspDmaLength<GSP_DISPLAY_PUSH_OFFSET+0x4000) return FALSE;
    VideoPortZeroMemory((PVOID)notify,0x1000);
    /* Volta+ display DMA descriptor: coherent PCI, RW, 4KB pages.
       Start/limit use 256-byte units; descriptor resides after 8KB RAMHT. */
    descriptor[0]=0x46;
    descriptor[1]=(ULONG)first;descriptor[2]=(ULONG)(first>>32);
    descriptor[3]=(ULONG)last;descriptor[4]=(ULONG)(last>>32);
    if(!NvxGspUpload(dev,instanceVa+descriptorOffset,descriptor,sizeof(descriptor))) return FALSE;
    if(!NvxDisplayRamhtEntry(handle,0,descriptorOffset,dev->GspClientHandle,&slot,entry)) return FALSE;
    if(!NvxGspUpload(dev,instanceVa+slot*8,entry,sizeof(entry))) return FALSE;
    /* Nouveau's C37D core init publishes format/usage bounds for each of the
       eight windows before any core UPDATE. Keep this to capability bounds;
       no window/head ownership or scanout state is assigned. */
    for(ULONG window=0;window<8;window++) {
        ULONG formatMethod=0x1004+window*0x80;
        ULONG usageMethod=0x1010+window*0x80;
        if(!NvxDisplayPushHeader(formatMethod,2,&header)) return FALSE;
        push[n++]=header;push[n++]=0x0f;push[n++]=0;
        if(!NvxDisplayPushHeader(usageMethod,1,&header)) return FALSE;
        push[n++]=header;push[n++]=0x00107fff;
    }
    /* Mirror Nouveau's C67D head capability bounds (256x256 cursor, output
       LUT and 2-tap scaler). These are limits, not active head state. */
    for(ULONG head=0;head<4;head++) {
        ULONG usageMethod=0x2030+head*0x400;
        if(!NvxDisplayPushHeader(usageMethod,1,&header)) return FALSE;
        push[n++]=header;push[n++]=0x00001014;
    }
    __sync_synchronize();
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x680000),n);
    for(ULONG wait=0;wait<2000;wait++) {
        ULONG get=VideoPortReadRegisterUlong((PULONG)(bar0+0x680004));
        if(get!=0xffffffff && (get&0x3ff)==n) break;
        if(wait==1999) {
            LogStr("NVX [NATIVE]: core window-bounds init stalled GET=");
            LogHex(get);LogStr("; update disabled.\n");return FALSE;
        }
        VideoPortStallExecution(100);
    }
    LogStr("NVX [NATIVE]: C67D core capability bounds consumed GET=");LogHex(n);LogStr("\n");
    /* Each header carries the byte method offset and count in bits 27:18.
       Core PUT remains a DWORD index. This update changes no head state. */
    push[n++]=(1U<<18)|(0x208>>2);push[n++]=handle;
    push[n++]=(1U<<18)|(0x20c>>2);push[n++]=0x1000; /* notifier enabled, offset zero */
    push[n++]=(2U<<18)|(0x218>>2);push[n++]=0;push[n++]=0;
    push[n++]=(1U<<18)|(0x200>>2);
    push[n++]=NVX_C67D_UPDATE_INHIBIT_INTERRUPTS;
    NvxNativeLogDisplayStatus(dev,"before-update");
    __sync_synchronize();
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x680000),n);
    for(ULONG wait=0;wait<10000;wait++) {
        ULONG get=VideoPortReadRegisterUlong((PULONG)(bar0+0x680004));
        __sync_synchronize();
        if(get!=0xffffffff && (get&0x3ff)==n && (notify[0]>>30)==2) {
            dev->NativeDisplayCorePut=n;
            dev->NativeDisplayNotifierReady=TRUE;
            LogStr("NVX [NATIVE]: core UPDATE completed, GET=");LogHex(get);
            LogStr(" notifier=");LogHex(notify[0]);LogStr("; head timings unchanged.\n");return TRUE;
        }
        VideoPortStallExecution(100);
    }
    LogStr("NVX [NATIVE]: core UPDATE completion unconfirmed GET=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x680004)));
    LogStr(" notifier=");LogHex(notify[0]);LogStr("\n");
    NvxNativeLogDisplayStatus(dev,"after-timeout");
    LogStr("NVX [NATIVE]: native modeset disabled.\n");
    return FALSE;
}

static VOID NvxNativeReadOutputInfo(PNVX_DEVICE_EXTENSION dev)
{
    for(ULONG monitor=0;monitor<dev->MonitorCount;monitor++)
        dev->MonitorSor[monitor]=0xffffffffUL;
    for(ULONG monitor=0;monitor<dev->MonitorCount;monitor++) {
        /* RM 535 OR_GET_INFO: 40-byte prefix, u64 VBIOS, two bools, pad. */
        ULONG params[14]={0,dev->MonitorIds[monitor]},*reply=(ULONG *)dev->RpcRxRecord;
        if(!NvxGspControl(dev,dev->GspDisplayHandle,0x73028b,params,sizeof(params))) continue;
        dev->MonitorSor[monitor]=reply[28];
        dev->MonitorProtocol[monitor]=reply[30];
        LogStr("NVX [OUTPUT]: display=");LogHex(dev->MonitorIds[monitor]);
        LogStr(" index=");LogHex(reply[28]);LogStr(" type=");LogHex(reply[29]);
        LogStr(" protocol=");LogHex(reply[30]);LogStr(" litByVbios=");LogHex(reply[38]&255);LogStr("\n");
    }
}

/* Deliberately only NOPs: GET progress proves DMA fetch, not a modeset. */
BOOLEAN NvxGspInitDisplayChannels(PVOID extension)
{
    PNVX_DEVICE_EXTENSION dev=extension;
    const ULONG offsets[3]={0x680000,0x690000,0x692000};
    const ULONG classes[3]={0xc67d,0xc67e,0xc67e};
    const ULONG instances[3]={0,0,2};
    NVX_DISPLAY_INSTANCE_535 instance={0};
    PUCHAR bar0,scratch;
    ULONG64 base,va;
    if(!dev || dev->NativeDisplayAttempted) return dev && dev->NativeDisplayChannelsReady;
    if(!dev->GspCopyVerified || !dev->NativeDisplayHandle || !dev->GspFbLayoutValid ||
       dev->GspChannelFaulted || dev->Bar0Length<0x693000 ||
       dev->GspDmaLength<GSP_DISPLAY_PUSH_OFFSET+0x3000 ||
       dev->DisplayHeadCount<2 || (dev->DisplayWindowMask&5)!=5) return FALSE;
    dev->NativeDisplayAttempted=TRUE;
    /* Reserve the top 64KB of our existing 32MB CE VRAM aperture. This is
       outside all supported primary framebuffers (at most 16MB) and the CE
       smoke-test word at 16MB. Never use firmware-owned/protected regions. */
    base=0x1ff0000ULL;
    if(dev->GspFbFreeBase>base || dev->GspFbFreeLimit<base+0xffff ||
       dev->GspVramMapBytes<base+0x10000 ||
       (dev->GspScanoutMapped && dev->GspScanoutPhysical<base+0x10000 &&
        base<dev->GspScanoutPhysical+dev->GspScanoutBytes)) return FALSE;
    dev->NativeDisplayInstance=base;
    va=dev->GspVramGpuVa+base;
    bar0=dev->Bar0MmioVirtual;
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    if(!NvxGspSubmitFill(dev,va,0,0x10000)) return FALSE;
    /* GPU readback of the entire instance/RAMHT, not the unreliable BAR1 map. */
    for(ULONG offset=0;offset<0x10000;offset+=0x1000) {
        if(!NvxGspSubmitCopy(dev,dev->GspScratchGpuVa+0xa000,va+offset,0x1000)) return FALSE;
        for(ULONG i=0;i<0x1000;i++) if(scratch[0xa000+i]) return FALSE;
    }
    instance.physical=base;instance.size=0x10000;instance.addressSpace=2;instance.cache=2;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,0x20800a49,&instance,sizeof(instance))) {
        LogStr("NVX [NATIVE]: display instance registration failed; no channels submitted.\n");return FALSE;
    }
    LogStr("NVX [NATIVE]: verified 64KB display instance VRAM=");LogHex(base);LogStr("\n");
    for(ULONG channel=0;channel<3;channel++) {
        NVX_DISPLAY_PUSH_535 push={0};
        NVX_DISPLAY_CHANNEL_535 args={0};
        ULONG handle=0xc6700100+channel;
        PULONG buffer=(PULONG)((PUCHAR)dev->GspDmaVirtual+GSP_DISPLAY_PUSH_OFFSET+channel*0x1000);
        VideoPortZeroMemory(buffer,0x1000);
        push.addressSpace=1;
        push.physical=dev->GspDmaPhysical.QuadPart+GSP_DISPLAY_PUSH_OFFSET+channel*0x1000;
        push.limit=0xfff;push.cache=1;push.hclass=classes[channel];
        push.instance=instances[channel];push.valid=1;
        __sync_synchronize();
        if(!NvxGspControl(dev,dev->GspSubdeviceHandle,0x20800a58,&push,sizeof(push))) goto failed;
        args.instance=instances[channel];
        if(!NvxGspAlloc(dev,dev->NativeDisplayHandle,handle,classes[channel],&args,sizeof(args))) goto failed;
        dev->NativeDisplayChannels[channel]=handle;
        /* PUT/GET are DWORD indices, not byte offsets. Four zero NOP words. */
        __sync_synchronize();
        VideoPortWriteRegisterUlong((PULONG)(bar0+offsets[channel]),4);
        for(ULONG wait=0;wait<2000;wait++) {
            ULONG get=VideoPortReadRegisterUlong((PULONG)(bar0+offsets[channel]+4));
            if(get!=0xffffffff && (get&0x3ff)==4) {
                LogStr("NVX [NATIVE]: DMA NOP consumed class=");LogHex(classes[channel]);
                LogStr(" instance=");LogHex(instances[channel]);LogStr(" GET=");LogHex(get);LogStr("\n");
                break;
            }
            if(wait==1999) {
                LogStr("NVX [NATIVE]: DMA NOP timeout class=");LogHex(classes[channel]);
                LogStr(" GET=");LogHex(get);LogStr("\n");goto failed;
            }
            VideoPortStallExecution(100);
        }
    }
    dev->NativeDisplayChannelsReady=TRUE;
    (void)NvxNativeWakeDisplayEngine(dev);
    (void)NvxNativeVerifyNotifier(dev);
    LogStr("NVX [NATIVE]: core and windows 0/2 DMA verified; head timings unchanged.\n");
    return TRUE;
failed:
    /* Avoid leaked live channels on partial allocation. Keep backing pinned
       if RM cannot acknowledge teardown; whole-client shutdown also retains it. */
    for(ULONG channel=3;channel>0;channel--) {
        ULONG *handle=&dev->NativeDisplayChannels[channel-1];
        if(*handle && NvxNativeFreeObject(dev,dev->NativeDisplayHandle,*handle)) *handle=0;
    }
    LogStr("NVX [NATIVE]: channel bring-up incomplete; no head update submitted.\n");
    return FALSE;
}

/* Wake DPCD sink power state (0x600 = D0) and power up RM Main Link transmitter (0x731356) */
VOID NvxGspWakeSecondaryDpSink(PVOID HwDeviceExtension)
{
    static BOOLEAN s_sinkWoken = FALSE;
    PNVX_DEVICE_EXTENSION dev = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    NVX_DP_AUX_CTRL_PARAMS aux = {0};
    NVX_DP_MAIN_LINK_CTRL_PARAMS ml = {0};
    const ULONG *reply;
    const NVX_DP_AUX_CTRL_PARAMS *auxResult;
    ULONG displayId;

    if (!dev || !dev->GspDisplayHandle) return;
    if (s_sinkWoken) return;
    s_sinkWoken = TRUE;
    displayId = dev->SecondaryDpDisplayId;
    if (!displayId && dev->MonitorCount > 1) {
        displayId = dev->MonitorIds[1];
    }
    if (!displayId) return;

    /* 1. Wake DP sink: Write DPCD 0x00600 (SET_POWER) = 0x01 (D0: Normal Operation) */
    aux.subDeviceInstance = 0;
    aux.displayId = displayId;
    aux.addrOnly = 0;
    aux.cmd = (NVX_DP_AUX_TYPE_AUX << 3) | NVX_DP_AUX_WRITE;
    aux.addr = 0x600;
    aux.size = 0; /* 0 means 1 byte per RM AUX ABI */
    aux.data[0] = 0x01; /* DP_SET_POWER_D0 */

    for (int retry = 0; retry < 5; retry++) {
        if (NvxGspControl(dev, dev->GspDisplayHandle, NVX_CTRL_CMD_DP_AUXCH_CTRL,
                         &aux, sizeof(aux))) {
            reply = (const ULONG *)dev->RpcRxRecord;
            auxResult = (const NVX_DP_AUX_CTRL_PARAMS *)(reply + 26);
            if (auxResult->replyType == NVX_DP_AUX_REPLY_ACK) {
                LogStr("NVX [DP]: sink wake (DPCD 0x600 = D0) ACKed display=");
                LogHex(displayId); LogStr("\n");
                break;
            } else {
                LogStr("NVX [DP]: sink wake (DPCD 0x600 = D0) reply=");
                LogHex(auxResult->replyType); LogStr(" retry="); LogHex(retry); LogStr("\n");
            }
        }
        VideoPortStallExecution(1000);
    }
    VideoPortStallExecution(2000); /* 2ms sink PLL stabilization */

    /* 2. Power up Main Link transmitter via RM control 0x731356 */
    ml.subDeviceInstance = 0;
    ml.displayId = displayId;
    ml.ctrl = NVX_DP_MAIN_LINK_CTRL_POWER_STATE_POWERUP; /* 1 */
    if (NvxGspControl(dev, dev->GspDisplayHandle, NVX_CTRL_CMD_DP_MAIN_LINK_CTRL,
                     &ml, sizeof(ml))) {
        LogStr("NVX [DP]: RM Main Link power-up completed for display=");
        LogHex(displayId); LogStr("\n");
    } else {
        LogStr("NVX [DP]: RM Main Link power-up failed for display=");
        LogHex(displayId); LogStr("\n");
    }
}

BOOLEAN NvxNativeSetSecondaryScanout(PVOID extension)
{
    PNVX_DEVICE_EXTENSION dev = (PNVX_DEVICE_EXTENSION)extension;
    PUCHAR bar0;
    PULONG corePush, winPush;
    ULONG coreN = 0, winN = 0;
    uint32_t header;
    NVX_DISPLAY_TIMING *t;
    ULONG width, height, pitch;
    ULONG hTotal, vTotal, hSyncEnd, vSyncEnd, hBlankStart, vBlankStart, pixelClockHz;
    ULONG64 vramOffset;
    ULONG sor, wait;

    if (!dev) return FALSE;
    bar0 = dev->Bar0MmioVirtual;
    if (!bar0 || !dev->NativeDisplayChannelsReady) {
        LogStr("NVX [NATIVE]: secondary scanout skipped: display channels not ready\n");
        return FALSE;
    }

    if (dev->MonitorCount < 2) {
        LogStr("NVX [NATIVE]: secondary scanout skipped: fewer than 2 monitors\n");
        return FALSE;
    }
    t = &dev->MonitorTiming[1];
    sor = dev->MonitorSor[1];
    if (sor >= 4) {
        LogStr("NVX [NATIVE]: secondary scanout skipped: invalid SOR ");
        LogHex(sor); LogStr("\n");
        return FALSE;
    }

    NvxGspWakeSecondaryDpSink(dev);

    {
        PNVX_DEVICE_EXTENSION secDev = (PNVX_DEVICE_EXTENSION)dev->SecondaryDeviceExtension;
        ULONG reqMode = secDev ? secDev->CurrentModeIndex : 0;
        if (secDev && reqMode < secDev->NumModes) {
            width = secDev->Modes[reqMode].VisScreenWidth;
            height = secDev->Modes[reqMode].VisScreenHeight;
            pitch = secDev->Modes[reqMode].ScreenStride;
        } else {
            width = t->hActive ? t->hActive : 2560;
            height = t->vActive ? t->vActive : 1440;
            pitch = width * 4;
        }
    }
    vramOffset = 0x2000000ULL; /* +32MB into BAR1 */

    if (width == 2560 && height == 1440) {
        if (t->hTotal > width && t->vTotal > height && t->pixelClockHz) {
            hTotal = t->hTotal;
            vTotal = t->vTotal;
            hSyncEnd = t->hSyncEnd;
            vSyncEnd = t->vSyncEnd;
            hBlankStart = t->hActive;
            vBlankStart = t->vActive;
            pixelClockHz = t->pixelClockHz;
        } else {
            /* Default 2560x1440 CVT-RB @ 60Hz */
            hTotal = 2720;
            vTotal = 1481;
            hSyncEnd = 2640;
            vSyncEnd = 1448;
            hBlankStart = 2560;
            vBlankStart = 1440;
            pixelClockHz = 241500000;
        }
    } else if (width == 1920 && height == 1080) {
        /* Standard 1080p60 */
        hTotal = 2200;
        vTotal = 1125;
        hSyncEnd = 2052;
        vSyncEnd = 1088;
        hBlankStart = 1920;
        vBlankStart = 1080;
        pixelClockHz = 148500000;
    } else if (width == 1280 && height == 720) {
        /* Standard 720p60 */
        hTotal = 1650;
        vTotal = 750;
        hSyncEnd = 1390;
        vSyncEnd = 725;
        hBlankStart = 1280;
        vBlankStart = 720;
        pixelClockHz = 74250000;
    } else if (width == 1024 && height == 768) {
        /* Standard 1024x768@60 */
        hTotal = 1344;
        vTotal = 806;
        hSyncEnd = 1184;
        vSyncEnd = 774;
        hBlankStart = 1024;
        vBlankStart = 768;
        pixelClockHz = 65000000;
    } else if (width == 800 && height == 600) {
        /* Standard 800x600@60 */
        hTotal = 1056;
        vTotal = 628;
        hSyncEnd = 968;
        vSyncEnd = 605;
        hBlankStart = 800;
        vBlankStart = 600;
        pixelClockHz = 40000000;
    } else {
        hTotal = width + 160;
        vTotal = height + 41;
        hSyncEnd = width + 80;
        vSyncEnd = height + 8;
        hBlankStart = width;
        vBlankStart = height;
        pixelClockHz = 148500000;
    }

    LogStr("NVX [NATIVE]: programming secondary scanout Head 1 on SOR=");
    LogHex(sor);
    LogStr(" width="); LogHex(width);
    LogStr(" height="); LogHex(height);
    LogStr(" clock="); LogHex(pixelClockHz);
    LogStr("\n");

    corePush = (PULONG)((PUCHAR)dev->GspDmaVirtual + GSP_DISPLAY_PUSH_OFFSET);
    winPush = (PULONG)((PUCHAR)dev->GspDmaVirtual + GSP_DISPLAY_PUSH_OFFSET + 0x2000);

    /* Read current GET/PUT for Window 2 channel */
    winN = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x692000)) & 0x3ff;
    if (winN >= 900) winN = 0;

    /* 1. Program Window 2 DMA pushbuffer (C67E) */
    NvxDisplayPushHeader(NVX_C67E_SET_SIZE, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = (height << 16) | width;

    NvxDisplayPushHeader(NVX_C67E_SET_STORAGE, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = 0x10; /* Pitch layout (NVC37E_SET_STORAGE_MEMORY_LAYOUT_PITCH) */

    NvxDisplayPushHeader(NVX_C67E_SET_PARAMS, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = 0xCF; /* A8R8G8B8 */

    NvxDisplayPushHeader(NVX_C67E_SET_PLANAR_STORAGE(0), 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = pitch >> 6; /* 64-byte blocks */

    NvxDisplayPushHeader(NVX_C67E_SET_CONTEXT_DMA_ISO(0), 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = 0;

    NvxDisplayPushHeader(NVX_C67E_SET_OFFSET(0), 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = (ULONG)(vramOffset >> 8); /* 256-byte units */

    NvxDisplayPushHeader(NVX_C67E_SET_SIZE_IN, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = (height << 16) | width;

    NvxDisplayPushHeader(NVX_C67E_SET_SIZE_OUT, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = (height << 16) | width;

    NvxDisplayPushHeader(NVX_C67E_SET_INTERLOCK_FLAGS, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = 1; /* bit 0 = INTERLOCK_WITH_CORE */

    NvxDisplayPushHeader(NVX_C67E_UPDATE, 1, &header);
    winPush[winN++] = header;
    winPush[winN++] = 1; /* RELEASE_ELV_TRUE */

    /* Read current GET/PUT for Core channel */
    coreN = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x680000)) & 0x3ff;
    if (coreN >= 900) coreN = 0;

    /* 2. Program Core channel (C67D) for Head 1 and SOR */
    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_OVERSCAN_COLOR(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = 0;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_RASTER_SIZE(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (vTotal << 16) | hTotal;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_RASTER_SYNC_END(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (vSyncEnd << 16) | hSyncEnd;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_RASTER_BLANK_END(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (vTotal << 16) | hTotal;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_RASTER_BLANK_START(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (vBlankStart << 16) | hBlankStart;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_MIN_FRAME_IDLE(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (1U << 16) | 2U; /* trailing=1, leading=2 */

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = pixelClockHz;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = 0;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = pixelClockHz;

    /* Viewport Size In & Out */
    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_VIEWPORT_SIZE_IN(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (height << 16) | width;

    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_VIEWPORT_SIZE_OUT(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (height << 16) | width;

    /* Head 1 Usage Bounds */
    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_HEAD_USAGE_BOUNDS(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = 0x1110;

    /* Head 1 Display ID */
    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_DISPLAY_ID(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = dev->MonitorIds[1];

    /* Control output resource: 24bpp/32bpp, CRC complete raster, no ext packet window */
    NvxDisplayPushHeader(NVX_C67D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(1), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = 0xFC000041;

    /* Assign Window 2 to Head 1 */
    NvxDisplayPushHeader(NVX_C67D_WINDOW_SET_CONTROL(2), 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = 1; /* OWNER_HEAD1 */

    /* Connect SOR to Head 1: protocol (dev->MonitorProtocol[1] << 8) | owner Head 1 (0x2) */
    {
        ULONG proto = dev->MonitorProtocol[1] ? dev->MonitorProtocol[1] : 9;
        NvxDisplayPushHeader(NVX_C67D_SOR_SET_CONTROL(sor), 1, &header);
        corePush[coreN++] = header;
        corePush[coreN++] = (proto << 8) | 2;
    }

    /* Interlock Core with Window 2 (bit 2) */
    NvxDisplayPushHeader(NVX_C67D_SET_WINDOW_INTERLOCK_FLAGS, 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = (1U << 2);

    /* Core UPDATE */
    NvxDisplayPushHeader(NVX_C67D_UPDATE, 1, &header);
    corePush[coreN++] = header;
    corePush[coreN++] = 1;

    __sync_synchronize();

    /* Kick off Window 2 DMA first, then Core DMA */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x692000), winN);
    VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x680000), coreN);

    /* Wait for execution */
    for (wait = 0; wait < 2000; wait++) {
        ULONG coreGet = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x680004)) & 0x3ff;
        ULONG winGet = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x692004)) & 0x3ff;
        if (coreGet == coreN && winGet == winN) {
            LogStr("NVX [NATIVE]: secondary scanout methods consumed by hardware! coreGET=");
            LogHex(coreGet); LogStr(" winGET="); LogHex(winGet); LogStr("\n");
            return TRUE;
        }
        VideoPortStallExecution(100);
    }

    LogStr("NVX [NATIVE]: secondary scanout wait timed out coreGET=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x680004)));
    LogStr(" winGET=");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x692004)));
    LogStr(" (target coreN="); LogHex(coreN); LogStr(" winN="); LogHex(winN); LogStr(")\n");
    return FALSE;
}

/* ctrl0073specific.h GET_EDID_V2: fixed inline buffer, no guest pointers. */
static VOID NvxGspReadMonitorEdid(PNVX_DEVICE_EXTENSION dev,ULONG displayId)
{
    ULONG params[516]={0};
    const ULONG *reply;
    const UCHAR *edid;
    ULONG bytes,i,block;
    static const UCHAR header[8]={0,255,255,255,255,255,255,0};
    if(dev->MonitorCount>=8) return;
    params[1]=displayId;params[2]=2048;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730245,params,sizeof(params))) {
        LogStr("NVX [DISPLAY]: EDID unavailable id=");LogHex(displayId);LogStr("\n");return;
    }
    reply=(const ULONG *)dev->RpcRxRecord;
    bytes=reply[28];edid=(const UCHAR *)(reply+30);
    if(bytes<128 || bytes>2048 || (bytes&127)) return;
    for(i=0;i<8;i++) if(edid[i]!=header[i]) return;
    if((ULONG)edid[126]+1>bytes/128) return;
    for(block=0;block<bytes;block+=128) {
        ULONG checksum=0;
        for(i=0;i<128;i++) checksum+=edid[block+i];
        if(checksum&255) {
            LogStr("NVX [DISPLAY]: invalid EDID checksum id=");LogHex(displayId);LogStr("\n");return;
        }
    }
    i=dev->MonitorCount++;
    dev->MonitorIds[i]=displayId;dev->MonitorEdidBytes[i]=bytes;
    VideoPortMoveMemory(dev->MonitorEdid[i],(PVOID)edid,bytes);
    for(ULONG dtd=54;dtd+18<=126;dtd+=18) {
        if(NvxDecodeDetailedTiming(edid+dtd,18,&dev->MonitorTiming[i])) {
            NVX_DISPLAY_TIMING *t=&dev->MonitorTiming[i];
            LogStr("NVX [MODEPLAN]: display=");LogHex(displayId);
            LogStr(" width=");LogHex(t->hActive);LogStr(" height=");LogHex(t->vActive);
            LogStr(" clockHz=");LogHex(t->pixelClockHz);
            LogStr(" refreshMilliHz=");LogHex(t->refreshMilliHz);LogStr("\n");
            break;
        }
    }
    for(ULONG dtd=54;dtd+18<=126;dtd+=18) {
        if(edid[dtd]==0 && edid[dtd+1]==0 && edid[dtd+2]==0 && edid[dtd+3]==0xfc) {
            char name[14]={0};
            for(int k=0;k<13;k++) {
                char c=(char)edid[dtd+5+k];
                if(c=='\n' || c=='\r' || c==0) break;
                name[k]=c;
            }
            LogStr("NVX [DISPLAY]: monitor id=");LogHex(displayId);
            LogStr(" model name: '");LogStr(name);LogStr("'\n");
            break;
        }
    }
    LogStr("NVX [DISPLAY]: valid EDID id=");LogHex(displayId);
    LogStr(" bytes=");LogHex(bytes);LogStr("\n");
}

/* GA102 native display root; its constructor creates the RM API resource.
   Core/window channel ownership and head updates remain separate operations. */
static VOID NvxGspNativeDisplayRoot(PNVX_DEVICE_EXTENSION dev)
{
    ULONG caps[2]={0,0},info[7]={0};
    const ULONG *reply;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0xc6700001,0xc670,NULL,0)) {
        LogStr("NVX [NATIVE]: GA102 display root allocation failed.\n");return;
    }
    dev->NativeDisplayHandle=0xc6700001;
    if(NvxGspControl(dev,dev->NativeDisplayHandle,0x50700709,caps,sizeof(caps))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        dev->NativeDisplayCaps=reply[27]&255;
        LogStr("NVX [NATIVE]: display caps=");LogHex(dev->NativeDisplayCaps);LogStr("\n");
    }
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,0x20800a01,info,sizeof(info))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        dev->DisplayWindowMask=reply[27];
        LogStr("NVX [NATIVE]: windows=");LogHex(dev->DisplayWindowMask);
        LogStr(" heads=");LogHex(reply[29]);LogStr("; channels not yet started.\n");
    }
}

/* Query routing without changing any output assignments or taking ownership. */
static VOID NvxGspPlanHeads(PNVX_DEVICE_EXTENSION dev)
{
    ULONG heads[3]={0,0,0},mask[2]={0,0},route[5]={0,0,0,0,0};
    const ULONG *reply;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730102,heads,sizeof(heads))) return;
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->DisplayHeadCount=reply[28];
    if(!dev->DisplayHeadCount || dev->DisplayHeadCount>4) return;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730287,mask,sizeof(mask))) return;
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->DisplayHeadMask=reply[27]&((1UL<<dev->DisplayHeadCount)-1);
    LogStr("NVX [MODEPLAN]: heads=");LogHex(dev->DisplayHeadCount);
    LogStr(" mask=");LogHex(dev->DisplayHeadMask);LogStr("\n");
    for(ULONG head=0;head<dev->DisplayHeadCount;head++) {
        ULONG active[4]={0,head,0,0};
        if(!(dev->DisplayHeadMask&(1UL<<head))) continue;
        if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730126,active,sizeof(active))) return;
        reply=(const ULONG *)dev->RpcRxRecord;
        dev->DisplayActiveIds[head]=reply[29];
        route[2]|=reply[29];
        LogStr("NVX [MODEPLAN]: active head=");LogHex(head);
        LogStr(" display=");LogHex(reply[29]);LogStr("\n");
    }
    {
        ULONG ordinal=0;
        for(ULONG bit=0;bit<32;bit++) if(route[2]&(1UL<<bit)) {
            if(ordinal>=8) return;
            for(ULONG head=0;head<dev->DisplayHeadCount;head++)
                if(dev->DisplayActiveIds[head]&(1UL<<bit)) route[3]|=head<<(ordinal*4);
            ordinal++;
        }
    }
    route[1]=dev->GspConnectedDisplays;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730125,route,sizeof(route))) return;
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->DisplayRoutableMask=reply[27]&dev->GspConnectedDisplays;
    dev->DisplayHeadRoutingMap=reply[30];
    LogStr("NVX [MODEPLAN]: routable=");LogHex(dev->DisplayRoutableMask);
    LogStr(" headMap=");LogHex(dev->DisplayHeadRoutingMap);
    LogStr("; no native modeset submitted.\n");
    if(dev->DisplayRoutableMask==dev->GspConnectedDisplays)
        NvxGspNativeDisplayRoot(dev);
}

/* Read transmitter/sink DP limits after RM has routed the secondary. */
static VOID NvxGspReadDpLinkInfo(PNVX_DEVICE_EXTENSION dev,ULONG displayId,ULONG sor)
{
    NVX_DP_GET_CAPS_PARAMS caps={0};
    ULONG link[4]={0};
    NVX_DP_AUX_CTRL_PARAMS aux={0};
    const ULONG *reply;
    const NVX_DP_AUX_CTRL_PARAMS *auxResult;
    BOOLEAN dpcdReady=FALSE,enhancedFraming=FALSE;
    caps.sorIndex=sor;
    if(NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DP_GET_CAPS,
                     &caps,sizeof(caps))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        LogStr("NVX [DP]: transmitter SOR=");LogHex(sor);
        LogStr(" maxLinkRate=");LogHex(reply[28]);
        LogStr(" versions=");LogHex(reply[29]);
        LogStr(" UHBR=");LogHex(reply[30]);LogStr("\n");
    } else {
        LogStr("NVX [DP]: transmitter capability query failed for SOR=");LogHex(sor);LogStr("\n");
    }

    aux.displayId=displayId;
    aux.cmd=(NVX_DP_AUX_TYPE_AUX<<3)|NVX_DP_AUX_READ;
    aux.addr=0;aux.size=3; /* DPCD revision, max rate, lane count, downspread. */
    if(NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DP_AUXCH_CTRL,
                     &aux,sizeof(aux))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        auxResult=(const NVX_DP_AUX_CTRL_PARAMS *)(reply+26);
        if(auxResult->replyType==NVX_DP_AUX_REPLY_ACK && auxResult->size==4) {
            LogStr("NVX [DP]: DPCD rev=");LogHex(auxResult->data[0]);
            LogStr(" maxRate=");LogHex(auxResult->data[1]);
            LogStr(" laneCaps=");LogHex(auxResult->data[2]);
            LogStr(" downspread=");LogHex(auxResult->data[3]);LogStr("\n");
            if(auxResult->data[0]>=0x12 && (auxResult->data[2]&0x1f)>=4 &&
               auxResult->data[1]>=0x14) dpcdReady=TRUE;
            enhancedFraming=(auxResult->data[2]&0x80)!=0;
        } else {
            LogStr("NVX [DP]: DPCD read not acknowledged size=");LogHex(auxResult->size);
            LogStr(" reply=");LogHex(auxResult->replyType);LogStr("\n");
        }
    } else {
        LogStr("NVX [DP]: DPCD AUX capability read failed for display=");LogHex(displayId);LogStr("\n");
    }

    dev->SecondaryDpDisplayId=displayId;
    dev->SecondaryDpTrainReady=dpcdReady;
    dev->SecondaryDpEnhancedFraming=enhancedFraming;
    if(!dpcdReady)
        LogStr("NVX [DP]: deferred link training unavailable; sink capabilities are insufficient.\n");

    link[1]=displayId;
    if(NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DP_GET_LINK_CONFIG,
                     link,sizeof(link))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        LogStr("NVX [DP]: current transmitter link lanes=");LogHex(reply[28]);
        LogStr(" rate=");LogHex(reply[29]);LogStr("\n");
    } else {
        LogStr("NVX [DP]: current link configuration unavailable for display=");
        LogHex(displayId);LogStr("\n");
    }
}

/* Run after the primary mode is programmed so a secondary-link transition
   cannot leave the working VBE primary in its bootstrap resolution. */
VOID NvxGspTrainSecondaryDpLink(PVOID HwDeviceExtension)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    NVX_DP_CTRL_PARAMS train={0};
    ULONG link[4]={0};
    const ULONG *reply;
    if(!dev || !dev->SecondaryDpTrainReady || dev->SecondaryDpTrainingAttempted ||
       !dev->SecondaryDpDisplayId || !dev->GspDisplayHandle) return;
    dev->SecondaryDpTrainingAttempted=TRUE;

    NvxGspWakeSecondaryDpSink(dev);

    train.displayId=dev->SecondaryDpDisplayId;
    train.cmd=NVX_DP_CTRL_SET_LANE_COUNT|NVX_DP_CTRL_SET_LINK_BW;
    train.data=NVX_DP_CTRL_DATA_LANE_COUNT_4|NVX_DP_CTRL_DATA_LINK_BW_HBR2;
    if(dev->SecondaryDpEnhancedFraming) {
        train.cmd|=NVX_DP_CTRL_SET_ENHANCED_FRAMING;
        train.data|=NVX_DP_CTRL_DATA_ENHANCED_FRAMING;
    }
    if(NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DP_CTRL,
                     &train,sizeof(train))) {
        const NVX_DP_CTRL_PARAMS *result;
        reply=(const ULONG *)dev->RpcRxRecord;
        result=(const NVX_DP_CTRL_PARAMS *)(reply+26);
        LogStr("NVX [DP]: deferred RM link-training err=");LogHex(result->err);
        LogStr(" retryMs=");LogHex(result->retryTimeMs);
        LogStr(" lanes/bandwidth data=");LogHex(result->data);LogStr("\n");
    } else {
        LogStr("NVX [DP]: deferred RM link training failed for display=");
        LogHex(dev->SecondaryDpDisplayId);LogStr("\n");
    }
    link[1]=dev->SecondaryDpDisplayId;
    if(NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DP_GET_LINK_CONFIG,
                     link,sizeof(link))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        LogStr("NVX [DP]: post-training link lanes=");LogHex(reply[28]);
        LogStr(" rate=");LogHex(reply[29]);LogStr("\n");
    }
}

/* Ask RM to route the inactive connected DFP to an available SOR. Preserve
   every SOR already serving an active head. This only changes RM's connector
   routing plan; it does not link-train, modeset, or touch the primary scanout. */
static VOID NvxGspAssignSecondarySor(PNVX_DEVICE_EXTENSION dev)
{
    NVX_DFP_ASSIGN_SOR_PARAMS params={0};
    const NVX_DFP_ASSIGN_SOR_PARAMS *result;
    const ULONG *reply;
    ULONG target=0,excludeMask=0,activeSorMask=0,match=0,assignedSor=0xffffffffUL;
    if(!dev->NativeDisplayHandle || !dev->GspDisplayHandle ||
       !dev->MonitorCount || !dev->DisplayHeadCount) return;

    for(ULONG monitor=0;monitor<dev->MonitorCount;monitor++) {
        BOOLEAN active=FALSE;
        for(ULONG head=0;head<dev->DisplayHeadCount;head++)
            if(dev->DisplayActiveIds[head]&dev->MonitorIds[monitor]) active=TRUE;
        if(active) {
            if(dev->MonitorSor[monitor]>=4) {
                LogStr("NVX [DISPLAY]: preserving active output with unknown SOR; secondary assignment skipped.\n");
                return;
            }
            activeSorMask|=1UL<<dev->MonitorSor[monitor];
        } else if(target==0 && dev->MonitorSor[monitor]==0xffffffffUL) {
            target=dev->MonitorIds[monitor];
        }
        if(dev->MonitorSor[monitor]<4)
            excludeMask|=1UL<<dev->MonitorSor[monitor];
    }
    if(!target || !activeSorMask) {
        LogStr("NVX [DISPLAY]: no safely assignable inactive DFP/active SOR pair found.\n");
        return;
    }
    params.subDeviceInstance=0;
    params.displayId=target;
    params.sorExcludeMask=(uint8_t)excludeMask;
    params.flags=0; /* AUDIO_DEFAULT; do not constrain SOR choice by audio. */
    LogStr("NVX [DISPLAY]: assigning inactive display=");LogHex(target);
    LogStr(" excluding active SOR mask=");LogHex(excludeMask);LogStr("\n");
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DFP_ASSIGN_SOR,
                      &params,sizeof(params))) {
        LogStr("NVX [DISPLAY]: RM SOR assignment failed; primary routing unchanged.\n");
        return;
    }
    reply=(const ULONG *)dev->RpcRxRecord;
    result=(const NVX_DFP_ASSIGN_SOR_PARAMS *)(reply+26);
    for(ULONG sor=0;sor<4;sor++) {
        ULONG mask=result->sorAssignListWithTag[sor].displayMask;
        ULONG type=result->sorAssignListWithTag[sor].sorType;
        LogStr("NVX [DISPLAY]: SOR=");LogHex(sor);LogStr(" displayMask=");
        LogHex(mask);LogStr(" type=");LogHex(type);LogStr("\n");
        if(mask&target) { match++;assignedSor=sor; }
    }
    if(match!=1 || assignedSor>=4 || (activeSorMask&(1UL<<assignedSor))) {
        LogStr("NVX [DISPLAY]: RM result did not provide one distinct secondary SOR.\n");
        return;
    }
    for(ULONG monitor=0;monitor<dev->MonitorCount;monitor++)
        if(dev->MonitorIds[monitor]==target) dev->MonitorSor[monitor]=assignedSor;
    LogStr("NVX [DISPLAY]: secondary connector assigned SOR=");LogHex(assignedSor);
    LogStr("; head timing/link remain unchanged.\n");
    NvxGspReadDpLinkInfo(dev,target,assignedSor);
}

/* NVIDIA ctrl0073system.h: discover physical connectors, independent of
   scanout programming. Optional failure must preserve the v125 desktop. */
static VOID NvxGspDiscoverDisplays(PNVX_DEVICE_EXTENSION dev)
{
    ULONG supported[3]={0,0,0};
    ULONG connected[4]={0,0,0,0};
    const ULONG *reply;
    dev->GspSupportedDisplays=dev->GspConnectedDisplays=0;
    dev->MonitorCount=0;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0x73000001,0x73,NULL,0)) {
        LogStr("NVX [DISPLAY]: display-common allocation unavailable; single VBE scanout retained.\n");
        return;
    }
    dev->GspDisplayHandle=0x73000001;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730120,supported,sizeof(supported))) {
        LogStr("NVX [DISPLAY]: connector support query unavailable.\n");
        return;
    }
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->GspSupportedDisplays=reply[27];
    LogStr("NVX [DISPLAY]: supported mask=");LogHex(dev->GspSupportedDisplays);
    LogStr(" DDC mask=");LogHex(reply[28]);LogStr("\n");
    if(!dev->GspSupportedDisplays) return;
    connected[2]=dev->GspSupportedDisplays;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,0x730122,connected,sizeof(connected))) {
        LogStr("NVX [DISPLAY]: connected-state query unavailable.\n");
        return;
    }
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->GspConnectedDisplays=reply[28]&dev->GspSupportedDisplays;
    LogStr("NVX [DISPLAY]: connected mask=");LogHex(dev->GspConnectedDisplays);
    LogStr("; discovery only, additional heads not programmed.\n");
    for(ULONG bit=0;bit<32;bit++)
        if(dev->GspConnectedDisplays&(1UL<<bit))
            NvxGspReadMonitorEdid(dev,1UL<<bit);
    NvxNativeReadOutputInfo(dev);
    NvxGspPlanHeads(dev);
    NvxGspAssignSecondarySor(dev);

    if (dev->DisplayActiveIds[0] && dev->MonitorCount > 1 && dev->MonitorIds[0] != dev->DisplayActiveIds[0]) {
        ULONG primIdx = 0;
        for (ULONG m = 1; m < dev->MonitorCount; m++) {
            if (dev->MonitorIds[m] == dev->DisplayActiveIds[0]) {
                primIdx = m;
                break;
            }
        }
        if (primIdx > 0) {
            ULONG tempId = dev->MonitorIds[0];
            ULONG tempBytes = dev->MonitorEdidBytes[0];
            UCHAR tempEdid[2048];
            NVX_DISPLAY_TIMING tempTiming = dev->MonitorTiming[0];
            ULONG tempSor = dev->MonitorSor[0];
            ULONG tempProt = dev->MonitorProtocol[0];

            VideoPortMoveMemory(tempEdid, dev->MonitorEdid[0], sizeof(tempEdid));

            dev->MonitorIds[0] = dev->MonitorIds[primIdx];
            dev->MonitorEdidBytes[0] = dev->MonitorEdidBytes[primIdx];
            VideoPortMoveMemory(dev->MonitorEdid[0], dev->MonitorEdid[primIdx], sizeof(tempEdid));
            dev->MonitorTiming[0] = dev->MonitorTiming[primIdx];
            dev->MonitorSor[0] = dev->MonitorSor[primIdx];
            dev->MonitorProtocol[0] = dev->MonitorProtocol[primIdx];

            dev->MonitorIds[primIdx] = tempId;
            dev->MonitorEdidBytes[primIdx] = tempBytes;
            VideoPortMoveMemory(dev->MonitorEdid[primIdx], tempEdid, sizeof(tempEdid));
            dev->MonitorTiming[primIdx] = tempTiming;
            dev->MonitorSor[primIdx] = tempSor;
            dev->MonitorProtocol[primIdx] = tempProt;

            LogStr("NVX [DISPLAY]: swapped primary active Head 0 monitor to index 0 (id=");
            LogHex(dev->MonitorIds[0]); LogStr(" SOR="); LogHex(dev->MonitorSor[0]);
            LogStr(" secId="); LogHex(dev->MonitorIds[primIdx]); LogStr(" secSOR="); LogHex(dev->MonitorSor[primIdx]); LogStr(")\n");
        }
    }
}

static BOOLEAN NvxGspCreateObjects(PNVX_DEVICE_EXTENSION dev,ULONG rxSequence)
{
    ULONG client[27]={0xc1d00001,0,0x0078766e};
    ULONG device[14]={0},subdevice=0;
    NVX_VASPACE_535 va={0};
    dev->RpcTxSequence=2;dev->RpcRxSequence=rxSequence;
    if(!NvxGspAlloc(dev,0,0xc1d00001,0,client,sizeof(client))) return FALSE;
    dev->GspClientHandle=0xc1d00001;
    device[1]=dev->GspClientHandle; /* hClientShare = client handle (Nouveau r535_gsp_device_ctor) */
    if(!NvxGspAlloc(dev,dev->GspClientHandle,0x80000001,0x80,device,sizeof(device))) return FALSE;
    dev->GspDeviceHandle=0x80000001;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0x20800001,0x2080,&subdevice,4)) return FALSE;
    dev->GspSubdeviceHandle=0x20800001;
    if(!NvxGspGetName(dev)) return FALSE;
    /* Private externally owned VASpace; page directory installed below. */
    va.flags=8;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0x90f10001,0x90f1,&va,sizeof(va))) return FALSE;
    dev->GspVaSpaceHandle=0x90f10001;
    LogStr("NVX [RM]: private GPU virtual address space allocated.\n");
    if(!NvxGspRegisterScratch(dev)) return FALSE;
    if(!NvxGspMapScratch(dev)) return FALSE;
    if(!NvxGspGetFbLayout(dev)) LogStr("NVX [FB]: static VRAM layout unavailable; retaining system-memory copy probe.\n");
    if(!NvxGspSelectCopyEngine(dev)) return FALSE;
    if(!NvxGspAllocChannel(dev)) return FALSE;
    LogStr("NVX [RM]: dedicated channel and copy engine allocated; testing GPU execution.\n");
    if(!NvxGspCopySmokeTest(dev)) return FALSE;
    NvxGspDiscoverDisplays(dev);
    return TRUE;
}

static BOOLEAN NvxWait(PUCHAR bar0, ULONG reg, ULONG mask, ULONG expected,
                       ULONG iterations, ULONG delay)
{
    while (iterations--) {
        ULONG value = VideoPortReadRegisterUlong((PULONG)(bar0 + reg));
        if (value != 0xffffffff && (value & mask) == expected) return TRUE;
        VideoPortStallExecution(delay);
    }
    LogStr("NVX [GSP]: Timeout at register "); LogHex(reg);
    LogStr(" value "); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+reg)));
    LogStr("\n");
    return FALSE;
}

int NvxGspSequencerCore(void *context,unsigned int opcode)
{
    PNVX_DEVICE_EXTENSION dev=context;
    PUCHAR bar0;
    ULONG value,i;
    if (!dev||!dev->Bar0MmioVirtual||!dev->GspDmaVirtual||!dev->GspBootVerified||
        opcode<5||opcode>8) return FALSE;
    bar0=dev->Bar0MmioVirtual;
    LogStr("NVX [SEQ-CORE]: opcode=");LogHex(opcode);LogStr("\n");
    if (opcode==5||opcode==8) {
        for(i=0;i<150;i++) {
            if(VideoPortReadRegisterUlong((PULONG)(bar0+0x1100f4))&0x80000000U) break;
            VideoPortStallExecution(1);
        }
        value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE),value|1U);
        for(i=0;i<10;i++) (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE),value&~1U);
        for(i=0;i<10;i++) (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
        if(!NvxWait(bar0,0x1100f4,0x1000,0,2000,10)) return FALSE;
    }
    if(opcode==5) {
        value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_BCR));
        if(value&0x10) {
            VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_BCR),0);
            if(!NvxWait(bar0,NV_PGSP_FALCON_BCR,1,1,20000,5)) return FALSE;
        }
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x110084),VideoPortReadRegisterUlong((PULONG)bar0));
        value=VideoPortReadRegisterUlong((PULONG)(bar0+0x110624));
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x110624),value|0x80);
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x11010c),0);
        return TRUE;
    }
    if(opcode==6) {
        value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_CPUCTL));
        VideoPortWriteRegisterUlong((PULONG)(bar0+(value&0x40?NV_PGSP_FALCON_CPUCTL_ALIAS:NV_PGSP_FALCON_CPUCTL)),2);
        return TRUE;
    }
    if(opcode==7) return NvxWait(bar0,NV_PGSP_FALCON_CPUCTL,0x10,0x10,40000,50);
    /* GA102 CORE_RESUME: select RISC-V, restore LibOS argument pointer,
       restart the suspended SEC2 RTOS, and verify stage-3 handoff. */
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_BCR),0x111);
    {
        ULONG64 args=dev->GspDmaPhysical.QuadPart+GSP_LIBOS_ARGS_OFFSET;
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX0),(ULONG)args);
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX1),(ULONG)(args>>32));
    }
    value=VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL));
    VideoPortWriteRegisterUlong((PULONG)(bar0+(value&0x40?NV_PSEC_FALCON_CPUCTL_ALIAS:NV_PSEC_FALCON_CPUCTL)),2);
    if(!NvxWait(bar0,0x1180f8,0x04000000,0x04000000,40000,50)||
       VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX0))) return FALSE;
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_OS),0);
    return (VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_RISCV_STAT))&0x80)!=0;
}

static int NvxSeqRead(void *context,uint32_t address,uint32_t *value)
{
    PNVX_DEVICE_EXTENSION dev=context;
    if(!dev||!value||(address&3)||dev->Bar0Length<4||address>dev->Bar0Length-4) return 0;
    *value=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+address));return 1;
}
static int NvxSeqWrite(void *context,uint32_t address,uint32_t value)
{
    PNVX_DEVICE_EXTENSION dev=context;
    if(!dev||(address&3)||dev->Bar0Length<4||address>dev->Bar0Length-4) return 0;
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+address),value);return 1;
}
static int NvxSeqDelay(void *context,uint32_t microseconds)
{
    (void)context;
    while(microseconds) {
        ULONG step=microseconds>50?50:microseconds;
        VideoPortStallExecution(step);microseconds-=step;
    }
    return 1;
}

/*
 * GA102 Falcon enable sequence.  This follows gp102_flcn_reset_eng(),
 * ga102_flcn_select(), and gm200_flcn_enable() instead of overwriting the
 * entire ENGINE register.  The latter is important on a GPU handed to us by
 * the VGA firmware, where unrelated control bits can already be populated.
 */
static BOOLEAN NvxSec2ResetEnable(PUCHAR bar0)
{
    ULONG value;
    ULONG timeout;

    /* GA102 reset preparation is advisory in Nouveau, with a 150us window. */
    (void)VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400f4));
    timeout = 150;
    while (timeout--) {
        if (VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400f4)) & 0x80000000)
            break;
        VideoPortStallExecution(1);
    }

    value = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_ENGINE));
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_ENGINE), value | 1);
    VideoPortStallExecution(10);
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_ENGINE), value & ~1UL);

    if (!NvxWait(bar0, 0x8400f4, 0x00001000, 0, 2000, 10))
        return FALSE;

    /* Select the Falcon core when the second PRI block currently selects RISCV. */
    value = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x841668));
    if (value & 0x10) {
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x841668), 0);
        if (!NvxWait(bar0, 0x841668, 1, 1, 1000, 10))
            return FALSE;
    }

    /* MC enable is dynamically described by TOP on Ampere.  Firmware normally
       leaves SEC2 enabled; record its state rather than guessing a PMC mask. */
    LogStr("NVX [SEC2]: MC_ENABLE 0x000600: 0x");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x000600)));
    LogStr(" BCR: 0x");
    LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x841668)));
    LogStr("\n");

    if (!NvxWait(bar0, 0x8400f4, 0x00001000, 0, 2000, 10))
        return FALSE;

    VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x840084),
                                VideoPortReadRegisterUlong((PULONG)bar0));
    return TRUE;
}

static VOID NvxLogSec2State(PUCHAR bar0, const char *where)
{
    LogStr("NVX [SEC2]: "); LogStr(where);
    LogStr(" HWCFG2="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400f4)));
    LogStr(" CPUCTL="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_CPUCTL)));
    LogStr(" ALIAS="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_CPUCTL_ALIAS)));
    LogStr(" BOOTVEC="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_BOOTVEC)));
    LogStr(" DMACTL="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x84010c)));
    LogStr(" DMACMD="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_DMATRFCMD)));
    LogStr(" FBIF_CTL="); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x840624)));
    LogStr("\n");
}

static BOOLEAN NvxSec2Dma(PUCHAR bar0, ULONG64 physical, ULONG source,
                          ULONG length, BOOLEAN imem)
{
    ULONG offset;
    /* IMEM tags derive from the source offset; physical TCM starts at zero. */
    if (!imem) physical += source;
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFBASE), (ULONG)(physical>>8));
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFBASE1), (ULONG)(physical>>40));
    for (offset=0; offset<length; offset+=256) {
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFMOFF), offset);
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFFBOFF), offset+(imem ? source : 0));
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_DMATRFCMD), imem ? 0x614 : 0x600);
        /* Bit 2 is SEC, not BUSY. Completion is bit 1, for every transfer. */
        if (!NvxWait(bar0,NV_PSEC_FALCON_DMATRFCMD,2,2,20000,5)) return FALSE;
    }
    return TRUE;
}

BOOLEAN NvxGspInitSubsystem(PVOID HwDeviceExtension) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR bar0 = (PUCHAR)devExt->Bar0MmioVirtual;
    ULONG bootloaderLen = src_driver_bootloader_535_113_01_bin_len;
    ULONG booterLoadLen = src_driver_booter_load_535_113_01_bin_len;
    ULONG alignedLen = (bootloaderLen + 0xFF) & ~0xFF;
    ULONG timeout;
    VP_DEVICE_DESCRIPTION devDesc;
    NVX_BOOTER_INFO bt;
    int signatureIndex;
    uint32_t imageOffset, imageSize, signatureOffset, signatureSize;
    ULONG64 firmwareSize = (ULONG64)(_binary_dist_full_gsp_ga104_bin_end -
                                     _binary_dist_full_gsp_ga104_bin_start);
    /* A failed engine may still reference staging memory. Do not replace it. */
    if (devExt->GspDmaVirtual) return devExt->GspInitialized;
    devExt->GspInitialized = FALSE;
    devExt->GspBootVerified = FALSE;

    LogStr("NVX [GSP]: Initializing GA104 GSP Subsystem with authentic binary blobs...\n");
    VideoPortDebugPrint(Info, "NVX [GSP]: Initializing GA104 GSP Subsystem with authentic binary blobs...\n");

    if (!bar0) {
        LogStr("NVX [GSP]: BAR0 MMIO virtual mapping is NULL! Aborting.\n");
        return FALSE;
    }

    LogStr("NVX [GSP]: Embedded Falcon Bootloader size: ");
    LogHex(bootloaderLen);
    LogStr(" bytes (aligned ");
    LogHex(alignedLen);
    LogStr(")\n");
    LogStr("NVX [GSP]: Embedded Booter Load size: ");
    LogHex(booterLoadLen);
    LogStr(" bytes\n");

    /* Reject malformed/mismatched containers before DMA allocation or engine writes. */
    if (!NvxParseBooter(src_driver_booter_load_535_113_01_bin, booterLoadLen, &bt) ||
        !NvxFindElfSection(_binary_dist_full_gsp_ga104_bin_start, firmwareSize,
                          ".fwimage", &imageOffset, &imageSize) ||
        !NvxFindElfSection(_binary_dist_full_gsp_ga104_bin_start, firmwareSize,
                          ".fwsignature_ga10x", &signatureOffset, &signatureSize) ||
        imageSize != GSP_FW_IMAGE_SIZE || signatureSize != GSP_SIG_SIZE ||
        bt.DataSize > GSP_WPR_META_OFFSET-GSP_BOOTER_LOAD_OFFSET ||
        bt.EngineId != 1 || bt.UcodeId > 16) {
        LogStr("NVX [GSP]: Invalid/unsupported firmware container.\n");
        return FALSE;
    }
    signatureIndex = NvxSelectSignature(VideoPortReadRegisterUlong(
        (PULONG)(bar0+0x824140+(bt.UcodeId-1)*4)), bt.FuseVersion, bt.SignatureCount);
    if (signatureIndex < 0) {
        LogStr("NVX [SEC2]: No signature matches GPU fuse version.\n");
        return FALSE;
    }
    LogStr("NVX [SEC2]: Selected signature index "); LogHex(signatureIndex); LogStr("\n");

    /* Obtain DMA adapter from VideoPort */
    VideoPortZeroMemory(&devDesc, sizeof(devDesc));
    devDesc.ScatterGather = TRUE;
    devDesc.Dma32BitAddresses = TRUE;
    devDesc.Dma64BitAddresses = FALSE;
    devDesc.MaximumLength = 0x3000000; /* 48 MB */

    devExt->DmaAdapter = VideoPortGetDmaAdapter(HwDeviceExtension, &devDesc);
    LogStr("NVX [GSP]: VideoPortGetDmaAdapter returned: 0x");
    LogHex((unsigned long long)devExt->DmaAdapter);
    LogStr("\n");

    if (!devExt->DmaAdapter) return FALSE;

    /* Allocate 48MB contiguous coherent DMA memory for GSP staging, Radix-3 table, and queues */
    devExt->GspDmaLength = 0x3000000; /* 48 MB */
    devExt->GspDmaVirtual = VideoPortAllocateCommonBuffer(
        HwDeviceExtension,
        devExt->DmaAdapter,
        devExt->GspDmaLength,
        &devExt->GspDmaPhysical,
        TRUE,
        NULL
    );

    if (!devExt->GspDmaVirtual) {
        LogStr("NVX [GSP]: Failed to allocate contiguous DMA buffer for GSP (48MB)!\n");
        VideoPortDebugPrint(Error, "NVX [GSP]: Failed to allocate contiguous DMA buffer for GSP (%d bytes)\n", devExt->GspDmaLength);
        VideoPortPutDmaAdapter(HwDeviceExtension, devExt->DmaAdapter);
        devExt->DmaAdapter = NULL;
        return FALSE;
    }
    if ((devExt->GspDmaPhysical.QuadPart & 0xfff) ||
        (ULONG64)devExt->GspDmaPhysical.QuadPart + devExt->GspDmaLength > 0x100000000ULL) {
        LogStr("NVX [GSP]: DMA allocation is not aligned/below 4GB.\n");
        NvxGspShutdown(HwDeviceExtension);
        return FALSE;
    }

    /* Parse authentic NVFW container header (Ported from Linux Nouveau nvfw_bin_hdr) */
    PNVFW_BIN_HDR blHdr = (PNVFW_BIN_HDR)src_driver_bootloader_535_113_01_bin;
    PRM_RISCV_UCODE_DESC blDesc = NULL;
    ULONG blCodeOffset = 0;
    ULONG blCodeSize = bootloaderLen;

    if (blHdr->BinMagic == 0x10DE) {
        LogStr("NVX [GSP]: Authentic NVFW container header detected!\n");
        LogStr("NVX [GSP]:   BinVer: 0x"); LogHex(blHdr->BinVer);
        LogStr(" HdrOff: 0x"); LogHex(blHdr->HeaderOffset);
        LogStr(" DataOff: 0x"); LogHex(blHdr->DataOffset);
        LogStr(" DataSize: 0x"); LogHex(blHdr->DataSize);
        LogStr("\n");
        blCodeOffset = blHdr->DataOffset;
        blCodeSize = blHdr->DataSize;
        if (blHdr->HeaderOffset <= bootloaderLen - sizeof(RM_RISCV_UCODE_DESC)) {
            blDesc = (PRM_RISCV_UCODE_DESC)((PUCHAR)src_driver_bootloader_535_113_01_bin + blHdr->HeaderOffset);
            LogStr("NVX [GSP]:   RISC-V AppVersion: 0x"); LogHex(blDesc->AppVersion);
            LogStr(" MonitorCodeOff: 0x"); LogHex(blDesc->MonitorCodeOffset);
            LogStr(" ManifestOff: 0x"); LogHex(blDesc->ManifestOffset);
            LogStr("\n");
        }
    }

    if (!blDesc || blHdr->BinVer != 1 || blCodeOffset > bootloaderLen ||
        blCodeSize > bootloaderLen-blCodeOffset ||
        blCodeSize > GSP_BOOTER_LOAD_OFFSET-GSP_BOOTLOADER_OFFSET ||
        blDesc->MonitorCodeOffset >= blCodeSize || blDesc->MonitorDataOffset >= blCodeSize ||
        blDesc->ManifestOffset >= blCodeSize) {
        LogStr("NVX [GSP]: Invalid bootloader descriptor.\n");
        NvxGspShutdown(HwDeviceExtension);
        return FALSE;
    }
    ULONG btCodeOffset = bt.DataOffset;
    ULONG btCodeSize = bt.DataSize;

    /* Zero entire 48MB common buffer */
    VideoPortZeroMemory(devExt->GspDmaVirtual, devExt->GspDmaLength);

    /*
     * STAGE 1: Copy authentic GSP Firmware Image (.fwimage section from gsp_ga104.bin)
     * Size: 38,043,648 bytes (0x2448000), 9288 4KB pages.
     */
    LogStr("NVX [GSP]: Staging authentic GSP .fwimage (36.28MB) at DMA offset 0x0...\n");
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_FW_IMAGE_OFFSET,
                        (PVOID)(_binary_dist_full_gsp_ga104_bin_start + imageOffset),
                        (ULONG)GSP_FW_IMAGE_SIZE);

    /*
     * STAGE 2: Copy authentic GA10x Cryptographic Signature (.fwsignature_ga10x)
     * Size: 4096 bytes (0x1000).
     */
    LogStr("NVX [GSP]: Staging authentic GA10x signature (4KB) at DMA offset 0x");
    LogHex(GSP_SIG_OFFSET);
    LogStr("...\n");
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_SIG_OFFSET,
                        (PVOID)(_binary_dist_full_gsp_ga104_bin_start + signatureOffset),
                        (ULONG)GSP_SIG_SIZE);

    /*
     * STAGE 3: Construct Radix-3 Page Table for GSP Firmware Image.
     * Level 0: 1 4KB page at GSP_RADIX3_LVL0_OFFSET (points to Level 1 page)
     * Level 1: 1 4KB page at GSP_RADIX3_LVL1_OFFSET (19 entries pointing to Level 2 pages)
     * Level 2: 19 4KB pages at GSP_RADIX3_LVL2_OFFSET (9288 entries pointing to .fwimage pages)
     */
    LogStr("NVX [GSP]: Constructing Radix-3 Page Table (21 pages / 84KB)...\n");
    {
        PULONG64 pRadixLvl0 = (PULONG64)((PUCHAR)devExt->GspDmaVirtual + GSP_RADIX3_LVL0_OFFSET);
        PULONG64 pRadixLvl1 = (PULONG64)((PUCHAR)devExt->GspDmaVirtual + GSP_RADIX3_LVL1_OFFSET);
        PULONG64 pRadixLvl2 = (PULONG64)((PUCHAR)devExt->GspDmaVirtual + GSP_RADIX3_LVL2_OFFSET);
        ULONG numPages = (ULONG)((GSP_FW_IMAGE_SIZE + 4095) / 4096); /* 9288 pages */
        ULONG i, k;

        /* Level 0: 1 entry pointing to Level 1 page physical address */
        pRadixLvl0[0] = devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL1_OFFSET;

        /* Level 1: 19 entries, each pointing to a Level 2 page physical address */
        for (i = 0; i < 19; i++) {
            pRadixLvl1[i] = (devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL2_OFFSET) + ((ULONG64)i * 4096ULL);
        }

        /* Level 2: 9288 entries, each pointing to a 4KB page of .fwimage */
        for (k = 0; k < numPages; k++) {
            pRadixLvl2[k] = (devExt->GspDmaPhysical.QuadPart + GSP_FW_IMAGE_OFFSET) + ((ULONG64)k * 4096ULL);
        }

        LogStr("NVX [GSP]: Radix-3 Level 0 at Phys 0x");
        LogHex(devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL0_OFFSET);
        LogStr(", Level 1 at Phys 0x");
        LogHex(devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL1_OFFSET);
        LogStr(", Level 2 at Phys 0x");
        LogHex(devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL2_OFFSET);
        LogStr("\n");
    }

    /* STAGE 4: Stage Bootloader ucode at GSP_BOOTLOADER_OFFSET */
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_BOOTLOADER_OFFSET,
                        (PUCHAR)src_driver_bootloader_535_113_01_bin + blCodeOffset,
                        blCodeSize);

    /* STAGE 5: Stage Booter Load ucode at GSP_BOOTER_LOAD_OFFSET & patch RSA signature */
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_BOOTER_LOAD_OFFSET,
                        (PUCHAR)src_driver_booter_load_535_113_01_bin + btCodeOffset,
                        btCodeSize);
    VideoPortMoveMemory((PUCHAR)devExt->GspDmaVirtual + GSP_BOOTER_LOAD_OFFSET + bt.PatchOffset,
                        src_driver_booter_load_535_113_01_bin + bt.SignatureOffset + signatureIndex * bt.SignatureSize,
                        bt.SignatureSize);

    /*
     * STAGE 6: Authentic GSP firmware WPR metadata V1 in DMA buffer at GSP_WPR_META_OFFSET.
     * This metadata is passed to the SEC2 booter and GSP bootloader.
     */
    PGSP_FW_WPR_META_V1 wprMeta = (PGSP_FW_WPR_META_V1)((PUCHAR)devExt->GspDmaVirtual + GSP_WPR_META_OFFSET);
    PHYSICAL_ADDRESS wprPhys;
    wprPhys.QuadPart = devExt->GspDmaPhysical.QuadPart + GSP_WPR_META_OFFSET;

    VideoPortZeroMemory(wprMeta, sizeof(GSP_FW_WPR_META_V1));
    wprMeta->Magic = GSP_FW_WPR_META_MAGIC;
    wprMeta->Revision = GSP_FW_WPR_META_REVISION;

    /* SYSMEM pointers for SEC2 to verify and copy */
    wprMeta->SysmemAddrOfRadix3Elf = devExt->GspDmaPhysical.QuadPart + GSP_RADIX3_LVL0_OFFSET;
    wprMeta->SizeOfRadix3Elf = GSP_FW_IMAGE_SIZE;
    wprMeta->SysmemAddrOfSignature = devExt->GspDmaPhysical.QuadPart + GSP_SIG_OFFSET;
    wprMeta->SizeOfSignature = GSP_SIG_SIZE;

    wprMeta->SysmemAddrOfBootloader = devExt->GspDmaPhysical.QuadPart + GSP_BOOTLOADER_OFFSET;
    wprMeta->SizeOfBootloader = blCodeSize;
    wprMeta->BootloaderCodeOffset = blDesc ? blDesc->MonitorCodeOffset : 0x1000;
    wprMeta->BootloaderDataOffset = blDesc ? blDesc->MonitorDataOffset : 0x800;
    wprMeta->BootloaderManifestOffset = blDesc ? blDesc->ManifestOffset : 0x0;

    /* Authentic GA104 (8GB VRAM) Descending Framebuffer Layout */
    wprMeta->FbSize = (ULONG64)VideoPortReadRegisterUlong((PULONG)(bar0+0x1183a4)) << 20;
    if (wprMeta->FbSize < 0x20000000ULL || wprMeta->FbSize > 0x1000000000ULL) {
        LogStr("NVX [GSP]: Invalid hardware VRAM size.\n");
        NvxGspShutdown(HwDeviceExtension);
        return FALSE;
    }
    LogStr("NVX [GSP]: Physical VRAM bytes "); LogHex(wprMeta->FbSize); LogStr("\n");
    wprMeta->VgaWorkspaceOffset = wprMeta->FbSize - 0x100000ULL;
    {
        ULONG workspace = VideoPortReadRegisterUlong((PULONG)(bar0+0x625f04));
        if (workspace & 8) {
            ULONG64 addr = (ULONG64)(workspace & 0xffffff00) << 8;
            wprMeta->VgaWorkspaceOffset = addr < wprMeta->VgaWorkspaceOffset ?
                wprMeta->FbSize - 0x20000 : addr;
        }
        if (wprMeta->VgaWorkspaceOffset >= wprMeta->FbSize) {
            LogStr("NVX [GSP]: Invalid VGA workspace.\n");
            NvxGspShutdown(HwDeviceExtension);
            return FALSE;
        }
    }
    wprMeta->VgaWorkspaceSize = wprMeta->FbSize - wprMeta->VgaWorkspaceOffset;
    wprMeta->GspFwWprEnd = wprMeta->VgaWorkspaceOffset & ~0x1ffffULL;
    wprMeta->FrtsSize = 0x100000ULL;
    wprMeta->FrtsOffset = wprMeta->GspFwWprEnd - wprMeta->FrtsSize;
    if (devExt->FwsecFrtsReady) {
        if (devExt->VerifiedWprEnd < 0x100000ULL ||
            devExt->VerifiedFrtsOffset != devExt->VerifiedWprEnd-0x100000ULL ||
            devExt->VerifiedWprEnd > wprMeta->FbSize ||
            devExt->VerifiedVgaWorkspaceOffset >= wprMeta->FbSize) {
            LogStr("NVX [GSP]: Invalid verified FRTS layout; bootstrap blocked.\n");
            return FALSE;
        }
        wprMeta->VgaWorkspaceOffset=devExt->VerifiedVgaWorkspaceOffset;
        wprMeta->VgaWorkspaceSize=wprMeta->FbSize-wprMeta->VgaWorkspaceOffset;
        wprMeta->GspFwWprEnd=devExt->VerifiedWprEnd;
        wprMeta->FrtsOffset=devExt->VerifiedFrtsOffset;
        LogStr("NVX [GSP]: Using verified WPR end ");LogHex(wprMeta->GspFwWprEnd);
        LogStr(" FRTS ");LogHex(wprMeta->FrtsOffset);LogStr("\n");
    }
    wprMeta->BootBinOffset = (wprMeta->FrtsOffset - wprMeta->SizeOfBootloader) & ~0xFFFULL;
    wprMeta->GspFwOffset = (wprMeta->BootBinOffset - wprMeta->SizeOfRadix3Elf) & ~0xFFFFULL;
    /* 535 LibOS2: 8MB base + 1MB per-GB allowance + 96MB client heap. */
    wprMeta->GspFwHeapSize = 0x6800000ULL +
        ((((wprMeta->FbSize + 0x3fffffffULL) >> 30) * (96ULL << 10) + 0xfffffULL) & ~0xfffffULL);
    wprMeta->GspFwHeapOffset = (wprMeta->GspFwOffset - wprMeta->GspFwHeapSize) & ~0xFFFFFULL;
    wprMeta->GspFwHeapSize = (wprMeta->GspFwOffset-wprMeta->GspFwHeapOffset) & ~0xfffffULL;
    wprMeta->GspFwWprStart = (wprMeta->GspFwHeapOffset-sizeof(*wprMeta)) & ~0xfffffULL;
    wprMeta->NonWprHeapSize = 0x100000ULL;
    wprMeta->NonWprHeapOffset = wprMeta->GspFwWprStart - wprMeta->NonWprHeapSize;
    wprMeta->GspFwRsvdStart = wprMeta->NonWprHeapOffset;
    wprMeta->BootCount = 0;
    wprMeta->Verified = 0;

    /* 535 shared memory: PTE page followed by two 256KB queues. */
    {
        ULONG i;
        PULONG64 ptes = (PULONG64)((PUCHAR)devExt->GspDmaVirtual+GSP_SHARED_OFFSET);
        NVX_MSGQ_HEADER *cmd = (NVX_MSGQ_HEADER *)((PUCHAR)devExt->GspDmaVirtual+GSP_CMD_QUEUE_OFFSET);
        PGSP_ARGUMENTS_CACHED args = (PGSP_ARGUMENTS_CACHED)((PUCHAR)devExt->GspDmaVirtual+GSP_RM_ARGS_OFFSET);
        for (i=0;i<129;i++) ptes[i] = devExt->GspDmaPhysical.QuadPart+GSP_SHARED_OFFSET+(ULONG64)i*4096;
        cmd->Size = 0x40000; cmd->MsgSize = 4096; cmd->MsgCount = 63;
        cmd->Flags = 1; cmd->RxHdrOff = 32; cmd->EntryOff = 4096;
        args->MessageQueueInitArguments.SharedMemPhysAddr = devExt->GspDmaPhysical.QuadPart+GSP_SHARED_OFFSET;
        args->MessageQueueInitArguments.PageTableEntryCount = 129;
        args->MessageQueueInitArguments.CmdQueueOffset = 0x1000;
        args->MessageQueueInitArguments.StatQueueOffset = 0x41000;
        devExt->CmdQueueVirtual = cmd;
        devExt->StatQueueVirtual = (PUCHAR)devExt->GspDmaVirtual+GSP_STAT_QUEUE_OFFSET;
        devExt->CmdQueueSize = devExt->StatQueueSize = 0x40000;
        {
            NVX_SYSTEM_INFO_535 sys;
            ULONG registry[2]={8,0};
            PUCHAR entries=(PUCHAR)cmd+4096;
            VideoPortZeroMemory(&sys,sizeof(sys));
            sys.gpuPhysAddr=devExt->PhysicalBar0.QuadPart;
            sys.gpuPhysFbAddr=devExt->PhysicalBar1.QuadPart;
            sys.gpuPhysInstAddr=devExt->PhysicalInstBar.QuadPart;
            sys.dbdf=((ULONG64)devExt->PciBusNumber<<8)|(devExt->PciSlot&31U);
            /* Conservative user VA ceiling until OS capabilities are plumbed. */
            sys.maxUserVa=0x7ffeffffULL;
            sys.mirrorBase=0x88000;sys.mirrorSize=0x1000;
            sys.hypervisor=3;sys.passthrough=TRUE;
            if (!NvxEncodeRpc(entries,4096,72,0,&sys,sizeof(sys)) ||
                !NvxEncodeRpc(entries+4096,4096,73,1,registry,sizeof(registry))) {
                LogStr("NVX [RPC]: startup packet preparation failed.\n");return FALSE;
            }
            LogStr("NVX [RPC]: prepared SYSTEM_INFO bytes=");LogHex(sizeof(sys));
            LogStr(" BAR0=");LogHex(sys.gpuPhysAddr);LogStr(" BAR1=");LogHex(sys.gpuPhysFbAddr);
            LogStr(" INST=");LogHex(sys.gpuPhysInstAddr);LogStr(" DBDF=");LogHex(sys.dbdf);
            LogStr("; empty REGISTRY prepared.\n");
            /* Publish both complete slots before Booter can start RM. */
            __sync_synchronize();
            cmd->WritePtr=2;
            __sync_synchronize();
            LogStr("NVX [RPC]: published startup SYSTEM_INFO then REGISTRY; write=2.\n");
        }
    }
    PLIBOS_MEMORY_REGION_INIT_ARGUMENT libosArgs = (PLIBOS_MEMORY_REGION_INIT_ARGUMENT)((PUCHAR)devExt->GspDmaVirtual+GSP_LIBOS_ARGS_OFFSET);
    PHYSICAL_ADDRESS libosPhys;
    libosPhys.QuadPart = devExt->GspDmaPhysical.QuadPart+GSP_LIBOS_ARGS_OFFSET;
    {
        static const ULONG64 ids[] = {0x4c4f47494e4954ULL, 0x4c4f47494e5452ULL, 0x4c4f47524dULL, 0x524d41524753ULL};
        ULONG i,j;
        for (i=0;i<4;i++) {
            ULONG64 off = i==3 ? GSP_RM_ARGS_OFFSET : 0x2581000ULL+(ULONG64)i*0x10000;
            libosArgs[i].Id8 = ids[i];
            libosArgs[i].Pa = devExt->GspDmaPhysical.QuadPart+off;
            libosArgs[i].Size = i==3 ? 4096 : 65536;
            libosArgs[i].Kind = libosArgs[i].Loc = 1;
            if (i<3) {
                PULONG64 log = (PULONG64)((PUCHAR)devExt->GspDmaVirtual+off);
                for (j=0;j<16;j++) log[j+1] = libosArgs[i].Pa+(ULONG64)j*4096;
            }
        }
    }

    LogStr("NVX [GSP]: Staging DMA buffer at Virtual 0x");
    LogHex((unsigned long long)devExt->GspDmaVirtual);
    LogStr(", Physical 0x");
    LogHex(devExt->GspDmaPhysical.QuadPart);
    LogStr("\n");
    LogStr("NVX [GSP]: WPR Metadata at Physical 0x");
    LogHex(wprPhys.QuadPart);
    LogStr("\n");

    {
        ULONG boot0 = VideoPortReadRegisterUlong((PULONG)bar0);
        ULONG flcnStat = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1100F4));
        ULONG sec2Stat = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x8400F4));
        ULONG dispOwner = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x6254E8));
        ULONG dispCore = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x6104E0));
        ULONG dispPut = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x680000));
        ULONG dispDma = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x610B24));
        ULONG headRg = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x616300));
        LogStr("NVX [GSP]: BAR0[0]: 0x"); LogHex(boot0);
        LogStr(" Flcn 0x1100F4: 0x"); LogHex(flcnStat);
        LogStr(" SEC2 0x8400F4: 0x"); LogHex(sec2Stat);
        LogStr("\n");
        LogStr("NVX [DISP]: Owner(0x6254E8): 0x"); LogHex(dispOwner);
        LogStr(" Core(0x6104E0): 0x"); LogHex(dispCore);
        LogStr(" Put(0x680000): 0x"); LogHex(dispPut);
        LogStr(" Dma(0x610B24): 0x"); LogHex(dispDma);
        LogStr(" HeadCapability(0x616300): 0x"); LogHex(headRg);
        LogStr("\n");
    }

    /*
     * STEP 1: Reset GSP Falcon, enable RISC-V mode, and program boot arguments.
     * In GA104, GSP RISC-V is unlocked and booted by the SEC2 booter load ucode.
     */
    LogStr("NVX [GSP]: Preparing GSP Falcon for RISC-V execution...\n");

    /* Wait for Falcon reset prep */
    timeout = 1000;
    while (!(VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1100F4)) & 0x80000000) && --timeout) {
        VideoPortStallExecution(10);
    }

    /* Reset GSP Falcon engine */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_ENGINE), 1);
    VideoPortStallExecution(20);
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_ENGINE), 0);
    VideoPortStallExecution(20);

    /* Wait for GSP memory scrubbing to finish */
    timeout = 2000;
    while ((VideoPortReadRegisterUlong((PULONG)(bar0 + 0x1100F4)) & 0x00001000) && --timeout) {
        VideoPortStallExecution(10);
    }
    if (timeout == 0) {
        LogStr("NVX [GSP]: GSP memory scrubbing wait timeout!\n");
        return FALSE;
    } else {
        LogStr("NVX [GSP]: GSP memory scrubbing complete!\n");
    }

    /* Put GSP Falcon into RISC-V mode (0x111668 |= 0x111) */
    {
        ULONG bcr = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_BCR));
        VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_BCR), bcr | 0x111);
    }

    /* Pass Libos init arguments physical base to GSP Mailbox 0 / 1 */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_MAILBOX0),
                               (ULONG)(libosPhys.QuadPart & 0xFFFFFFFF));
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_MAILBOX1),
                               (ULONG)(libosPhys.QuadPart >> 32));

    /*
     * STEP 2: Execute Booter Load on SEC2 Falcon (0x840000).
     * SEC2 verifies the WPR metadata, initializes WPR in VRAM, loads the bootloader into GSP, and unlocks GSP.
     */
    LogStr("NVX [SEC2]: Preparing SEC2 Falcon to execute Booter Load...\n");

    NvxLogSec2State(bar0, "before reset");
    if (!NvxSec2ResetEnable(bar0)) {
        LogStr("NVX [SEC2]: Reset/enable sequence failed.\n");
        return FALSE;
    }
    NvxLogSec2State(bar0, "after reset");

    /* Setup SEC2 DMA interface */
    {
        ULONG val = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x840624));
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x840624), val | 0x80);
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x84010C), 0);
        val = VideoPortReadRegisterUlong((PULONG)(bar0 + 0x840600));
        VideoPortWriteRegisterUlong((PULONG)(bar0 + 0x840600), (val & ~0x00010007) | 5);
    }

    ULONG64 booterPhys = devExt->GspDmaPhysical.QuadPart+GSP_BOOTER_LOAD_OFFSET;
    if (!NvxSec2Dma(bar0,booterPhys,bt.ImemOffset,bt.ImemSize,TRUE) ||
        !NvxSec2Dma(bar0,booterPhys,bt.DmemOffset,bt.DmemSize,FALSE)) return FALSE;

    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_PARAADDR),bt.PatchOffset-bt.DmemOffset);
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_ENGIDMASK),bt.EngineId);
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_UCODE_ID),bt.UcodeId);
    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON2_MOD_SEL),1);

    /* Provide WPR metadata address in SEC2 mailboxes */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_MAILBOX0), (ULONG)(wprPhys.QuadPart & 0xFFFFFFFF));
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_MAILBOX1), (ULONG)(wprPhys.QuadPart >> 32));

    /* Check CPUCTL and start CPU.  Turing+ Falcons require the alias register
       when CPUCTL.ALIAS_EN is set; this is the exact OpenRM 535 behavior. */
    ULONG cpuCtl = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PSEC_FALCON_CPUCTL));
    LogStr("NVX [SEC2]: CPUCTL before start: 0x"); LogHex(cpuCtl); LogStr("\n");

    VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_BOOTVEC),bt.ImemOffset);
    if (cpuCtl & 0x40) {
        LogStr("NVX [SEC2]: Starting through CPUCTL_ALIAS.\n");
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL_ALIAS),2);
    } else {
        LogStr("NVX [SEC2]: Starting through CPUCTL.\n");
        VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL),2);
    }
    NvxLogSec2State(bar0, "immediately after start");
    /* Boot ROM authentication may finish before we observe RUNNING. Require
       HALTED + zero status + verified metadata, rather than seeing an edge. */
    {
        BOOLEAN sawRunning = FALSE;
        ULONG lastCtl = cpuCtl;
        timeout = 100000;
        while (timeout) {
            ULONG ctl = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL));
            ULONG status = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX0));
            ULONG64 verified = ((volatile GSP_FW_WPR_META_V1 *)wprMeta)->Verified;
            if (ctl != lastCtl) {
                LogStr("NVX [SEC2]: CPUCTL transition: 0x"); LogHex(lastCtl);
                LogStr(" -> 0x"); LogHex(ctl); LogStr("\n");
                lastCtl = ctl;
            }
            if (ctl != 0xffffffff) {
                if (!(ctl & 0x10)) sawRunning = TRUE;
                if ((ctl & 0x10) && (sawRunning || status != (ULONG)wprPhys.QuadPart ||
                                     verified == GSP_FW_WPR_VERIFIED)) break;
            }
            timeout--;
            VideoPortStallExecution(50);
        }
        if (!timeout) {
            LogStr("NVX [SEC2]: Execution timeout; stale HALTED is not completion.\n");
            return FALSE;
        }
    }
    {
        ULONG status = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX0));
        ULONG status1 = VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_MAILBOX1));
        ULONG64 verified = ((volatile GSP_FW_WPR_META_V1 *)wprMeta)->Verified;
        LogStr("NVX [SEC2]: Halted. Mailbox0: "); LogHex(status);
        LogStr(" Mailbox1: "); LogHex(status1);
        LogStr(" Verified: "); LogHex(verified);
        LogStr(" SEC2 CPUCTL: "); LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_CPUCTL)));
        LogStr("\n");
        /* Booter authenticates/copies metadata into protected framebuffer.
           The input sysmem copy need not receive the Verified writeback.
           OpenRM accepts a completed Booter with mailbox0 == 0. */
        if (status) {
            LogStr("NVX [SEC2]: Booter failed; RM startup skipped.\n");
            return FALSE;
        }
        devExt->GspBootVerified = TRUE;
    }

    /*
     * STEP 3: Setup GSP Falcon OS version and verify RISC-V execution.
     */
    VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_OS), blDesc ? blDesc->AppVersion : 0);

    {
        ULONG riscvStat = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_RISCV_STAT));
        ULONG bcrVal = VideoPortReadRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_BCR));
        LogStr("NVX [GSP]: RISC-V Active (0x111388): 0x"); LogHex(riscvStat);
        LogStr(" BCR (0x111668): 0x"); LogHex(bcrVal);
        LogStr("\n");
    }

    /* No 0x55aa55aa mailbox handshake exists in RM 535. RISC-V activity
       and WPR verification do not establish RM readiness. Poll the real queue. */
    LogStr("NVX [GSP]: Waiting for RM INIT_DONE event (0x1001).\n");
    ULONG rxSequence=0;
    timeout = 20000;
    while (timeout--) {
        volatile NVX_MSGQ_HEADER *msg = (volatile NVX_MSGQ_HEADER *)devExt->StatQueueVirtual;
        volatile NVX_MSGQ_HEADER *local=(volatile NVX_MSGQ_HEADER *)devExt->CmdQueueVirtual;
        ULONG write=msg->WritePtr,read=local->ReadPtr;
        if (write>=63||read>=63) {
            LogStr("NVX [RPC]: invalid queue pointer; receive stopped.\n");return FALSE;
        }
        if (write!=read) {
            uint32_t function,result,sequence,elements=0,decodedElements=0;
            const uint8_t *entry=devExt->RpcRxRecord;
            int gathered;
            BOOLEAN consumed=FALSE;
            __sync_synchronize();
            gathered=NvxGatherRpc((const uint8_t *)devExt->StatQueueVirtual+4096,
                63*4096,63,read,write,devExt->RpcRxRecord,sizeof(devExt->RpcRxRecord),&elements);
            if (!gathered) { VideoPortStallExecution(100);continue; }
            if (gathered<0) {
                LogStr("NVX [RPC]: invalid record element count; receive stopped.\n");return FALSE;
            }
            if (msg->MsgSize!=4096||msg->MsgCount!=63||msg->EntryOff!=4096||
                !NvxDecodeRpcRecord(entry,elements*4096,&function,&result,&sequence,&decodedElements)||
                decodedElements!=elements||sequence!=rxSequence) {
                const uint32_t *raw=(const uint32_t *)entry;
                LogStr("NVX [RPC]: invalid checksum/header/sequence; read=");LogHex(read);
                LogStr(" write=");LogHex(write);LogStr(" expected_seq=");LogHex(rxSequence);
                LogStr(" seq=");LogHex(raw[9]);LogStr(" elements=");LogHex(raw[10]);
                LogStr(" version=");LogHex(raw[12]);LogStr(" signature=");LogHex(raw[13]);
                LogStr(" length=");LogHex(raw[14]);LogStr(" function=");LogHex(raw[15]);
                LogStr(" result=");LogHex(raw[16]);LogStr("; receive stopped.\n");return FALSE;
            }
            /* Avoid serial output before acknowledging the sequencer. */
            if(function!=0x1002||result) {
                LogStr("NVX [RPC]: startup event=");LogHex(function);
                LogStr(" result=");LogHex(result);LogStr(" sequence=");LogHex(sequence);LogStr("\n");
            }
            if (function==0x1002 && !result) {
                const uint32_t *words=(const uint32_t *)entry;
                const uint32_t *params=words+20;
                uint32_t count=0;
                if (!NvxValidateSeq(params,words[14]-32,devExt->Bar0Length,&count)) {
                    LogStr("NVX [SEQ]: rejected malformed sequencer request.\n");return FALSE;
                }
                /* Match OpenRM receive ordering: acknowledge the validated
                   copy before dispatch or slow serial diagnostics. */
                __sync_synchronize();local->ReadPtr=(read+elements)%63;__sync_synchronize();
                rxSequence++;consumed=TRUE;
                LogStr("NVX [SEQ]: validated capacity=");LogHex(params[0]);
                LogStr(" used=");LogHex(params[1]);LogStr(" commands=");LogHex(count);LogStr("\n");
                {
                    NVX_SEQ_IO io={devExt,NvxSeqRead,NvxSeqWrite,NvxSeqDelay,NvxGspSequencerCore};
                    uint32_t failedWord=0;
                    if(!NvxExecuteSeq((uint32_t *)params,words[14]-32,devExt->Bar0Length,&io,&failedWord)) {
                        LogStr("NVX [SEQ]: execution failed at word=");LogHex(failedWord);
                        if(failedWord<params[1]) {
                            LogStr(" opcode=");LogHex(params[10+failedWord]);
                            if(params[10+failedWord]==2) {
                                ULONG reg=params[11+failedWord];
                                LogStr(" reg=");LogHex(reg);
                                LogStr(" value=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+reg)));
                                LogStr(" mask=");LogHex(params[12+failedWord]);
                                LogStr(" expected=");LogHex(params[13+failedWord]);
                            }
                        }
                        LogStr("; staging retained.\n");return FALSE;
                    }
                    LogStr("NVX [SEQ]: execution completed.\n");
                }
            }
            if (function==0x100c && !result) {
                /* RM 535 rpc_ucode_libos_print_v1E_08: two U32s then bytes.
                   Diagnostic notification, no reply or register operations. */
                const uint32_t *words=(const uint32_t *)entry;
                uint32_t bytes=words[14]-32;
                if(bytes<8 || words[21]>bytes-8) {
                    LogStr("NVX [LIBOS]: malformed print payload retained.\n");return FALSE;
                }
                LogStr("NVX [LIBOS]: engine=");LogHex(words[20]);
                LogStr(" bytes=");LogHex(words[21]);LogStr("\n");
                for(uint32_t i=0;i<words[21] && i<256;i+=4) {
                    uint32_t value=0;
                    for(uint32_t j=0;j<4 && i+j<words[21];j++)
                        value|=(uint32_t)entry[88+i+j]<<(8*j);
                    LogStr("NVX [LIBOS-WORD]: offset=");LogHex(i);
                    LogStr(" value=");LogHex(value);LogStr("\n");
                }
            }
            if (result || (function!=72 && function!=73 && function!=0x1001 && function!=0x101b && function!=0x1002 && function!=0x100c)) {
                LogStr("NVX [RPC]: unhandled startup event retained for diagnosis.\n");return FALSE;
            }
            if (function==0x101b) {
                /* 535 GspUserSharedData contains gspAssertCount. */
                const uint32_t *words=(const uint32_t *)entry;
                if (words[14]<36) return FALSE;
                devExt->GspAssertCount=words[20];
                LogStr("NVX [RPC]: firmware assertion count=");
                LogHex(devExt->GspAssertCount);LogStr("\n");
            }
            if(!consumed) {
                __sync_synchronize();local->ReadPtr=(read+elements)%63;__sync_synchronize();
                rxSequence++;
            }
            if (function==0x1001) {
                if (!(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_RISCV_STAT))&0x80))
                    return FALSE;
                devExt->GspInitialized=TRUE;
                LogStr("NVX [GSP]: RM INIT_DONE received; display channels still unavailable.\n");
                if(!NvxGspCreateObjects(devExt,rxSequence))
                    LogStr("NVX [RM]: acceleration bring-up incomplete; software display retained.\n");
                return TRUE;
            }
            continue;
        }
        VideoPortStallExecution(100);
    }
    LogStr("NVX [GSP]: RM INIT_DONE timeout; capturing bounded startup diagnostics.\n");
    {
        volatile ULONG *queue=(volatile ULONG *)devExt->StatQueueVirtual;
        ULONG i,j;
        LogStr("NVX [RM-DIAG]: status queue header");
        for (i=0;i<9;i++) { LogStr(" ");LogHex(queue[i]); }
        LogStr("\n");
        LogStr("NVX [RM-DIAG]: host command write=");
        LogHex(((volatile NVX_MSGQ_HEADER *)devExt->CmdQueueVirtual)->WritePtr);
        LogStr(" firmware command read=");LogHex(((volatile NVX_MSGQ_HEADER *)devExt->StatQueueVirtual)->ReadPtr);
        LogStr("\n");
        for (i=0;i<3;i++) {
            volatile UCHAR *log=(volatile UCHAR *)devExt->GspDmaVirtual+
                0x2581000ULL+(ULONG64)i*0x10000;
            ULONG nonzero=0,printed=0;
            LogStr("NVX [RM-DIAG]: log index=");LogHex(i);
            LogStr(" header");
            for (j=0;j<8;j++) { LogStr(" ");LogHex(((volatile ULONG64 *)log)[j]); }
            LogStr("\n");
            /* 535 LibOS PUT counts 64-bit ring entries, excluding PUT itself.
               Preserve the latest 512 words exactly for offline decoding. */
            {
                volatile ULONG64 *words=(volatile ULONG64 *)log;
                ULONG64 put=words[0],first=put>512 ? put-512 : 0,k;
                LogStr("NVX [RM-RING]: log=");LogHex(i);
                LogStr(" put=");LogHex(put);LogStr(" first=");LogHex(first);LogStr("\n");
                for (k=first;k<put;k++) {
                    LogStr("NVX [RM-WORD]: log=");LogHex(i);
                    LogStr(" index=");LogHex(k);
                    LogStr(" value=");LogHex(words[1+(k%8191)]);LogStr("\n");
                }
            }
            for (j=128;j<65536;j++) if (log[j]) nonzero++;
            (void)printed;
            LogStr("\nNVX [RM-DIAG]: nonzero_bytes=");LogHex(nonzero);LogStr("\n");
        }
        LogStr("NVX [RM-DIAG]: riscv=");
        LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_RISCV_STAT)));
        LogStr(" mailbox0=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX0)));
        LogStr(" mailbox1=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_MAILBOX1)));
        LogStr("\n");
    }
    return FALSE;
}

VOID NvxGspShutdown(PVOID HwDeviceExtension) {
    PNVX_DEVICE_EXTENSION devExt = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR bar0 = (PUCHAR)devExt->Bar0MmioVirtual;

    if (devExt->GspDmaVirtual) {
        /* Resetting GSP/SEC2 alone does NOT stop a scheduled CE/PBDMA.
           Tear down the RM client and its channels before freeing their
           backing pages. If RM cannot confirm teardown, retain the DMA. */
        if(devExt->GspInitialized&&devExt->GspClientHandle) {
            ULONG release[4]={devExt->GspClientHandle,0,devExt->GspClientHandle,0};
            const ULONG *reply=(const ULONG *)devExt->RpcRxRecord;
            if(!NvxGspCall(devExt,10,release,sizeof(release))||reply[14]<48||
               reply[20]!=release[0]||reply[21]!=0||reply[22]!=release[2]||reply[23]!=0) {
                LogStr("NVX [GSP]: RM teardown unconfirmed; DMA backing retained for channel safety.\n");
                return;
            }
            devExt->GspClientHandle=0;
            LogStr("NVX [GSP]: RM client and channels released before DMA teardown.\n");
        }
        if (bar0) {
            VideoPortWriteRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_ENGINE),1);
            VideoPortWriteRegisterUlong((PULONG)(bar0 + NV_PGSP_FALCON_ENGINE), 1); /* Halt Falcon */
            /* Flush posted resets before releasing DMA-visible storage. */
            (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PSEC_FALCON_ENGINE));
            (void)VideoPortReadRegisterUlong((PULONG)(bar0+NV_PGSP_FALCON_ENGINE));
            VideoPortStallExecution(20);
        }

        VideoPortFreeCommonBuffer(
            HwDeviceExtension,
            devExt->GspDmaLength,
            devExt->GspDmaVirtual,
            devExt->GspDmaPhysical,
            TRUE
        );

        devExt->GspDmaVirtual = NULL;
        devExt->CmdQueueVirtual = devExt->StatQueueVirtual = NULL;
        devExt->GspInitialized = FALSE;
        devExt->GspBootVerified = FALSE;
        devExt->GspCopyVerified = FALSE;
        devExt->GspChannelFaulted = FALSE;
        devExt->GspFbLayoutValid = FALSE;
        devExt->GspScanoutMapped = FALSE;

        if (devExt->DmaAdapter) {
            VideoPortPutDmaAdapter(HwDeviceExtension, devExt->DmaAdapter);
            devExt->DmaAdapter = NULL;
        }

        LogStr("NVX [GSP]: Subsystem halted and buffers released.\n");
        VideoPortDebugPrint(Info, "NVX [GSP]: Subsystem halted and buffers released.\n");
    }
}

