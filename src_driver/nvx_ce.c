/*
 * NVIDIA Ampere GA104 Copy Engine & Channel Subsystem
 */

static BOOLEAN NvxGspCopySmokeTest(PNVX_DEVICE_EXTENSION dev)
{
    /* C56F/C7B5 535 ABI. Scratch layout: GPFIFO 0..1fff, instance 2000,
       USERD 3200, CE fault buffer 4000..8fff, init push 9000, source a000,
       destination b000, completion c000. No scanout/VRAM is touched. */
    PUCHAR scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    volatile ULONG *userd=(volatile ULONG *)(scratch+0x3200);
    volatile ULONG *dst=(volatile ULONG *)(scratch+0xb000);
    volatile ULONG *fence=(volatile ULONG *)(scratch+0xc000);
    volatile ULONG *backup=(volatile ULONG *)(scratch+0xf000);
    volatile ULONG *restored=(volatile ULONG *)(scratch+0xe000);
    PULONG src=(PULONG)(scratch+0xa000),push=(PULONG)(scratch+0x9000);
    volatile ULONG64 *fifo=(volatile ULONG64 *)scratch;
    ULONG token=0,n=0,runlistBase=0,runlistQuery[124]={0}; /* two arrays of 62 engines */
    ULONG64 va=dev->GspScratchGpuVa,pushVa=va+0x9000;
    ULONG64 vramVa=dev->GspVramGpuVa+0x1000000ULL; /* 16MB, inside free ISO region */
    const ULONG done=0x91c0ffee,guard=0xf17e55aa;
    dev->GspCopyVerified=FALSE;
    dev->GspChannelFaulted=FALSE;
    if(dev->Bar0Length<0x94||!dev->GspCopyEngineHandle||!va) return FALSE;
    if(!NvxGspControl(dev,dev->GspChannelHandle,NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,&token,sizeof(token))) {
        LogStr("NVX [COPY]: work-submit token unavailable; no commands submitted.\n");
        return FALSE;
    }
    token=((const ULONG *)dev->RpcRxRecord)[26];
    LogStr("NVX [COPY]: RM work-submit token=");LogHex(token);LogStr("\n");
    /* GSP-RM returns zero for the external work-submit token. Do not invent a
       non-GSP token from the runlist selector; use the GSP internal doorbell. */
    runlistQuery[0]=dev->GspCopyEngineType;
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_GPU_GET_ENGINE_RUNLIST_PRI_BASE,runlistQuery,sizeof(runlistQuery))) {
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
        LogStr(" runlistId=");LogHex((dbcfg>>16)&0xffffU);
        LogStr(" workToken=");LogHex(token);LogStr(" chid=0x1\n");
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
    NVX_APPEND_COPY(vramVa,va+0xf000);
    NVX_APPEND_COPY(va+0xa000,vramVa);
    NVX_APPEND_COPY(vramVa,va+0xb000);
    NVX_APPEND_COPY(va+0xf000,vramVa);
    NVX_APPEND_COPY(vramVa,va+0xe000);
#undef NVX_APPEND_COPY
    push[n++]=0x20038090; /* SET_SEMAPHORE_A/B/PAYLOAD */
    push[n++]=(ULONG)((va+0xc000)>>32);push[n++]=(ULONG)(va+0xc000);push[n++]=done;
    push[n++]=0x200180c0;push[n++]=0x0400000c; /* flushed one-word semaphore release */
    fifo[0]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();userd[0x8c/4]=1;__sync_synchronize();
    /* Ring both the Ampere usermode doorbell (0x30090) and the runlist internal doorbell (runlistBase+0x90). */
    if(dev->Bar0Length>=0x30094) {
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),token);
    }
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x90),1);
    LogStr("NVX [COPY]: submitted guarded VRAM write/read/restore roundtrip and completion release.\n");
    /* Cold bare-metal GSP/channel startup can take longer than the VM path.
       Give the asynchronous copy engine ten seconds before rejecting native
       scanout, and emit one-second progress snapshots for the next log. */
    for(ULONG wait=0;wait<100000;wait++) {
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
        if(wait && (wait%10000)==0) {
            LogStr("NVX [COPY]: still waiting; seconds=");LogHex(wait/10000);
            LogStr(" GPGet=");LogHex(userd[0x88/4]);
            LogStr(" GPPut=");LogHex(userd[0x8c/4]);
            LogStr(" fence=");LogHex(fence[0]);LogStr("\n");
        }
        VideoPortStallExecution(100);
    }
    LogStr("NVX [COPY]: completion timeout; GPGet=");LogHex(userd[0x88/4]);
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr(" fence=");LogHex(fence[0]);LogStr("\n");
    {
        PULONG ramfc=(PULONG)(scratch+0x2000);
        ULONG fault[26]={0};
        ULONG chramBase=0;
        ULONG chcfg=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+4));
        chramBase=chcfg&0xfffffff0U;
        for(ULONG i=0;i<3;i++) {
            ULONG current=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x200+i*0x40));
            ULONG inst=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x208+i*0x40));
            ULONG instHi=VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+runlistBase+0x20c+i*0x40));
            LogStr("NVX [COPY]: runlist-engine=");LogHex(i);
            LogStr(" channel-status=");LogHex(current);
            LogStr(" inst-lo=");LogHex(inst);LogStr(" inst-hi=");LogHex(instHi);LogStr("\n");
        }
        if(chramBase&&chramBase+8<=dev->Bar0Length) {
            LogStr("NVX [COPY]: CHRAM after timeout slot0=");
            LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chramBase)));
            LogStr(" slot1=");
            LogHex(VideoPortReadRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+chramBase+4)));
            LogStr("\n");
        }
        LogStr("NVX [COPY]: RAMFC userdLo=");LogHex(ramfc[0x8/4]);
        LogStr(" userdHi=");LogHex(ramfc[0xc/4]);LogStr(" gpBaseLo=");LogHex(ramfc[0x48/4]);
        LogStr(" gpBaseHi=");LogHex(ramfc[0x4c/4]);LogStr(" chid=");LogHex(ramfc[0xe8/4]);LogStr("\n");
        VideoPortZeroMemory(fault,sizeof(fault));
        if(NvxGspControl(dev,dev->GspChannelHandle,NVA06F_CTRL_CMD_GET_MMU_FAULT_INFO,fault,sizeof(fault))) {
            const NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS *reply=
                (const NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS *)(dev->RpcRxRecord+104);
            LogStr("NVX [COPY]: MMU fault addrHi=");LogHex(reply->addrHi);
            LogStr(" addrLo=");LogHex(reply->addrLo);
            LogStr(" type=");LogHex(reply->faultType);LogStr("\n");
        } else LogStr("NVX [COPY]: MMU fault-info control unavailable.\n");
        {
            NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS channelState={0};
            if(NvxGspControl(dev,dev->GspChannelHandle,
                             NVB06F_CTRL_CMD_GET_CHANNEL_HW_STATE,
                             &channelState,sizeof(channelState))) {
                const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *reply=
                    (const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *)(dev->RpcRxRecord+104);
                LogStr("NVX [COPY]: channel hardware state=");
                LogHex(reply->state);LogStr("\n");
            } else LogStr("NVX [COPY]: channel hardware state unavailable.\n");
        }
    }
    return FALSE;
}

/* The two display adapters share one CE queue, fence and push-buffer ring. */
static volatile ULONG NvxGspEngineBusy=0;

static BOOLEAN NvxGspLockUserScratch(void)
{
    ULONG wait;
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) return TRUE;
        VideoPortStallExecution(10);
    }
    return FALSE;
}

static BOOLEAN NvxGspLockEngine(void)
{
    ULONG wait;
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGspEngineBusy,1)) return TRUE;
        VideoPortStallExecution(10);
    }
    return FALSE;
}

static VOID NvxGspUnlockEngine(VOID)
{
    __sync_lock_release(&NvxGspEngineBusy);
}

static BOOLEAN NvxGspSubmitCopyUnlocked(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG64 srcVa,
                                        ULONG srcPitch,ULONG dstPitch,
                                        ULONG bytesPerLine,ULONG lineCount)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    PUCHAR scratch;
    volatile ULONG *userd;
    volatile ULONG *fence;
    PULONG push;
    volatile ULONG64 *fifo;
    ULONG slot,n=0,fenceVal;
    ULONG64 srcSpan,dstSpan;
    ULONG64 va,pushVa;

    if(!dev||!dev->GspInitialized||!dev->GspCopyVerified||dev->GspChannelFaulted) return FALSE;
    if(!bytesPerLine||bytesPerLine>0x1000000||!lineCount||
       srcPitch<bytesPerLine||dstPitch<bytesPerLine||
       (bytesPerLine&3)||(srcPitch&3)||(dstPitch&3)) return FALSE;
    srcSpan=(ULONG64)(lineCount-1)*srcPitch+bytesPerLine;
    dstSpan=(ULONG64)(lineCount-1)*dstPitch+bytesPerLine;
    if(srcSpan>0x1000000||dstSpan>0x1000000) return FALSE;

    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    userd=(volatile ULONG *)(scratch+0x3200);
    fence=(volatile ULONG *)(scratch+0xc000);
    fifo=(volatile ULONG64 *)scratch;
    va=dev->GspScratchGpuVa;

    slot=dev->GspGPPut%16;
    push=(PULONG)(scratch+0xd000+slot*0x100);
    pushVa=va+0xd000+slot*0x100;
    fenceVal=dev->GspNextFence++;
    if(!fenceVal) fenceVal=dev->GspNextFence++;

    push[n++]=0x20018000;push[n++]=0xc7b5; /* SET_OBJECT AMPERE_DMA_COPY_B */
    push[n++]=0x20048100; /* OFFSET_IN/OUT_UPPER/LOWER */
    push[n++]=(ULONG)(srcVa>>32);push[n++]=(ULONG)srcVa;
    push[n++]=(ULONG)(dstVa>>32);push[n++]=(ULONG)dstVa;
    push[n++]=0x20048104; /* PITCH_IN/OUT, LINE_LENGTH, LINE_COUNT */
    push[n++]=srcPitch;push[n++]=dstPitch;push[n++]=bytesPerLine;push[n++]=lineCount;
    /* Enable MULTI_LINE_ENABLE: LINE_COUNT and the configured pitches are
       ignored by the CE when this bit is clear, silently copying only the
       first 256-byte row. */
    push[n++]=0x200180c0;push[n++]=0x04000386; /* pitch->pitch, multi-line */
    push[n++]=0x20038090; /* SET_SEMAPHORE_A/B/PAYLOAD */
    push[n++]=(ULONG)((va+0xc000)>>32);push[n++]=(ULONG)(va+0xc000);push[n++]=fenceVal;
    push[n++]=0x200180c0;push[n++]=0x0400000c; /* flushed release */

    fifo[dev->GspGPPut%1024]=(pushVa&0xfffffffcULL)|(((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();
    dev->GspGPPut = (dev->GspGPPut + 1) % 1024;
    userd[0x8c/4] = dev->GspGPPut;
    __sync_synchronize();

    if(dev->Bar0Length>=0x30094) {
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),dev->GspSubmitToken);
    }
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
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);
    LogStr(" src=");LogHex(srcVa);LogStr(" dst=");LogHex(dstVa);
    LogStr(" srcPitch=");LogHex(srcPitch);LogStr(" dstPitch=");LogHex(dstPitch);
    LogStr(" lineBytes=");LogHex(bytesPerLine);LogStr(" lines=");LogHex(lineCount);
    LogStr(" runlist=");LogHex(dev->GspRunlistBase);
    LogStr(" submitToken=");LogHex(dev->GspSubmitToken);LogStr("\n");
    {
        ULONG chramBase=0;
        ULONG chcfg=VideoPortReadRegisterUlong((PULONG)
            ((PUCHAR)dev->Bar0MmioVirtual+dev->GspRunlistBase+4));
        chramBase=chcfg&0xfffffff0U;
        for(ULONG i=0;i<3;i++) {
            ULONG status=VideoPortReadRegisterUlong((PULONG)
                ((PUCHAR)dev->Bar0MmioVirtual+dev->GspRunlistBase+0x200+i*0x40));
            ULONG instLo=VideoPortReadRegisterUlong((PULONG)
                ((PUCHAR)dev->Bar0MmioVirtual+dev->GspRunlistBase+0x208+i*0x40));
            ULONG instHi=VideoPortReadRegisterUlong((PULONG)
                ((PUCHAR)dev->Bar0MmioVirtual+dev->GspRunlistBase+0x20c+i*0x40));
            LogStr("NVX [COPY-Q]: engine=");LogHex(i);
            LogStr(" channelStatus=");LogHex(status);
            LogStr(" instance=");LogHex(instHi);LogHex(instLo);LogStr("\n");
        }
        if(chramBase&&chramBase+8<=dev->Bar0Length) {
            LogStr("NVX [COPY-Q]: CHRAM slot0=");
            LogHex(VideoPortReadRegisterUlong((PULONG)
                ((PUCHAR)dev->Bar0MmioVirtual+chramBase)));
            LogStr(" slot1=");
            LogHex(VideoPortReadRegisterUlong((PULONG)
                ((PUCHAR)dev->Bar0MmioVirtual+chramBase+4)));
            LogStr("\n");
        }
        if(dev->CmdQueueVirtual&&dev->StatQueueVirtual) {
        {
            NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS fault={0};
            if(NvxGspControl(dev,dev->GspChannelHandle,
                             NVA06F_CTRL_CMD_GET_MMU_FAULT_INFO,
                             &fault,sizeof(fault))) {
                const NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS *reply=
                    (const NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS *)
                    (dev->RpcRxRecord+104);
                LogStr("NVX [COPY-Q]: MMU fault addrHi=");LogHex(reply->addrHi);
                LogStr(" addrLo=");LogHex(reply->addrLo);
                LogStr(" type=");LogHex(reply->faultType);
                if(reply->addrHi==0&&
                   reply->addrLo>=NVX_GART_GPU_VA_BASE&&
                   reply->addrLo<NVX_GART_GPU_VA_BASE+
                       (ULONG64)NVX_GART_SLICE_COUNT*NVX_GART_SLICE_BYTES&&
                   dev->GspGartPteVirtual) {
                    ULONG pteIndex=(ULONG)((reply->addrLo-
                        NVX_GART_GPU_VA_BASE)>>12);
                    LogStr(" gartPteIndex=");LogHex(pteIndex);
                    LogStr(" gartPte=");
                    LogHex(((PULONG64)dev->GspGartPteVirtual)[pteIndex]);
                    LogStr(" gartPtePhysical=");
                    LogHex(dev->GspGartPtePhysical.QuadPart+
                           (ULONG64)pteIndex*sizeof(ULONG64));
                }
                LogStr("\n");
            } else {
                LogStr("NVX [COPY-Q]: MMU fault info unavailable.\n");
            }
        }
        {
            NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS state={0};
            if(NvxGspControl(dev,dev->GspChannelHandle,
                             NVB06F_CTRL_CMD_GET_CHANNEL_HW_STATE,
                             &state,sizeof(state))) {
                const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *reply=
                    (const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *)
                    (dev->RpcRxRecord+104);
                LogStr("NVX [COPY-Q]: channel hardware state=");
                LogHex(reply->state);LogStr("\n");
            } else {
                LogStr("NVX [COPY-Q]: channel hardware state unavailable.\n");
            }
        }
        }
    }
    dev->GspChannelFaulted=TRUE;
    return FALSE;
}

/* Submit four block-linear GOB strips in one CE pushbuffer and wait once.
 * A 64x16 RGBA tile is four contiguous 16x16 strips in Turing/Ampere layout.
 * The four copy methods plus one release fit in the existing 256-byte slot. */
static BOOLEAN NvxGspSubmitColorTileStrips(PNVX_DEVICE_EXTENSION dev,
    const ULONG64 dstVa[4],const ULONG64 srcVa[4])
{
    PUCHAR scratch;
    volatile ULONG *userd,*fence;
    volatile ULONG64 *fifo;
    PULONG push;
    ULONG slot,n=0,fenceVal,i;
    ULONG64 va,pushVa;
    if(!dev||!dev->GspInitialized||!dev->GspCopyVerified||
       dev->GspChannelFaulted) return FALSE;
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET;
    userd=(volatile ULONG *)(scratch+0x3200);
    fence=(volatile ULONG *)(scratch+0xc000);
    fifo=(volatile ULONG64 *)scratch;
    va=dev->GspScratchGpuVa;
    slot=dev->GspGPPut%16;
    push=(PULONG)(scratch+0xd000+slot*0x100);
    pushVa=va+0xd000+slot*0x100;
    for(i=0;i<4;i++) {
        push[n++]=0x20018000;push[n++]=0xc7b5;
        push[n++]=0x20048100;
        push[n++]=(ULONG)(srcVa[i]>>32);push[n++]=(ULONG)srcVa[i];
        push[n++]=(ULONG)(dstVa[i]>>32);push[n++]=(ULONG)dstVa[i];
        push[n++]=0x20048104;
        push[n++]=1024;push[n++]=1024;push[n++]=1024;push[n++]=1;
        push[n++]=0x200180c0;push[n++]=0x04000386;
    }
    fenceVal=dev->GspNextFence++;
    if(!fenceVal) fenceVal=dev->GspNextFence++;
    push[n++]=0x20038090;
    push[n++]=(ULONG)((va+0xc000)>>32);push[n++]=(ULONG)(va+0xc000);
    push[n++]=fenceVal;
    push[n++]=0x200180c0;push[n++]=0x0400000c;
    if(n>64) return FALSE;
    fence[0]=0;__sync_synchronize();
    fifo[dev->GspGPPut%1024]=(pushVa&0xfffffffcULL)|
        (((pushVa>>32)&0xffULL)<<32)|((ULONG64)n<<42);
    __sync_synchronize();
    dev->GspGPPut=(dev->GspGPPut+1)%1024;
    userd[0x8c/4]=dev->GspGPPut;__sync_synchronize();
    if(dev->Bar0Length>=0x30094)
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
            0x30090),dev->GspSubmitToken);
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+
        dev->GspRunlistBase+0x90),1);
    for(ULONG wait=0;wait<20000;wait++) {
        if(fence[0]==fenceVal) {__sync_synchronize();return TRUE;}
        VideoPortStallExecution(10);
    }
    LogStr("NVX [COPY-Q]: color-tile strip batch timeout fence=");
    LogHex(fenceVal);LogStr(" GPGet=");LogHex(userd[0x88/4]);
    LogStr(" GPPut=");LogHex(userd[0x8c/4]);LogStr("\\n");
    dev->GspChannelFaulted=TRUE;
    return FALSE;
}

BOOLEAN NvxGspSubmitCopy(PVOID extension,ULONG64 dstVa,ULONG64 srcVa,ULONG bytes)
{
    return NvxGspSubmitCopy2D(extension,dstVa,srcVa,bytes,bytes,bytes,1);
}

#if NVX_ENABLE_GR_CHANNEL_PROBE
static VOID NvxGspLogGrMappingFault(PNVX_DEVICE_EXTENSION dev)
{
    NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS fault={0};
    NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS state={0};
    if(NvxGspControl(dev,dev->GspChannelHandle,NVA06F_CTRL_CMD_GET_MMU_FAULT_INFO,
                     &fault,sizeof(fault))) {
        const NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS *reply=
            (const NV906F_CTRL_GET_MMU_FAULT_INFO_PARAMS *)(dev->RpcRxRecord+104);
        LogStr("NVX [GR-MAP]: CE MMU fault addrHi=");LogHex(reply->addrHi);
        LogStr(" addrLo=");LogHex(reply->addrLo);
        LogStr(" type=");LogHex(reply->faultType);LogStr("\n");
    } else LogStr("NVX [GR-MAP]: CE MMU fault info unavailable.\n");
    if(NvxGspControl(dev,dev->GspChannelHandle,NVB06F_CTRL_CMD_GET_CHANNEL_HW_STATE,
                     &state,sizeof(state))) {
        const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *reply=
            (const NVB06F_CTRL_GET_CHANNEL_HW_STATE_PARAMS *)(dev->RpcRxRecord+104);
        LogStr("NVX [GR-MAP]: CE channel hardware state=");LogHex(reply->state);LogStr("\n");
    }
}
/* Exercise both ends of the nonprivileged test alias with the CE channel.
   This validates the alias/backing, not the protected mapping or GR execution. */
static BOOLEAN NvxGspVerifyGrBacking(PNVX_DEVICE_EXTENSION dev)
{
    volatile ULONG *scratch=(volatile ULONG *)((PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET+0xa000);
    ULONG slice,end,i;
    if(!dev->GspGrContextVirtual||!dev->GspGrContextAliasMapped||
       dev->GspGrContextLength!=GSP_GR_CONTEXT_BYTES||
       !dev->GspCopyVerified||dev->GspChannelFaulted) return FALSE;
    for(slice=0;slice<6;slice++) for(end=0;end<2;end++) {
        ULONG offset=slice*0x200000+(end?0x1fffc0:0);
        volatile ULONG *backing=(volatile ULONG *)((PUCHAR)dev->GspGrContextVirtual+offset);
        ULONG64 backingVa=GSP_GR_CONTEXT_TEST_GPU_VA+offset;
        ULONG64 scratchVa=dev->GspScratchGpuVa+0xa000;
        for(i=0;i<16;i++) {backing[i]=0x41a50000U^(slice<<8)^(end<<7)^i;scratch[i]=0;}
        __sync_synchronize();
        if(!NvxGspSubmitCopy(dev,scratchVa,backingVa,64)) {
            LogStr("NVX [GR-MAP]: GPU read from context backing failed.\n");
            NvxGspLogGrMappingFault(dev);return FALSE;
        }
        for(i=0;i<16;i++) if(scratch[i]!=(0x41a50000U^(slice<<8)^(end<<7)^i)) {
            LogStr("NVX [GR-MAP]: GPU readback mismatch at offset=");LogHex(offset);LogStr("\n");return FALSE;
        }
        for(i=0;i<16;i++) {scratch[i]=0xc31f0000U^(slice<<8)^(end<<7)^i;backing[i]=0;}
        __sync_synchronize();
        if(!NvxGspSubmitCopy(dev,backingVa,scratchVa,64)) {
            LogStr("NVX [GR-MAP]: GPU write to context backing failed.\n");
            NvxGspLogGrMappingFault(dev);return FALSE;
        }
        for(i=0;i<16;i++) if(backing[i]!=(0xc31f0000U^(slice<<8)^(end<<7)^i)) {
            LogStr("NVX [GR-MAP]: GPU write mismatch at offset=");LogHex(offset);LogStr("\n");return FALSE;
        }
        for(i=0;i<16;i++) {backing[i]=0;scratch[i]=0;}
    }
    __sync_synchronize();
    dev->GspGrContextAliasVerified=TRUE;
    LogStr("NVX [GR-MAP]: GPU read/write verified for nonprivileged alias; protected mapping remains unverified.\n");
    return TRUE;
}
#endif

BOOLEAN NvxGspSubmitCopy2D(PVOID extension,ULONG64 dstVa,ULONG64 srcVa,
                           ULONG srcPitch,ULONG dstPitch,ULONG bytesPerLine,ULONG lineCount)
{
    BOOLEAN result;
    if(!NvxGspLockUserScratch()) return FALSE;
    result=NvxGspSubmitCopy2DLocked(extension,dstVa,srcVa,srcPitch,dstPitch,
                                    bytesPerLine,lineCount);
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return result;
}

BOOLEAN NvxGspSubmitCopy2DLocked(PVOID extension,ULONG64 dstVa,ULONG64 srcVa,
                                 ULONG srcPitch,ULONG dstPitch,
                                 ULONG bytesPerLine,ULONG lineCount)
{
    BOOLEAN result;
    if(!NvxGspLockEngine()) return FALSE;
    result=NvxGspSubmitCopyUnlocked(extension,dstVa,srcVa,srcPitch,dstPitch,
                                    bytesPerLine,lineCount);
    NvxGspUnlockEngine();
    return result;
}

/* Read one 64x16 RGBA8 tile from a dynamic Ampere color target. With a
 * 16-row block height, each 16-pixel-wide GOB strip is contiguous, while
 * adjacent strips are separated by the block-height interleave. Stage the
 * four strips separately, then decode the verified Turing GOB ordering. */
BOOLEAN NvxGspReadUserColorTargetTile(PVOID HwDeviceExtension,ULONG64 srcVa,
    ULONG targetWidth,ULONG targetHeight,ULONG tileMode,ULONG tileX,ULONG tileY,
    PULONG output,ULONG outputPixels)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    PUCHAR scratch;
    ULONG strip,x,y,activeStrips=0;
    ULONG64 stripDst[4],stripSrc[4];
    ULONG wait;
    BOOLEAN ok=FALSE;
    if(!dev||!output||outputPixels!=NVX_3D_READBACK_PIXELS||!srcVa||
       (srcVa&0xffffULL)||tileX%64U||tileY%16U||tileX>=targetWidth||
       tileY>=targetHeight||tileMode<0x10U||tileMode>0x70U||
       !Nvx3dComputeColorSurfaceLayout(targetWidth,targetHeight,&layout)||
       layout.TileMode!=tileMode||!dev->GspDmaVirtual||
       !dev->GspScratchGpuVa||!dev->GspCopyVerified||dev->GspChannelFaulted)
        return FALSE;
    for(wait=0;wait<20000;wait++) {
        if(!__sync_lock_test_and_set(&NvxGrUserSubmitBusy,1)) break;
        VideoPortStallExecution(10);
    }
    if(wait==20000) return FALSE;
    if(!NvxGspLockEngine()) {
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    scratch=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET+0xe000;
    VideoPortZeroMemory(scratch,NVX_3D_READBACK_BYTES);
    for(strip=0;strip<4;strip++) {
        ULONG xOrigin=tileX+strip*16U;
        ULONG offset;
        if(xOrigin>=targetWidth) {
            stripDst[strip]=stripSrc[strip]=0;
            continue;
        }
        offset=Nvx3dTuringColorPixelOffset(xOrigin,tileY,targetWidth,tileMode);
        if((ULONG64)offset+1024U>layout.Bytes)
            goto done;
        stripDst[strip]=dev->GspScratchGpuVa+0xe000+strip*1024U;
        stripSrc[strip]=srcVa+offset;
        activeStrips++;
    }
    if(activeStrips==4) {
        if(!NvxGspSubmitColorTileStrips(dev,stripDst,stripSrc)) goto done;
    } else {
        for(strip=0;strip<4;strip++)
            if(stripSrc[strip]&&
               !NvxGspSubmitCopyUnlocked(dev,stripDst[strip],stripSrc[strip],
                                          1024U,1024U,1024U,1U))
                goto done;
    }
    __sync_synchronize();
    for(y=0;y<16;y++) for(x=0;x<64;x++) {
        ULONG offset=Nvx3dTuringColorPixelOffset(x,y,64U,0x10U);
        output[y*64U+x]=*(volatile ULONG *)(scratch+offset);
    }
    ok=TRUE;
done:
    NvxGspUnlockEngine();
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return ok;
}

static BOOLEAN NvxGspSubmitFillUnlocked(PVOID HwDeviceExtension,ULONG64 dstVa,ULONG color,ULONG bytes)
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
    push=(PULONG)(scratch+0xd000+slot*0x100);
    pushVa=va+0xd000+slot*0x100;
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

    if(dev->Bar0Length>=0x30094) {
        VideoPortWriteRegisterUlong((PULONG)((PUCHAR)dev->Bar0MmioVirtual+0x30090),dev->GspSubmitToken);
    }
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

BOOLEAN NvxGspSubmitFill(PVOID extension,ULONG64 dstVa,ULONG color,ULONG bytes)
{
    BOOLEAN result;
    if(!NvxGspLockUserScratch()) return FALSE;
    result=NvxGspSubmitFillLocked(extension,dstVa,color,bytes);
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return result;
}

BOOLEAN NvxGspSubmitFillLocked(PVOID extension,ULONG64 dstVa,ULONG color,ULONG bytes)
{
    BOOLEAN result;
    if(!NvxGspLockEngine()) return FALSE;
    result=NvxGspSubmitFillUnlocked(extension,dstVa,color,bytes);
    NvxGspUnlockEngine();
    return result;
}

static BOOLEAN NvxGspUploadUnlocked(PVOID HwDeviceExtension,ULONG64 dstVa,const void *src,ULONG bytes)
{
    PNVX_DEVICE_EXTENSION dev=(PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    BOOLEAN gart;
    if(!dev||!src||!bytes||bytes>GSP_UPLOAD_MAX_BYTES||
       !dev->GspInitialized||!dev->GspCopyVerified||dev->GspChannelFaulted||
       !dev->GspDmaVirtual||!dev->GspVramGpuVa) return FALSE;
    gart=NvxGspUserAllocationContains(dev,dstVa,bytes,TRUE);
    if(!gart&&NvxMmuCheckRange(dev->GspVramGpuVa,dev->GspVramMapBytes,
                               dstVa,bytes)!=NVX_MMU_OK)
        return FALSE;
    if(!gart) {
        ULONG64 offset=dstVa-dev->GspVramGpuVa;
        if(!dev->GspFbLayoutValid ||
           offset>dev->GspFbFreeLimit || bytes-1>dev->GspFbFreeLimit-offset) return FALSE;
    }
    VideoPortMoveMemory((PUCHAR)dev->GspDmaVirtual+GSP_UPLOAD_OFFSET,(PVOID)src,bytes);
    __sync_synchronize();
    if(!NvxGspSubmitCopyUnlocked(HwDeviceExtension,dstVa,GSP_UPLOAD_GPU_VA,
                                 bytes,bytes,bytes,1))
        return FALSE;
    /* Verify the real staging path, not just the separately mapped scratch. */
    {
        static ULONG verifiedUploads[2]={0,0};
        ULONG adapter=gart?1U:(dstVa-dev->GspVramGpuVa)>=0x2000000ULL;
        if(verifiedUploads[adapter]<3) {
            ULONG count=bytes<256?bytes:256, mismatches=0;
            PUCHAR readback=(PUCHAR)dev->GspDmaVirtual+GSP_SCRATCH_OFFSET+0xa000;
            if(!NvxGspSubmitCopyUnlocked(HwDeviceExtension,dev->GspScratchGpuVa+0xa000,dstVa,
                                         count,count,count,1))
                return FALSE;
            for(ULONG i=0;i<count;i++)
                if(readback[i]!=((const UCHAR *)src)[i]) mismatches++;
            verifiedUploads[adapter]++;
            LogStr("NVX [UPLOAD-CHECK]: dst=");LogHex(dstVa);
            LogStr(" GPU readback bytes=");LogHex(count);
            LogStr(" mismatches=");LogHex(mismatches);LogStr("\n");
            if(mismatches) return FALSE;
        }
    }
    return TRUE;
}

/* Two XDDM adapters share one coherent staging buffer and CE queue. */
BOOLEAN NvxGspUpload(PVOID extension,ULONG64 dstVa,const void *src,ULONG bytes)
{
    BOOLEAN result;
    if(!NvxGspLockUserScratch()) return FALSE;
    if(!NvxGspLockEngine()) {
        __sync_lock_release(&NvxGrUserSubmitBusy);
        return FALSE;
    }
    result=NvxGspUploadUnlocked(extension,dstVa,src,bytes);
    NvxGspUnlockEngine();
    __sync_lock_release(&NvxGrUserSubmitBusy);
    return result;
}

