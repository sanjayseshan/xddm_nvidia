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
#include <class/cl0002.h>
#include <class/cl0090.h>
#include <class/clc369.h>
#include <class/clb069.h>
#include <ctrl/ctrlb069.h>
#include <ctrl/ctrl2080/ctrl2080internal.h>
#include <ctrl/ctrl2080/ctrl2080nvd.h>
#include <stddef.h>
#include "bootloader_blob.h"
#include "booter_load_blob.h"

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
    VideoPortZeroMemory(tables,GSP_PAGE_TABLES_SIZE);
    /* PDEs: coherent-system aperture, volatile, next-table address >> 4. */
    tables[0]=((physical+0x1000)>>4)|0xc;
    tables[512]=((physical+0x2000)>>4)|0xc;
    tables[1024]=((physical+0x3000)>>4)|0xc;
    /* PD0 has paired large/small entries; VA>>21 == 128, small is +8. */
    tables[1536+128*2+1]=((physical+0x4000)>>4)|0xc;
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
    ULONG64 scratchPhys,instPhys,userdPhys,methodPhys,fifoGpuVa,scratchEnd;
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
    ULONG64 pushVa,grFenceVa;
    PUCHAR scratch;
    PULONG push;
    volatile ULONG *userd;
    volatile ULONG *grFence;
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
    grBufferInfo=&dev->GrContextInfoParams;
    scratchPhys=(ULONG64)dev->GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET;
    instPhys=scratchPhys+0xa000;
    userdPhys=scratchPhys+0xb400;
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
    chan.gpFifoEntries=256;
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
    chan.ramfcMem.base=instPhys;
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
    LogStr("NVX [GR]: GR submission disabled; no draw tested.\n");
    return FALSE;
#else
    if(!dev->GspGrPromotionAccepted||!dev->GspGrContextAliasVerified||
       dev->GspMmuFaultQueued) {
        LogStr("NVX [GR]: context promotion or backing unverified; no GR submission.\n");
        return FALSE;
    }
#endif
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
    /* This GSP path rejects GET_WORK_SUBMIT_TOKEN. Use the same isolated
       internal runlist doorbell as the earlier probe, and drain notifications
       with a read-only name query instead of the rejected control. */
    if(!NvxGspGetName(dev)) return FALSE;
    assertCountBefore=dev->GspAssertCount;

    userd=(volatile ULONG *)(scratch+0xb400);
    fifo=(volatile ULONG64 *)(scratch+0x9000);
    push=(PULONG)(scratch+0xf000);
    pushVa=dev->GspScratchGpuVa+0xf000;
    grFence=(volatile ULONG *)(scratch+0xb500);
    grFenceVa=dev->GspScratchGpuVa+0xb500;
    /* The GR context VA is promoted, but its external semaphore access remains
       unverified. Use this already-mapped coherent scratch word to isolate the
       target mapping from the report-semaphore method itself. */
    grFence[0]=0;grFence[1]=0x13579bdf;grFence[2]=0x2468ace0;
    if(userd[0x88/4]||userd[0x8c/4]) {
        LogStr("NVX [GR]: fresh graphics USERD is not empty; submission skipped.\n");
        return FALSE;
    }
    VideoPortZeroMemory((PVOID)(scratch+0x9000),0x1000);
    VideoPortZeroMemory((PVOID)(scratch+0xf000),0x1000);
    /* Method A alone asserted in v56. The tested VA is below 4GB, so omit A
       and test B/C/D with the default zero upper address. */
    push[0]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|NVC56F_SET_OBJECT;
    push[1]=classEngineId;
    push[2]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(1U<<16)|NVC797_WAIT_FOR_IDLE;
    push[3]=0;
    push[4]=(NVC56F_DMA_INCR_OPCODE_VALUE<<29)|(3U<<16)|NVC797_SET_REPORT_SEMAPHORE_B;
    push[5]=(ULONG)grFenceVa;
    push[6]=0x47524631;
    push[7]=DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_OPERATION,_RELEASE)|
            DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_RELEASE,_AFTER_ALL_PRECEEDING_WRITES_COMPLETE)|
            DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_PIPELINE_LOCATION,_ALL)|
            DRF_DEF(C797,_SET_REPORT_SEMAPHORE_D,_STRUCTURE_SIZE,_ONE_WORD);
    fifo[0]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|(8ULL<<42);
    __sync_synchronize();
    userd[0x8c/4]=1;
    __sync_synchronize();
    /* USERD is allocated at fixed index 2 above.  On GA104 the scheduler
       exposes that graphics channel in CHRAM slot 2; ring that channel's
       doorbell rather than borrowing channel 1 (the independent CE channel).
       Keep RM's opaque token authoritative whenever it supplies one. */
    token=(dbcfg&0xffff0000U)|2U;
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),token);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x90),2);
    LogStr("NVX [GR]: rang channel doorbell token=");LogHex(token);LogStr(" chid=0x2\n");
    LogStr("NVX [GR]: submitted SET_OBJECT, WAIT_FOR_IDLE and C797 semaphore methods B/C/D.\n");
    for(wait=0;wait<100000;wait++) {
        if(grFence[0]==0x47524631) {
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
    if(userd[0x88/4]==1||grFence[0]==0x47524631) {
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
                  userd[0x88/4]==1&&grFence[0]==0x47524631&&
                  grFence[1]==0x13579bdf&&grFence[2]==0x2468ace0) {
            dev->GspGraphicsCommandVerified=TRUE;
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
    LogStr("NVX [GR]: GR semaphore release completed with guards intact; no drawing tested.\n");
    return TRUE;
}
#else
BOOLEAN NvxGspUnregisterFaultShadow(PNVX_DEVICE_EXTENSION dev)
{
    return !dev||!dev->GspFaultShadowRegistered;
}
#endif

/* Modular components matching OpenRM architecture */
#include "nvx_ce.c"
#include "nvx_disp_rpc.c"
#include "nvx_gsp_boot.c"
