/* User-mode harness for the real GSP bootstrap, with VideoPort/MMIO mocked. */
#include "nvx_gsp.h"
#include "nvx_rpc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ULONG regs[0x1000000/4];
static NVX_DEVICE_EXTENSION dev;
static int scenario, imemTransfers, dmemTransfers, cpuStarts;
static ULONG regoff(PULONG p) { return (ULONG)((PUCHAR)p-(PUCHAR)regs); }
VOID LogStr(const char *s) { (void)s; }
VOID LogHex(unsigned long long x) { (void)x; }
VOID VideoPortDebugPrint(VIDEO_DEBUG_LEVEL level, const CHAR *fmt, ...) { (void)level;(void)fmt; }
VOID VideoPortStallExecution(ULONG us) { (void)us; }
VOID VideoPortZeroMemory(PVOID p,ULONG n) { memset(p,0,n); }
VOID VideoPortMoveMemory(PVOID d,PVOID s,ULONG n) { memcpy(d,s,n); }
PVOID VideoPortGetDmaAdapter(PVOID extension,PVP_DEVICE_DESCRIPTION desc)
{
    (void)extension;
    assert(desc->Dma32BitAddresses && !desc->Dma64BitAddresses);
    return (PVOID)1;
}
VOID VideoPortPutDmaAdapter(PVOID extension,PVOID adapter) { (void)extension;(void)adapter; }
PVOID VideoPortAllocateCommonBuffer(PVOID e,PVOID a,ULONG n,PPHYSICAL_ADDRESS pa,BOOLEAN c,PVOID r)
{
    (void)e;(void)a;(void)c;(void)r;
    pa->QuadPart=0x10000000;
    return calloc(1,n);
}
VOID VideoPortFreeCommonBuffer(PVOID e,ULONG n,PVOID p,PHYSICAL_ADDRESS pa,BOOLEAN c)
{ (void)e;(void)n;(void)pa;(void)c; free(p); }
ULONG VideoPortReadRegisterUlong(PULONG p) {
    if(regoff(p)==0x110040 && dev.CmdQueueVirtual &&
       ((NVX_MSGQ_HEADER *)dev.CmdQueueVirtual)->ReadPtr>=5)
        return 0x80000000;
    return regs[regoff(p)/4];
}
VOID VideoPortWriteRegisterUlong(PULONG p,ULONG value)
{
    ULONG off=regoff(p);
    regs[off/4]=value;
    if(off==0x110c00) {
        NVX_MSGQ_HEADER *q=dev.StatQueueVirtual,*cmd=dev.CmdQueueVirtual;
        ULONG write=q->WritePtr;
        PULONG packet=(PULONG)((PUCHAR)q+4096+write*4096);
        PULONG request=(PULONG)((PUCHAR)cmd+4096+(cmd->WritePtr-1)*4096);
        ULONG alloc[44]={0};
        uint32_t f,r,s;
        assert(cmd->WritePtr>=3&&cmd->WritePtr<=16);
        assert(NvxDecodeRpc((PUCHAR)request,4096,&f,&r,&s));
        assert((f==103||f==76||f==4||f==14)&&s==cmd->WritePtr-1);
        if(f==4) {
            assert(request[23]==0x81&&request[28]==65536&&request[30]==16);
            assert(request[32]==(16U<<16));
            assert(request[34]==((dev.GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET)>>12));
            ULONG assertions=7;
            assert(NvxEncodeRpc((PUCHAR)packet,4096,0x101b,write-1,&assertions,4));
            packet[16]=0;packet[8]^=0xffffffffU;
            write++;packet=(PULONG)((PUCHAR)q+4096+write*4096);
        }
        if(f==103&&request[23]==0xa06c) {
            /* KEPLER_CHANNEL_GROUP_A on Device */
            assert(request[21]==dev.GspDeviceHandle);
            assert(request[22]==0xa06c0001);
            assert(request[28+2]==dev.GspVaSpaceHandle);
        }
        if(f==103&&request[23]==0x9067) {
            /* FERMI_CONTEXT_SHARE_A on TSG */
            assert(request[21]==0xa06c0001);
            assert(request[22]==0x90670001);
            assert(request[28+0]==dev.GspVaSpaceHandle);
        }
        if(f==103&&request[23]==0xc56f) {
            /* Validate AMPERE_CHANNEL_GPFIFO_A descriptors match registered scratch and TSG parent */
            ULONG64 scratchPhys=dev.GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET;
            assert(request[21]==0xa06c0001);          /* hParent == TSG */
            assert(request[22]==0xc56f0001);          /* hObject */
            assert(request[28+5]==0x00200120);        /* flags: PRIVILEGED, PAGE_FIXED, userd_i=1 (ChID=1) */
            assert(request[28+6]==0x90670001);        /* hContextShare == ContextShare */
            assert(request[28+7]==0);                 /* hVASpace == 0 */
            assert(request[28+34]==0);                /* subDeviceId == 0 */
            assert(request[28+36]==(ULONG)(scratchPhys+0x2000));
            assert(request[28+38]==0x1000);
            assert(request[28+40]==1); /* SYSMEM */
            assert(request[28+42]==(ULONG)(scratchPhys+0x3200));
            assert(request[28+44]==0x200);
            assert(request[28+46]==1); /* SYSMEM */
            assert(request[28+48]==(ULONG)(scratchPhys+0x2000));
            assert(request[28+50]==0x200);
            assert(request[28+52]==1); /* SYSMEM */
            assert(request[28+54]==(ULONG)(scratchPhys+0x4000));
            assert(request[28+58]==1); /* SYSMEM */
            assert(request[28+61]==0x15);
        }
        if(f==76&&request[22]==0x801813) {
            ULONG64 physical=dev.GspDmaPhysical.QuadPart+GSP_PAGE_TABLES_OFFSET;
            PULONG64 tables=(PULONG64)((PUCHAR)dev.GspDmaVirtual+GSP_PAGE_TABLES_OFFSET);
            assert(request[26]==(ULONG)physical&&request[28]==4&&request[29]==1);
            assert(tables[0]==((physical+4096)>>4|0xc));
            assert(tables[1536+257]==((physical+0x4000)>>4|0xc));
            assert(tables[2048]==((dev.GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET)>>4|0xd));
            assert(tables[2064]==0);
        }
        memcpy(alloc,request+20,f==76?156:(f==14?56:16));
        if(f==76&&request[22]==0x20800110) memcpy((PUCHAR)alloc+28,"Mock RTX 3070",14);
        if(f==76&&request[22]==0x20802a08) {
            ULONG retSize=0x1000;
            memcpy((PUCHAR)alloc+24,&retSize,sizeof(retSize));
        }
        assert(NvxEncodeRpc((PUCHAR)packet,4096,f,write-1,alloc,f==76?156:(f==14?56:32)));
        packet[16]=0;packet[8]^=0xffffffffU;
        q->WritePtr=write+1;q->ReadPtr=cmd->WritePtr;
    }
    if(off==NV_PGSP_FALCON_BCR && value==0) regs[off/4]=1;
    if((off==NV_PGSP_FALCON_CPUCTL||off==NV_PGSP_FALCON_CPUCTL_ALIAS)&&value==2)
        regs[NV_PGSP_FALCON_CPUCTL/4]=0x10;
    if(off==NV_PSEC_FALCON_DMATRFCMD) {
        ULONG dst=regs[NV_PSEC_FALCON_DMATRFMOFF/4];
        ULONG src=regs[NV_PSEC_FALCON_DMATRFFBOFF/4];
        if(value==0x614) {
            assert(dst==(ULONG)imemTransfers*256);
            assert(src==dst+0x100);
            imemTransfers++;
        } else {
            assert(value==0x600);
            assert(dst==(ULONG)dmemTransfers*256 && src==dst);
            dmemTransfers++;
        }
        if(scenario!=2) regs[off/4]=2;
    }
    if((off==NV_PSEC_FALCON_CPUCTL || off==NV_PSEC_FALCON_CPUCTL_ALIAS) && value==2) {
        assert((scenario==3) == (off==NV_PSEC_FALCON_CPUCTL_ALIAS));
        cpuStarts++;
        {
            NVX_MSGQ_HEADER *cmd=dev.CmdQueueVirtual;
            uint32_t f,r,s;
            assert(cmd->WritePtr==2);
            assert(NvxDecodeRpc((PUCHAR)cmd+4096,4096,&f,&r,&s));
            assert(f==72&&s==0&&r==0xffffffffU);
            assert(NvxDecodeRpc((PUCHAR)cmd+8192,4096,&f,&r,&s));
            assert(f==73&&s==1);
        }
        regs[NV_PSEC_FALCON_CPUCTL/4]=0x10; /* may finish before driver observes RUNNING */
        if(scenario==0 || scenario==3) {
            volatile GSP_FW_WPR_META_V1 *meta=(void *)((PUCHAR)dev.GspDmaVirtual+GSP_WPR_META_OFFSET);
            NVX_MSGQ_HEADER *q=dev.StatQueueVirtual;
            ULONG *entry=(void *)((PUCHAR)q+4096);
            assert(meta->FbSize==0x200000000ULL); /* not BAR1's 256MB */
            assert(meta->BootloaderCodeOffset==0x1800);
            assert(meta->BootloaderDataOffset==0x800);
            assert(meta->GspFwHeapSize>=0x6900000);
            assert(meta->GspFwWprEnd==0x10000000ULL);
            assert(meta->FrtsOffset==0x0ff00000ULL);
            assert(meta->GspFwWprStart<meta->FrtsOffset);
            /* Booter need not write the sysmem metadata back. */
            meta->Verified=0;
            regs[NV_PSEC_FALCON_MAILBOX0/4]=0;
            regs[NV_PGSP_FALCON_RISCV_STAT/4]=0x80;
            q->MsgSize=4096;q->MsgCount=63;q->EntryOff=4096;q->WritePtr=7;
            for (ULONG i=0;i<6;i++) {
                PULONG packet=entry+(i>=3?i+1:i)*1024;
                ULONG assertions=7;
                /* Firmware waits for CPU receive acknowledgement before
                   publishing the mailbox bit requested by this sequencer. */
                ULONG params[19]={32,9,0,0,0,0,0,0,0,0,
                    2,0x110040,0x80000000,0x80000000,0,3,0,0x110044,0};
                ULONG print[3]={0,4,0x12345678};
                ULONG function=i==5?0x1001:(i==4?0x100c:(i==3?0x1002:(i==2?0x101b:72+i)));
                assert(NvxEncodeRpc((PUCHAR)packet,4096,function,i,
                    i==4?print:(i==3?params:(i==2?&assertions:NULL)),i==4?sizeof(print):(i==3?sizeof(params):(i==2?4:0))));
                packet[16]=0;packet[8]^=0xffffffffU;
                if (i==2) {
                    packet[10]=2;packet[14]=5000;packet[8]=0;
                    for (ULONG j=0;j<2048;j++) if (j!=8) packet[8]^=packet[j];
                }
            }
        }
    }
}
int main(void)
{
    for(scenario=0;scenario<4;scenario++) {
        memset(&dev,0,sizeof(dev));memset(regs,0,sizeof(regs));
        imemTransfers=dmemTransfers=cpuStarts=0;
        dev.Bar0MmioVirtual=regs;dev.Bar0Length=sizeof(regs);dev.Bar1Length=0x10000000;
        dev.FwsecFrtsReady=TRUE;
        dev.VerifiedWprEnd=0x10000000ULL;
        dev.VerifiedFrtsOffset=0x0ff00000ULL;
        dev.VerifiedVgaWorkspaceOffset=0x1fffe0000ULL;
        regs[0x1183a4/4]=8192;
        regs[NV_PSEC_FALCON_CPUCTL/4]=(scenario==3) ? 0x50 : 0x10;
        assert(NvxGspInitSubsystem(&dev)==(scenario==0 || scenario==3));
        assert(dev.GspInitialized==(scenario==0 || scenario==3));
        if (dev.GspInitialized) {
            assert(((NVX_MSGQ_HEADER *)dev.CmdQueueVirtual)->ReadPtr==22);
            assert(dev.GspScratchGpuVa==0x10000000);
            assert(dev.GspClientHandle==0xc1d00001);
            assert(dev.GspDeviceHandle==0x80000001&&dev.GspSubdeviceHandle==0x20800001);
            assert(dev.GspVaSpaceHandle==0x90f10001);
            assert(dev.GspScratchMemoryHandle==0x81000001);
            assert(dev.GspChannelGroupHandle==0xa06c0001);
            assert(dev.GspContextShareHandle==0x90670001);
            assert(dev.GspChannelHandle==0xc56f0001);
            assert(dev.GspCopyEngineHandle==0xc6b50001);
            assert(dev.GspAssertCount==7);
        }
        if(scenario==2) { assert(imemTransfers==1 && !cpuStarts); }
        else { assert(imemTransfers==131 && dmemTransfers==98 && cpuStarts==1); }
        if(scenario==0) {
            assert(!NvxGspSequencerCore(&dev,9));
            assert(NvxGspSequencerCore(&dev,5));
            assert(NvxGspSequencerCore(&dev,6));
            assert(NvxGspSequencerCore(&dev,7));
            assert(dev.GspDmaVirtual);
        }
        NvxGspShutdown(&dev);
        assert(!dev.GspDmaVirtual && !dev.DmaAdapter);
    }
    puts("GSP mock tests passed: direct/alias boot, stale HALTED, DMA timeout, offsets and VRAM layout.");
    return 0;
}
