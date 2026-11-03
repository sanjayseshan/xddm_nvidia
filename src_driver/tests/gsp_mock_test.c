/* User-mode harness for the real GSP bootstrap, with VideoPort/MMIO mocked. */
#include "nvx_gsp.h"
#include "nvx_rpc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ULONG regs[0x1000000/4];
static UCHAR gpuMemory[0x10000];
static UCHAR gpuVram[0x4000000];
static NVX_DEVICE_EXTENSION dev;
static int scenario, imemTransfers, dmemTransfers, cpuStarts,freeFailures;
static BOOLEAN dpLinkTrained;
static ULONG regoff(PULONG p) { return (ULONG)((PUCHAR)p-(PUCHAR)regs); }
static void gpuWrite64(ULONG offset,ULONG64 value) {memcpy(gpuMemory+offset,&value,8);}
VOID LogStr(const char *s) {
    if(!strcmp(s,"NVX [SEQ]: validated capacity="))
        assert(((NVX_MSGQ_HEADER *)dev.CmdQueueVirtual)->ReadPtr>=5);
}
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
    pa->QuadPart=0x10004000; /* 4KB aligned, deliberately not 64KB aligned */
    return calloc(1,n);
}
VOID VideoPortFreeCommonBuffer(PVOID e,ULONG n,PVOID p,PHYSICAL_ADDRESS pa,BOOLEAN c)
{ (void)e;(void)n;(void)pa;(void)c; free(p); }
ULONG VideoPortReadRegisterUlong(PULONG p) {
    ULONG off=regoff(p);
    if(off>=0x700000&&off<0x710000) {
        ULONG64 address=((ULONG64)regs[0x1700/4]<<16)+(off-0x700000);
        ULONG value;
        if(address>sizeof(gpuMemory)-4) return 0xffffffff;
        memcpy(&value,gpuMemory+address,4);return value;
    }
    if(regoff(p)==0x110040 && dev.CmdQueueVirtual &&
       ((NVX_MSGQ_HEADER *)dev.CmdQueueVirtual)->ReadPtr>=5)
        return 0x80000000;
    return regs[regoff(p)/4];
}
static PUCHAR dmaPointer(ULONG64 va,ULONG bytes)
{
    if(va>=dev.GspScratchGpuVa && va-dev.GspScratchGpuVa<=GSP_SCRATCH_SIZE &&
       bytes<=GSP_SCRATCH_SIZE-(va-dev.GspScratchGpuVa))
        return (PUCHAR)dev.GspDmaVirtual+GSP_SCRATCH_OFFSET+(va-dev.GspScratchGpuVa);
    if(va>=GSP_UPLOAD_GPU_VA && va-GSP_UPLOAD_GPU_VA<=0x10000 &&
       bytes<=0x10000-(va-GSP_UPLOAD_GPU_VA))
        return (PUCHAR)dev.GspDmaVirtual+GSP_UPLOAD_OFFSET+(va-GSP_UPLOAD_GPU_VA);
    assert(va>=dev.GspVramGpuVa && va-dev.GspVramGpuVa<=sizeof(gpuVram) &&
           bytes<=sizeof(gpuVram)-(va-dev.GspVramGpuVa));
    return gpuVram+(va-dev.GspVramGpuVa);
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
        ULONG alloc[528]={0},replyBytes;
        uint32_t f,r,s;
        assert(cmd->WritePtr>=3&&cmd->WritePtr<=60);
        assert(NvxDecodeRpc((PUCHAR)request,4096,&f,&r,&s));
        assert((f==103||f==76||f==4||f==14||f==10||f==65)&&s==cmd->WritePtr-1);
        if(f==65) {
            UCHAR cfg[2168]={0};
            assert(request[14]==32+2168);
            for(ULONG i=0;i<542;i++) assert(request[20+i]==0);
            ULONG64 bar1=0x1fa000000ULL,bar2=0x1fb000000ULL;
            memcpy(cfg+2088,&bar1,8);memcpy(cfg+2096,&bar2,8);
            ULONG count=scenario==10?17:2;
            ULONG64 base=0,limit=0x0fffffff,reserved=0x10000000;
            memcpy(cfg+840,&count,4);
            memcpy(cfg+848,&base,8);memcpy(cfg+856,&limit,8);memcpy(cfg+864,&reserved,8);
            cfg[848+28]=cfg[848+29]=cfg[848+30]=1;
            base=0x180000;limit=scenario==11?0xfff:0x3ddffffULL;reserved=0;
            memcpy(cfg+896,&base,8);memcpy(cfg+904,&limit,8);memcpy(cfg+912,&reserved,8);
            cfg[896+28]=cfg[896+29]=1;
            assert(NvxEncodeRpc((PUCHAR)packet,4096,f,write-1,cfg,scenario==9?840:sizeof(cfg)));
            packet[16]=0;packet[8]^=0xffffffffU;
            q->WritePtr=write+1;q->ReadPtr=cmd->WritePtr;
            return;
        }
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
            assert(request[28+3]==11); /* dedicated COPY2 TSG */
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
            assert(request[28+32]==11);                /* dedicated COPY2 channel */
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
        if(f==103&&request[23]==0xc7b5) {
            assert(request[21]==dev.GspChannelHandle);
            assert(request[25]==8&&request[28]==1&&request[29]==11);
        }
        assert(f!=103||request[23]!=0xc6b5); /* GA100 class is invalid on GA104. */
        if(f==76&&request[22]==0x801813) {
            ULONG64 physical=dev.GspDmaPhysical.QuadPart+GSP_PAGE_TABLES_OFFSET;
            PULONG64 tables=(PULONG64)((PUCHAR)dev.GspDmaVirtual+GSP_PAGE_TABLES_OFFSET);
            assert(request[26]==(ULONG)physical&&request[28]==4&&request[29]==1);
            assert(tables[0]==((physical+4096)>>4|0xc));
            assert(tables[1536+257]==((physical+0x4000)>>4|0xc));
            assert(tables[2048]==((dev.GspDmaPhysical.QuadPart+GSP_SCRATCH_OFFSET)>>4|0xd));
            assert(tables[2064]==0);
            assert(tables[1025]==((physical+0x5000)>>4|0xc));
            assert(tables[2560]==((physical+0x6000)>>4|0xc));
            assert(tables[3072]==0); /* Reserved low VRAM is deliberately unmapped. */
            assert(tables[4095]==0);
            assert(tables[3072+32]==(dev.GspFbLayoutValid?((0x200000ULL>>4)|1):0));
            assert(tables[2560+16*2]==((physical+0x7000)>>4|0xc));
            assert(tables[3583]==(dev.GspFbLayoutValid?(((ULONG64)0x1ff0000>>4)|1):0));
            assert(tables[2560+32*2]==0);
            assert(tables[2560+32*2+1]==((physical+0x8000)>>4|0xc));
            {
                PULONG64 upload=(PULONG64)((PUCHAR)tables+0x8000);
                assert(upload[0]==((dev.GspDmaPhysical.QuadPart+GSP_UPLOAD_OFFSET)>>4|0xd));
                assert(upload[15]==((dev.GspDmaPhysical.QuadPart+GSP_UPLOAD_OFFSET+0xf000)>>4|0xd));
                assert(upload[16]==0);
                /* Decoding the first 4KB leaf must preserve the DMA base's
                   low 64KB offset; a large leaf would lose these bits. */
                assert(((upload[0]&~0xffULL)<<4)==dev.GspDmaPhysical.QuadPart+GSP_UPLOAD_OFFSET);
            }
        }
        if(f==76&&request[22]==0xa06f0107&&scenario==4) {
            ULONG rc[5]={11,1,0x1f,0,0};
            assert(NvxEncodeRpc((PUCHAR)packet,4096,0x1004,write-1,rc,sizeof(rc)));
            packet[16]=0;packet[8]^=0xffffffffU;
            write++;packet=(PULONG)((PUCHAR)q+4096+write*4096);
        }
        if(f==76 && request[22]==NVX_CTRL_CMD_DP_GET_CAPS && scenario==12) {
            ULONG error[67]={0x1a,0,3};
            memcpy((PUCHAR)error+12,"mock display diagnostic",24);
            assert(NvxEncodeRpc((PUCHAR)packet,4096,0x1006,write-1,error,sizeof(error)));
            packet[16]=0;packet[8]^=0xffffffffU;
            write++;packet=(PULONG)((PUCHAR)q+4096+write*4096);
        }
        replyBytes=f==76?24+request[24]:(f==14?56:32);
        assert(replyBytes<=sizeof(alloc));
        memcpy(alloc,request+20,f==76?replyBytes:(f==14?56:16));
        if(f==76&&request[22]==0x73028b) {
            assert(request[24]==56 && (request[27]==1 || request[27]==4));
            alloc[8]=request[27]==1?1:(scenario==12?0xffffffffU:2);
            alloc[9]=2;alloc[10]=request[27]==1?0:8;
        }
        if(f==76&&request[22]==NVX_CTRL_CMD_DFP_ASSIGN_SOR) {
            assert(request[21]==0x73000001 && request[24]==80);
            assert(request[27]==4); /* Inactive connected output. */
            assert((request[28]&2)!=0); /* Keep active SOR 1 away from it. */
            assert(request[29]==0 && request[30]==0 && request[31]==0);
            if(scenario==12) { alloc[22]=4;alloc[23]=NVX_DFP_SOR_TYPE_SINGLE; }
        }
        if(f==76&&request[22]==NVX_CTRL_CMD_DP_GET_CAPS) {
            assert(scenario==12 && request[24]==56 && request[27]==3);
            alloc[8]=3;alloc[9]=3; /* HBR2, DP 1.2 + 1.4 */
        }
        if(f==76&&request[22]==NVX_CTRL_CMD_DP_CTRL) {
            assert(scenario==12 && request[24]==28 && request[27]==4);
            assert(request[28]==(NVX_DP_CTRL_SET_LANE_COUNT|
                                 NVX_DP_CTRL_SET_LINK_BW|
                                 NVX_DP_CTRL_SET_ENHANCED_FRAMING));
            assert(request[29]==(NVX_DP_CTRL_DATA_LANE_COUNT_4|
                                 NVX_DP_CTRL_DATA_LINK_BW_HBR2|
                                 NVX_DP_CTRL_DATA_ENHANCED_FRAMING));
            dpLinkTrained=TRUE;
        }
        if(f==76&&request[22]==NVX_CTRL_CMD_DP_AUXCH_CTRL) {
            assert(scenario==12 && request[24]==48 && request[27]==4);
            if(request[29]==9 && request[30]==0 && request[35]==3) {
                alloc[11]=0x00841414; /* DPCD 1.4, HBR2, 4 lanes, no downspread. */
                alloc[15]=4;alloc[16]=NVX_DP_AUX_REPLY_ACK;
            } else if(request[29]==8 && request[30]==0x600 && request[35]==0) {
                alloc[15]=1;alloc[16]=NVX_DP_AUX_REPLY_ACK;
            } else {
                assert(0 && "Unexpected AUX request");
            }
        }
        if(f==76&&request[22]==NVX_CTRL_CMD_DP_MAIN_LINK_CTRL) {
            assert(scenario==12 && request[24]==12 && request[27]==4);
            assert(request[28]==NVX_DP_MAIN_LINK_CTRL_POWER_STATE_POWERUP);
        }
        if(f==76&&request[22]==NVX_CTRL_CMD_DP_GET_LINK_CONFIG) {
            assert(scenario==12 && request[24]==16 && request[27]==4);
            if(dpLinkTrained) { alloc[8]=4;alloc[9]=0x14; }
            else { alloc[8]=0;alloc[9]=6; }
        }
        if(f==76&&request[22]==0x50700709) {
            assert(request[21]==0xc6700001 && request[24]==8);alloc[7]=3;
        }
        if(f==76&&request[22]==0x20800a01) {
            assert(request[21]==dev.GspSubdeviceHandle && request[24]==28);
            alloc[7]=255;alloc[9]=4;
        }
        if(f==76&&request[22]==0x730102) alloc[8]=4;
        if(f==76&&request[22]==0x730287) alloc[7]=15;
        if(f==76&&request[22]==0x730126) {
            assert(request[27]<4);alloc[9]=request[27]==0?1:0;
        }
        if(f==76&&request[22]==0x730125) {
            assert(request[27]==5 && request[28]==1 && request[29]==0);
            alloc[10]=0x10;
        }
        if(f==76&&request[22]==0x730245) {
            UCHAR *edid=(UCHAR *)(alloc+10);
            assert(request[24]==2064 && request[28]==2048);
            assert(request[27]==1 || request[27]==4);
            alloc[8]=128;
            for(ULONG j=1;j<7;j++) edid[j]=255;
            edid[18]=1;edid[19]=4;
            ULONG sum=0;
            for(ULONG j=0;j<127;j++) sum+=edid[j];
            edid[127]=(UCHAR)(0-sum);
            if(request[27]==4 && scenario==3) edid[127]^=1;
            if(request[27]==4 && scenario==7) alloc[8]=2049;
        }
        if(f==76&&request[22]==0x730120) {
            assert(request[21]==0x73000001 && request[24]==12);
            alloc[7]=5; alloc[8]=5; /* two supported physical connectors */
        }
        if(f==76&&request[22]==0x730122) {
            assert(request[21]==0x73000001 && request[24]==16 && request[28]==5);
            alloc[8]=5; /* two attached monitors, no scanout creation */
        }
        if(f==76&&request[22]==0x20800110) memcpy((PUCHAR)alloc+28,"Mock RTX 3070",14);
        if(f==76&&request[22]==0x20802a08) {
            ULONG retSize=0x5000; /* Actual GA104 v90 firmware result. */
            memcpy((PUCHAR)alloc+24,&retSize,sizeof(retSize));
        }
        if(f==76&&request[22]==0x20802a07) {
            assert(request[26]>=9&&request[26]<=11&&request[24]==8);
            alloc[7]=request[26]<11?0x0321:0x032c;
        }
        if(f==76&&request[22]==0xc36f0108) alloc[6]=0; /* 535 GSP returns zero external token. */
        if(f==76&&request[22]==0x20800179) {
            assert(request[24]==496&&request[26]==11);
            alloc[6+62]=scenario==8?0xffffffffU:0x220400;
        }
        if(f==76 && request[22]==0x20800a49) {
            assert(request[24]==24 && request[26]==0x1ff0000 && request[28]==0x10000);
            assert(request[30]==2 && request[31]==2);
            for(ULONG i=0;i<0x10000;i++) assert(gpuVram[0x1ff0000+i]==0);
            if(scenario==3) alloc[3]=0x40; /* reject instance registration */
        }
        if(f==76 && request[22]==0x20800a58) {
            assert(request[24]==40 && request[26]==1 && request[32]==1);
            assert(request[28]==(ULONG)(dev.GspDmaPhysical.QuadPart+GSP_DISPLAY_PUSH_OFFSET+
                (request[33]==0xc67d?0:(request[34]==0?1:2))*0x1000));
            assert(request[30]==0xfff && request[35]==1);
            assert(request[33]==0xc67d || request[33]==0xc67e);
        }
        if(f==103 && (request[23]==0xc67d || request[23]==0xc67e)) {
            assert(request[21]==dev.NativeDisplayHandle && request[25]==32);
            assert(request[28]==(request[22]==0xc6700102?2:0));
            for(ULONG i=29;i<36;i++) assert(request[i]==0);
        }
        if(f==10) {
            assert(request[20]==dev.GspClientHandle);
            if(request[21]==0) {
                assert(request[22]==dev.GspClientHandle);
                if(freeFailures) {alloc[3]=0x40;freeFailures--;}
            } else {
                assert(request[21]==dev.NativeDisplayHandle);
                assert(request[22]>=0xc6700100 && request[22]<=0xc6700102);
            }
        }
        assert(NvxEncodeRpc((PUCHAR)packet,4096,f,write-1,alloc,replyBytes));
        packet[16]=0;packet[8]^=0xffffffffU;
        q->WritePtr=write+1;q->ReadPtr=cmd->WritePtr;
    }
    if(off==0x680000 || off==0x690000 || off==0x692000) {
        assert(!(value&3));value>>=2;
        ULONG channel=off==0x680000?0:(off==0x690000?1:2);
        PULONG buffer=(PULONG)((PUCHAR)dev.GspDmaVirtual+GSP_DISPLAY_PUSH_OFFSET+channel*0x1000);
        assert(value==4);
        for(ULONG i=0;i<4;i++) assert(buffer[i]==0);
        if(scenario!=7) regs[off/4+1]=16; /* stall only the display channel */
    }
    if(off==0x220490 && dev.GspGPPut>1) {
        PUCHAR scratch=(PUCHAR)dev.GspDmaVirtual+GSP_SCRATCH_OFFSET;
        ULONG previous=(dev.GspGPPut+1023)%1024;
        PULONG push=(PULONG)(scratch+0x9000+(previous%16)*0x100);
        if(push[2]==0x20048100) {
            assert(push[11]==1 && push[8]==push[9] && push[9]==push[10]);
            memcpy(dmaPointer(((ULONG64)push[5]<<32)|push[6],push[10]),
                   dmaPointer(((ULONG64)push[3]<<32)|push[4],push[10]),push[10]);
            *(PULONG)(scratch+0xc000)=push[17];
        } else {
            assert(push[2]==0x200281c1 && push[4]==0x00030005);
            PULONG dst=(PULONG)dmaPointer(((ULONG64)push[6]<<32)|push[7],push[9]*4);
            for(ULONG i=0;i<push[9];i++) dst[i]=push[3];
            *(PULONG)(scratch+0xc000)=push[15];
        }
        *(PULONG)(scratch+0x3288)=dev.GspGPPut;
        return;
    }
    if(off==0x220490) {
        PUCHAR scratch=(PUCHAR)dev.GspDmaVirtual+GSP_SCRATCH_OFFSET;
        PULONG push=(PULONG)(scratch+0x9000),userd=(PULONG)(scratch+0x3200);
        ULONG64 entry=*(PULONG64)scratch;
        ULONG64 address=(entry&0xfffffffcULL)|((entry>>32&0xffULL)<<32);
        assert(value==1&&regs[0x30090/4]==0x40001&&userd[0x8c/4]==1);
        assert(address==dev.GspScratchGpuVa+0x9000&&(entry>>42)==68);
        assert(push[0]==0x20018000&&push[1]==0xc7b5);
        assert(push[2]==0x20048100&&push[3]==0&&push[4]==0x21000000);
        assert(push[5]==0&&push[6]==0x1000d000);
        assert(push[14]==0x20048100&&push[16]==0x1000a000&&push[17]==0&&push[18]==0x21000000);
        assert(push[26]==0x20048100&&push[28]==0x21000000&&push[29]==0&&push[30]==0x1000b000);
        assert(push[38]==0x20048100&&push[40]==0x1000d000&&push[41]==0&&push[42]==0x21000000);
        assert(push[50]==0x20048100&&push[52]==0x21000000&&push[53]==0&&push[54]==0x1000e000);
        assert(push[62]==0x20038090&&push[64]==0x1000c000);
        assert(push[66]==0x200180c0&&push[67]==0x0400000c);
        if(scenario!=4) { /* timeout: GPU never writes completion */
            memcpy(scratch+0xd000,gpuVram+0x1000000,256);
            memcpy(gpuVram+0x1000000,scratch+0xa000,256);
            memcpy(scratch+0xb000,gpuVram+0x1000000,256);
            memcpy(gpuVram+0x1000000,scratch+0xd000,256);
            memcpy(scratch+0xe000,gpuVram+0x1000000,256);
            *(PULONG)(scratch+0xc000)=push[65];
            userd[0x88/4]=1;
            if(scenario==5) *(PULONG)(scratch+0xb000)^=1; /* stale/corrupt data */
            if(scenario==6) *(PULONG)(scratch+0xb100)^=1; /* guard corruption */
        }
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
        if(scenario==0 || scenario>=3) {
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
static void test_reusable_queue(void) {
    /* Test NvxGspSubmitCopy and NvxGspSubmitFill */
    NVX_DEVICE_EXTENSION dev_q;
    UCHAR *dma = malloc(0x3000000);
    memset(&dev_q, 0, sizeof(dev_q));
    memset(dma, 0, 0x3000000);
    dev_q.GspDmaVirtual = dma;
    dev_q.GspInitialized = TRUE;
    dev_q.GspCopyVerified = TRUE;
    dev_q.GspScratchGpuVa = 0x10000000;
    dev_q.GspRunlistBase = 0xc00400;
    dev_q.GspSubmitToken = 0x10001;
    dev_q.GspGPPut = 1;
    dev_q.GspNextFence = 0x100;
    dev_q.Bar0MmioVirtual = regs;
    dev_q.Bar0Length = sizeof(regs);

    PUCHAR scratch = dma + GSP_SCRATCH_OFFSET;
    volatile ULONG *fence = (volatile ULONG *)(scratch + 0xc000);
    fence[0] = 0x100; /* match expected fence */

    assert(NvxGspSubmitCopy(&dev_q, 0x21000000, 0x1000a000, 256));
    assert(dev_q.GspGPPut == 2);
    assert(dev_q.GspNextFence == 0x101);

    fence[0] = 0x101;
    assert(NvxGspSubmitFill(&dev_q, 0x21000000, 0xff00ff00, 256));
    assert(dev_q.GspGPPut == 3);
    assert(dev_q.GspNextFence == 0x102);

    /* Timeout scenario */
    fence[0] = 0x0;
    assert(!NvxGspSubmitCopy(&dev_q, 0x21000000, 0x1000a000, 256));
    assert(dev_q.GspChannelFaulted);
    free(dma);
}

int main(void)
{
    for(scenario=0;scenario<13;scenario++) {
        memset(&dev,0,sizeof(dev));memset(regs,0,sizeof(regs));
        imemTransfers=dmemTransfers=cpuStarts=0;
        freeFailures=scenario==7?1:0;
        dev.Bar0MmioVirtual=regs;dev.Bar0Length=sizeof(regs);dev.Bar1Length=0x10000000;
        dev.FwsecFrtsReady=TRUE;
        dev.VerifiedWprEnd=0x10000000ULL;
        dev.VerifiedFrtsOffset=0x0ff00000ULL;
        dev.VerifiedVgaWorkspaceOffset=0x1fffe0000ULL;
        regs[0x1183a4/4]=8192;
        regs[0x220408/4]=0x40000; /* doorbell selector 4 */
        regs[0x220404/4]=0x80000b;
        regs[0x220700/4]=0x80000000;
        regs[NV_PSEC_FALCON_CPUCTL/4]=(scenario==3) ? 0x50 : 0x10;
        assert(NvxGspInitSubsystem(&dev)==(scenario==0 || scenario>=3));
        assert(dev.GspInitialized==(scenario==0 || scenario>=3));
        if (dev.GspInitialized) {
            assert(((NVX_MSGQ_HEADER *)dev.CmdQueueVirtual)->ReadPtr==
                   (ULONG)((scenario==4?30:28)+(dev.GspCopyVerified?15:0)+dev.MonitorCount+
                           (scenario==12?5:0)));
            if(dev.GspCopyVerified) {
                assert(dev.GspDisplayHandle==0x73000001);
                assert(dev.NativeDisplayHandle==0xc6700001 && dev.NativeDisplayCaps==3);
                assert(dev.DisplayWindowMask==255);
                assert(dev.DisplayHeadCount==4 && dev.DisplayHeadMask==15);
                assert(dev.DisplayActiveIds[0]==1 && dev.DisplayActiveIds[1]==0);
                assert(dev.DisplayRoutableMask==5 && dev.DisplayHeadRoutingMap==0x10);
                assert(dev.GspSupportedDisplays==5 && dev.GspConnectedDisplays==5);
                assert(dev.MonitorCount==(ULONG)(scenario==0 || scenario==12?2:1));
                assert(dev.MonitorIds[0]==1 && dev.MonitorEdidBytes[0]==128);
                if(scenario==0) assert(dev.MonitorIds[1]==4 && dev.MonitorEdid[1][19]==4);
                if(scenario==12)
                    assert(dev.MonitorSor[1]==3 && !dpLinkTrained &&
                           dev.SecondaryDpTrainReady && dev.SecondaryDpDisplayId==4);
            }
            if(dev.GspCopyVerified) {
                memset(gpuVram+0x1ff0000,0xa5,0x10000);
                assert(NvxGspInitDisplayChannels(&dev)==(scenario==0 || scenario==12));
                assert(dev.NativeDisplayChannelsReady==(scenario==0 || scenario==12));
                assert(dev.NativeDisplayInstance==0x1ff0000);
                assert(!dev.NativeDisplayNotifierReady);
                if(scenario==0 || scenario==12) {
                    assert(dev.MonitorSor[0]==1 && dev.MonitorProtocol[0]==0);
                    assert(dev.NativeDisplayChannels[0]==0xc6700100);
                    assert(dev.NativeDisplayChannels[1]==0xc6700101);
                    assert(dev.NativeDisplayChannels[2]==0xc6700102);
                } else for(ULONG i=0;i<3;i++) assert(!dev.NativeDisplayChannels[i]);
                if(scenario==12) {
                    assert(!dpLinkTrained && !dev.SecondaryDpTrainingAttempted);
                    NvxGspTrainSecondaryDpLink(&dev);
                    assert(dpLinkTrained && dev.SecondaryDpTrainingAttempted);
                }
                ULONG sequence=dev.RpcTxSequence;
                assert(NvxGspInitDisplayChannels(&dev)==(scenario==0 || scenario==12));
                assert(dev.RpcTxSequence==sequence); /* never allocate twice */
            }
            assert(dev.GspScratchGpuVa==0x10000000);
            assert(dev.GspClientHandle==0xc1d00001);
            assert(dev.GspDeviceHandle==0x80000001&&dev.GspSubdeviceHandle==0x20800001);
            assert(dev.GspVaSpaceHandle==0x90f10001);
            assert(dev.GspScratchMemoryHandle==0x81000001);
            assert(dev.GspChannelGroupHandle==0xa06c0001);
            assert(dev.GspContextShareHandle==0x90670001);
            assert(dev.GspChannelHandle==0xc56f0001);
            assert(dev.GspCopyEngineHandle==0xc7b50001);
            assert(dev.GspCopyEngineType==11);
            assert(dev.GspFbLayoutValid==((scenario<9 || scenario==12)));
            if(dev.GspFbLayoutValid) {
                assert(dev.GspFbRegionCount==2&&dev.GspFbFreeBase==0x180000&&dev.GspFbFreeLimit==0x3ddffffULL);
                assert(dev.GspBar1PageDirectory==0x1fa000000ULL&&dev.GspBar2PageDirectory==0x1fb000000ULL);
            }
            else assert(dev.GspFbFreeBase==0&&dev.GspFbFreeLimit==0);
            assert(dev.GspCopyVerified==(scenario==0||scenario==3||scenario==7||scenario==12));
            assert(dev.GspAssertCount==7);
            assert(dev.GspChannelFaulted==(scenario==4));
            if(scenario==0) {
                UCHAR pixels[512];
                for(ULONG i=0;i<sizeof(pixels);i++) pixels[i]=(UCHAR)(i*17+3);
                assert(NvxGspUpload(&dev,dev.GspVramGpuVa+0x200000,pixels,sizeof(pixels)));
                assert(!memcmp(gpuVram+0x200000,pixels,sizeof(pixels)));
                assert(NvxGspUpload(&dev,dev.GspVramGpuVa+0x2000000,pixels,sizeof(pixels)));
                assert(!memcmp(gpuVram+0x2000000,pixels,sizeof(pixels)));
                assert(!NvxGspUpload(&dev,dev.GspVramGpuVa,pixels,sizeof(pixels)));
                assert(!NvxGspUpload(&dev,dev.GspVramGpuVa+0x3de0000,pixels,sizeof(pixels)));
            }
            if(scenario==0) {
                dev.PhysicalBar1.QuadPart=0xe0000000;
                dev.NumModes=1;dev.ModeFramebuffer[0]=dev.PhysicalBar1;
                dev.Modes[0].ScreenStride=2560;dev.Modes[0].VisScreenHeight=480;
                memset(gpuMemory,0,sizeof(gpuMemory));
                regs[0x1700/4]=0x1234;regs[0x1704/4]=0x80000001;
                gpuWrite64(0x1200,0x2c00); /* active VER2 PDB, 64KB big pages */
                gpuWrite64(0x2000,(0x3000>>4)|2);
                gpuWrite64(0x3000,(0x4000>>4)|2);
                gpuWrite64(0x4000,(0x5000>>4)|2);
                gpuWrite64(0x5000,(0x20000000>>4)|1);
                assert(NvxGspDiscoverScanout(&dev,0));
                assert(dev.GspScanoutMapped&&dev.GspScanoutPhysical==0x20000000&&dev.GspScanoutBytes==1228800);
                assert(regs[0x1700/4]==0x1234); /* Every PRAMIN access restores the window. */
                gpuWrite64(0x1200,0x2c02); /* reject system-memory root */
                assert(!NvxGspDiscoverScanout(&dev,0)&&!dev.GspScanoutMapped);
                assert(regs[0x1700/4]==0x1234);
                gpuWrite64(0x1200,0x2c00);gpuWrite64(0x5000,(0x20000000>>4)|65);
                assert(!NvxGspDiscoverScanout(&dev,0)); /* read-only scanout */
                regs[0x1704/4]=0x90000001;
                assert(!NvxGspDiscoverScanout(&dev,0));
            }
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
        if(scenario==7) {
            assert(dev.GspDmaVirtual&&dev.GspInitialized); /* teardown failure must retain DMA */
            NvxGspShutdown(&dev);
        }
        assert(!dev.GspDmaVirtual && !dev.DmaAdapter);
        assert(!dev.GspCopyVerified);
    }
    test_reusable_queue();
    puts("GSP mock tests passed: boot, GA104 CE, copy submission/completion, timeout, corrupt data/guards, reusable queue and DMA teardown retention.");
    return 0;
}
