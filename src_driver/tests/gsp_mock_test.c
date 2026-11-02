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
ULONG VideoPortReadRegisterUlong(PULONG p) { return regs[regoff(p)/4]; }
VOID VideoPortWriteRegisterUlong(PULONG p,ULONG value)
{
    ULONG off=regoff(p);
    regs[off/4]=value;
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
            q->MsgSize=4096;q->MsgCount=63;q->EntryOff=4096;q->WritePtr=4;
            for (ULONG i=0;i<4;i++) {
                PULONG packet=entry+i*1024;
                ULONG assertions=7;
                ULONG function=i==3?0x1001:(i==2?0x101b:72+i);
                assert(NvxEncodeRpc((PUCHAR)packet,4096,function,i,
                    i==2?&assertions:NULL,i==2?4:0));
                packet[16]=0;packet[8]^=0xffffffffU;
            }
        }
    }
}
int main(void)
{
    for(scenario=0;scenario<4;scenario++) {
        memset(&dev,0,sizeof(dev));memset(regs,0,sizeof(regs));
        imemTransfers=dmemTransfers=cpuStarts=0;
        dev.Bar0MmioVirtual=regs;dev.Bar1Length=0x10000000;
        dev.FwsecFrtsReady=TRUE;
        dev.VerifiedWprEnd=0x10000000ULL;
        dev.VerifiedFrtsOffset=0x0ff00000ULL;
        dev.VerifiedVgaWorkspaceOffset=0x1fffe0000ULL;
        regs[0x1183a4/4]=8192;
        regs[NV_PSEC_FALCON_CPUCTL/4]=(scenario==3) ? 0x50 : 0x10;
        assert(NvxGspInitSubsystem(&dev)==(scenario==0 || scenario==3));
        assert(dev.GspInitialized==(scenario==0 || scenario==3));
        if (dev.GspInitialized) {
            assert(((NVX_MSGQ_HEADER *)dev.CmdQueueVirtual)->ReadPtr==4);
            assert(dev.GspAssertCount==7);
        }
        if(scenario==2) { assert(imemTransfers==1 && !cpuStarts); }
        else { assert(imemTransfers==131 && dmemTransfers==98 && cpuStarts==1); }
        NvxGspShutdown(&dev);
        assert(!dev.GspDmaVirtual && !dev.DmaAdapter);
    }
    puts("GSP mock tests passed: direct/alias boot, stale HALTED, DMA timeout, offsets and VRAM layout.");
    return 0;
}
