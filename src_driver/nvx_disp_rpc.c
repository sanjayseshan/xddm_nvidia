/*
 * NVIDIA Ampere GA104 Display RPC & Native Scanout Subsystem
 */

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

/* A BIOS modeset can replace display instance/channel state. Release RM's
   objects first; never carry a cached ready flag across an INT10 modeset. */
BOOLEAN NvxGspReleaseDisplayChannels(PVOID extension)
{
    PNVX_DEVICE_EXTENSION dev=extension;
    BOOLEAN released=TRUE;
    if(!dev) return FALSE;
    if(dev->IsSecondaryDisplay) dev=dev->PrimaryDeviceExtension;
    if(!dev) return FALSE;
    dev->NativeDisplayChannelsReady=FALSE;
    dev->NativeDualActive=FALSE;
    dev->NativeDisplayNotifierReady=FALSE;
    for(ULONG channel=3;channel>0;channel--) {
        ULONG *handle=&dev->NativeDisplayChannels[channel-1];
        if(!*handle) continue;
        if(!NvxNativeFreeObject(dev,dev->NativeDisplayHandle,*handle)) {
            released=FALSE;
            LogStr("NVX [NATIVE]: display channel release unconfirmed; retaining handle=");
            LogHex(*handle);LogStr("; BIOS modeset blocked.\n");
        } else *handle=0;
    }
    /* Retry initialization only once all old channel objects are gone. */
    dev->NativeDisplayAttempted=!released;
    if(released) {
        dev->NativeDisplayFaulted=FALSE;
        dev->NativeDisplayCorePut=0;
        dev->GspScanoutMapped=FALSE;
        LogStr("NVX [NATIVE]: display channels released before BIOS modeset; lazy reinitialization enabled.\n");
    }
    return released;
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

static VOID NvxNativeReadOutputInfo(PNVX_DEVICE_EXTENSION dev)
{
    for(ULONG monitor=0;monitor<dev->MonitorCount;monitor++)
        dev->MonitorSor[monitor]=0xffffffffUL;
    for(ULONG monitor=0;monitor<dev->MonitorCount;monitor++) {
        /* RM 535 OR_GET_INFO: 40-byte prefix, u64 VBIOS, two bools, pad. */
        ULONG params[14]={0,dev->MonitorIds[monitor]},*reply=(ULONG *)dev->RpcRxRecord;
        if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO,params,sizeof(params))) continue;
        dev->MonitorSor[monitor]=reply[28];
        dev->MonitorProtocol[monitor]=reply[30];
        LogStr("NVX [OUTPUT]: display=");LogHex(dev->MonitorIds[monitor]);
        LogStr(" index=");LogHex(reply[28]);LogStr(" type=");LogHex(reply[29]);
        LogStr(" protocol=");LogHex(reply[30]);LogStr(" litByVbios=");LogHex(reply[38]&255);LogStr("\n");
    }
}

/* A common-buffer mapping does not establish the display engine's snoop
   policy. Publish dirty CPU lines explicitly before PUT, and invalidate the
   clean notifier line when polling device writes. CLFLUSH is ordered by the
   final full fence; derive its line size from CPUID rather than assuming 64. */
static BOOLEAN NvxNativeFlushDma(PVOID address,ULONG bytes)
{
    unsigned a,b,c,d,line;
    uintptr_t start,end;
    if(!bytes) return TRUE;
    __asm__ __volatile__("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1),"c"(0));
    line=((b>>8)&255)*8;
    if(!(d&(1U<<19)) || !line || (line&(line-1))) return FALSE;
    start=(uintptr_t)address&~((uintptr_t)line-1);
    end=(uintptr_t)address+bytes;
    for(;start<end;start+=line)
        __asm__ __volatile__("clflush (%0)" : : "r"(start) : "memory");
    __sync_synchronize();
    return TRUE;
}

static BOOLEAN NvxNativeWaitNotifier(volatile ULONG *notifier)
{
    for(ULONG wait=0;wait<2000;wait++) {
        if(!NvxNativeFlushDma((PVOID)notifier,16)) return FALSE;
        if((notifier[0]>>30)==2) return TRUE;
        VideoPortStallExecution(100);
    }
    return FALSE;
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
    /* The primary surface mapping follows SET_CURRENT_MODE. Do not publish
       live channels during boot or a premature secondary activation request. */
    if(!dev->FramebufferVirtual) {
        LogStr("NVX [NATIVE]: waiting for primary surface mapping after BIOS modeset.\n");
        return FALSE;
    }
    if(!dev->GspCopyVerified || !dev->NativeDisplayHandle || !dev->GspFbLayoutValid ||
       dev->GspChannelFaulted || dev->Bar0Length<0x693000 ||
       dev->GspDmaLength<GSP_DISPLAY_PUSH_OFFSET+0x3000 ||
       dev->DisplayHeadCount<2 || (dev->DisplayWindowMask&5)!=5) return FALSE;
    /* v169 trained the link before creating native channels. Retraining an
       already prepared transmitter can replace the state those channels use. */
    NvxGspTrainSecondaryDpLink(dev);
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
    if(!NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM,&instance,sizeof(instance))) {
        LogStr("NVX [NATIVE]: display instance registration failed; no channels submitted.\n");return FALSE;
    }
    /* Audit the hardware root after RM registration; do not override GSP's
       instance configuration based solely on an inferred cold-boot cause. */
    {
        ULONG config=VideoPortReadRegisterUlong((PULONG)(bar0+0x610010));
        ULONG root=VideoPortReadRegisterUlong((PULONG)(bar0+0x610014));
        ULONG expected=(ULONG)(base>>16);
        LogStr("NVX [NATIVE]: instance hardware config=");LogHex(config);
        LogStr(" root=");LogHex(root);LogStr(" expected=");LogHex(expected);LogStr("\n");
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
        /* RM NV_MEMORY_CACHED=0 selects coherent PCI system-memory fetch. */
        push.limit=0xfff;push.cache=0;push.hclass=classes[channel];
        push.instance=instances[channel];push.valid=1;
        __sync_synchronize();
        if(!NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER,&push,sizeof(push))) goto failed;
        args.instance=instances[channel];
        if(!NvxGspAlloc(dev,dev->NativeDisplayHandle,handle,classes[channel],&args,sizeof(args))) goto failed;
        dev->NativeDisplayChannels[channel]=handle;
        /* MMIO PUT/GET pack the DWORD pointer into bits 11:2. */
        if(!NvxNativeFlushDma(buffer,0x1000)) goto failed;
        VideoPortWriteRegisterUlong((PULONG)(bar0+offsets[channel]),NvxDisplayPutBytes(4));
        for(ULONG wait=0;wait<2000;wait++) {
            ULONG get=VideoPortReadRegisterUlong((PULONG)(bar0+offsets[channel]+4));
            if(get!=0xffffffff && NvxDisplayGetWords(get)==4) {
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

    LogStr("NVX [NATIVE]: core, Window0 and Window2 DMA verified; native handoff pending.\n");
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
    PNVX_DEVICE_EXTENSION dev = (PNVX_DEVICE_EXTENSION)HwDeviceExtension;
    NVX_DP_AUX_CTRL_PARAMS aux = {0};
    NVX_DP_MAIN_LINK_CTRL_PARAMS ml = {0};
    const ULONG *reply;
    const NVX_DP_AUX_CTRL_PARAMS *auxResult;
    ULONG displayId;

    if (!dev || !dev->GspDisplayHandle) return;
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

/* Check actual RM routing and sink link lock, not just pushbuffer fetch. */
static BOOLEAN NvxGspVerifyDisplays(PNVX_DEVICE_EXTENSION dev)
{
    BOOLEAN primaryActive=FALSE,secondaryActive=FALSE;
    for(ULONG head=0;head<2;head++) {
        ULONG active[4]={0,head,0,0};
        ULONG id=0;
        if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE,active,sizeof(active))) return FALSE;
        id=((const ULONG *)dev->RpcRxRecord)[29];
        LogStr("NVX [DISPLAY-VERIFY]: head=");LogHex(head);
        LogStr(" activeDisplay=");LogHex(id);LogStr("\n");
        if(head==0 && id==dev->MonitorIds[0]) primaryActive=TRUE;
        if(head==1 && id && (id==dev->SecondaryDpDisplayId || (dev->MonitorCount>1 && id==dev->MonitorIds[1]))) secondaryActive=TRUE;
        if(id) {
            NVX_DP_AUX_CTRL_PARAMS aux={0};
            aux.displayId=id;aux.cmd=9;aux.addr=0x202;aux.size=2;
            if(NvxGspControl(dev,dev->GspDisplayHandle,NVX_CTRL_CMD_DP_AUXCH_CTRL,&aux,sizeof(aux))) {
                const NVX_DP_AUX_CTRL_PARAMS *r=(const void *)((const ULONG *)dev->RpcRxRecord+26);
                BOOLEAN locked=r->replyType==0 && r->size==3 &&
                    (r->data[0]&0x77)==0x77 && (r->data[1]&0x77)==0x77 && (r->data[2]&1);
                LogStr("NVX [DISPLAY-VERIFY]: display=");LogHex(id);
                LogStr(" auxLock=");LogHex(locked);
                LogStr(" status=");LogHex(r->data[0]|(r->data[1]<<8)|(r->data[2]<<16));LogStr("\n");
                if(!locked) { if(head==1) secondaryActive=FALSE;else primaryActive=FALSE; }
            } else { if(head==1) secondaryActive=FALSE;else primaryActive=FALSE; }
        }
    }
    LogStr("NVX [DISPLAY-VERIFY]: bothHeadsActive=");LogHex(primaryActive&&secondaryActive);LogStr("\n");
    return primaryActive && secondaryActive && !dev->NativeDisplayFaulted;
}

/* Read-only snapshots: distinguish DMA-object lookup faults from an UPDATE
   which was fetched but is waiting for display supervision/interlocks. */
static VOID NvxNativeLogChannelState(PNVX_DEVICE_EXTENSION dev)
{
    PUCHAR bar0=dev->Bar0MmioVirtual;
    const ULONG channels[3]={0,1,3};
    const ULONG users[3]={0x680000,0x690000,0x692000};
    for(ULONG i=0;i<3;i++) {
        ULONG error=0x611020+channels[i]*12;
        LogStr("NVX [EVO]: channel=");LogHex(channels[i]);
        LogStr(" PUT=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+users[i])));
        LogStr(" GET=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+users[i]+4)));
        LogStr(" exception=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+error)));
        LogStr(" data=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+error+4)));
        LogStr(" code=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+error+8)));LogStr("\n");
    }
    LogStr("NVX [EVO]: supervisor=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x6107a8)));
    LogStr(" errorPending=");LogHex(VideoPortReadRegisterUlong((PULONG)(bar0+0x611848)));LogStr("\n");
}

/* Methods use the byte-addressed EVO wire format. */
static VOID NvxNativeMethod(PULONG push,PULONG count,ULONG method,ULONG data)
{
    uint32_t header=0;
    (void)NvxDisplayPushHeader(method,1,&header);
    push[(*count)++]=header;push[(*count)++]=data;
}

static VOID NvxNativeWindow(PULONG push,PULONG count,ULONG width,ULONG height,
                            ULONG pitch,ULONG offset,ULONG handle)
{
    NvxNativeMethod(push,count,0x308,0);
    NvxNativeMethod(push,count,0x224,(height<<16)|width);
    NvxNativeMethod(push,count,0x228,0); /* Linear DMA context kind, C67E. */
    NvxNativeMethod(push,count,0x22c,0xcf); /* Known accepted v159 ARGB image format. */
    NvxNativeMethod(push,count,0x230,pitch>>6);
    NvxNativeMethod(push,count,0x240,handle);
    NvxNativeMethod(push,count,0x260,offset>>8);
    NvxNativeMethod(push,count,0x298,(height<<16)|width);
    NvxNativeMethod(push,count,0x2a4,(height<<16)|width);
    NvxNativeMethod(push,count,0x370,1); /* Interlock with core. */
    NvxNativeMethod(push,count,0x200,1);
}

static BOOLEAN NvxNativeHead(PULONG push,PULONG count,ULONG head,
                              const NVX_DISPLAY_TIMING *mode,ULONG width,ULONG height,
                              ULONG display,ULONG sor,ULONG protocol)
{
    NVX_EVO_RASTER raster;
    ULONG h=head*0x400;
    ULONG output=0xfc000040;
    if(!NvxEncodeEvoRaster(mode,&raster) || sor>=4 ||
       (protocol!=8 && protocol!=9)) return FALSE;
    if(!(mode->flags&2)) output|=4;
    if(!(mode->flags&4)) output|=8;
    NvxNativeMethod(push,count,0x2000+h,0); /* RGB, VESA range. */
    NvxNativeMethod(push,count,0x2004+h,output);
    NvxNativeMethod(push,count,0x2008+h,0); /* Progressive, no lock pins. */
    NvxNativeMethod(push,count,0x200c+h,mode->pixelClockHz);
    NvxNativeMethod(push,count,0x2014+h,0x11); /* Two-tap output scaler. */
    NvxNativeMethod(push,count,0x201c+h,0);
    NvxNativeMethod(push,count,0x2020+h,display);
    NvxNativeMethod(push,count,0x2024+h,0);
    NvxNativeMethod(push,count,0x2028+h,mode->pixelClockHz);
    NvxNativeMethod(push,count,0x2030+h,0x1114);
    NvxNativeMethod(push,count,0x2048+h,0);
    NvxNativeMethod(push,count,0x204c+h,(height<<16)|width);
    NvxNativeMethod(push,count,0x2058+h,(mode->vActive<<16)|mode->hActive);
    NvxNativeMethod(push,count,0x205c+h,0);
    NvxNativeMethod(push,count,0x2064+h,raster.size);
    NvxNativeMethod(push,count,0x2068+h,raster.syncEnd);
    NvxNativeMethod(push,count,0x206c+h,raster.blankEnd);
    NvxNativeMethod(push,count,0x2070+h,raster.blankStart);
    NvxNativeMethod(push,count,0x2078+h,0);
    NvxNativeMethod(push,count,NVX_C67D_SOR_SET_CONTROL(sor),(protocol<<8)|(1U<<head));
    return TRUE;
}

BOOLEAN NvxNativeSetSecondaryScanout(PVOID extension)
{
    PNVX_DEVICE_EXTENSION dev=extension,sec;
    PUCHAR bar0;
    PULONG core,windows[2];
    volatile ULONG *notifier;
    ULONG counts[2],n;
    ULONG widths[2],heights[2],pitches[2];
    const ULONG offsets[2]={0,0x2000000};
    const ULONG users[2]={1,3};
    const ULONG windowIds[2]={0,2};
    const ULONG channels[2]={1,2};
    NVX_DISPLAY_TIMING modes[2];
    BOOLEAN ownersChanged=FALSE;
    if(!dev) return FALSE;
    if(dev->IsSecondaryDisplay) {
        dev = (PNVX_DEVICE_EXTENSION)dev->PrimaryDeviceExtension;
        if(!dev) return FALSE;
    }
    if(!dev->NativeDisplayChannelsReady) {
        LogStr("NVX [NATIVE]: display channels not yet ready; initializing now.\n");
        if(!NvxGspInitDisplayChannels(dev)) {
            LogStr("NVX [NATIVE]: display channel initialization failed.\n");
            return FALSE;
        }
    }
    if(dev->NativeDisplayFaulted || dev->MonitorCount<2 || !(sec=dev->SecondaryDeviceExtension)) {
        LogStr("NVX [NATIVE]: cannot set secondary scanout: faulted=");
        LogHex(dev->NativeDisplayFaulted);
        LogStr(" monitors=");LogHex(dev->MonitorCount);
        LogStr(" sec=");LogHex((unsigned long long)dev->SecondaryDeviceExtension);
        LogStr("\n");
        return FALSE;
    }
    {
        ULONG primMode = dev->CurrentModeIndex;
        if (primMode >= dev->NumModes || dev->Modes[primMode].BitsPerPlane != 32 ||
            dev->Modes[primMode].VisScreenWidth < 640) {
            /* Match dynamically against Head 0 decoded EDID timing if available */
            ULONG targetW = dev->MonitorTiming[0].hActive ? dev->MonitorTiming[0].hActive : 1920;
            ULONG targetH = dev->MonitorTiming[0].vActive ? dev->MonitorTiming[0].vActive : 1080;
            for (ULONG m = 0; m < dev->NumModes; m++) {
                if (dev->Modes[m].VisScreenWidth == targetW &&
                    dev->Modes[m].VisScreenHeight == targetH &&
                    dev->Modes[m].BitsPerPlane == 32) {
                    primMode = m;
                    dev->CurrentModeIndex = m;
                    break;
                }
            }
        }
        if (sec->CurrentModeIndex >= sec->NumModes || sec->Modes[sec->CurrentModeIndex].BitsPerPlane != 32 ||
            sec->Modes[sec->CurrentModeIndex].VisScreenWidth < 640) {
            /* Match dynamically against Head 1 decoded EDID timing if available */
            ULONG targetW = dev->MonitorTiming[1].hActive ? dev->MonitorTiming[1].hActive : 2560;
            ULONG targetH = dev->MonitorTiming[1].vActive ? dev->MonitorTiming[1].vActive : 1440;
            for (ULONG m = 0; m < sec->NumModes; m++) {
                if (sec->Modes[m].VisScreenWidth == targetW &&
                    sec->Modes[m].VisScreenHeight == targetH &&
                    sec->Modes[m].BitsPerPlane == 32) {
                    sec->CurrentModeIndex = m;
                    break;
                }
            }
        }
    }
    if(dev->CurrentModeIndex>=dev->NumModes || sec->CurrentModeIndex>=sec->NumModes) {
        LogStr("NVX [NATIVE]: invalid mode index prim=");
        LogHex(dev->CurrentModeIndex);LogStr(" sec=");LogHex(sec->CurrentModeIndex);LogStr("\n");
        return FALSE;
    }
    modes[0]=dev->MonitorTiming[0];modes[1]=dev->MonitorTiming[1];
    widths[0]=dev->Modes[dev->CurrentModeIndex].VisScreenWidth;
    heights[0]=dev->Modes[dev->CurrentModeIndex].VisScreenHeight;
    pitches[0]=dev->Modes[dev->CurrentModeIndex].ScreenStride;
    widths[1]=sec->Modes[sec->CurrentModeIndex].VisScreenWidth;
    heights[1]=sec->Modes[sec->CurrentModeIndex].VisScreenHeight;
    pitches[1]=sec->Modes[sec->CurrentModeIndex].ScreenStride;
    LogStr("NVX [NATIVE]: dual scanout config: Head0=");
    LogHex(widths[0]);LogStr("x");LogHex(heights[0]);LogStr(" pitch=");LogHex(pitches[0]);
    LogStr(" Head1=");LogHex(widths[1]);LogStr("x");LogHex(heights[1]);LogStr(" pitch=");LogHex(pitches[1]);
    LogStr("\n");
    for(ULONG i=0;i<2;i++) {
        NVX_EVO_RASTER raster;
        if(!NvxEncodeEvoRaster(&modes[i],&raster) || dev->MonitorSor[i]>=4 ||
           (dev->MonitorProtocol[i]!=8 && dev->MonitorProtocol[i]!=9) ||
           !widths[i] || !heights[i] || pitches[i]<widths[i]*4 || (pitches[i]&63)) {
            LogStr("NVX [NATIVE]: validation failed for head=");LogHex(i);
            LogStr(" w=");LogHex(widths[i]);LogStr(" h=");LogHex(heights[i]);
            LogStr(" pitch=");LogHex(pitches[i]);LogStr(" sor=");LogHex(dev->MonitorSor[i]);
            LogStr(" proto=");LogHex(dev->MonitorProtocol[i]);LogStr("\n");
            return FALSE;
        }
    }
    if(dev->MonitorSor[0]==dev->MonitorSor[1]) {
        LogStr("NVX [NATIVE]: SOR collision between heads\n");
        return FALSE;
    }

    /* Link preparation ran before native channel creation. Do not power up
       and retrain it again between DMA verification and ownership UPDATE. */

    bar0=dev->Bar0MmioVirtual;
    core=(PULONG)((PUCHAR)dev->GspDmaVirtual+GSP_DISPLAY_PUSH_OFFSET);
    notifier=(volatile ULONG *)((PUCHAR)core+0x3000);
    VideoPortZeroMemory((PVOID)notifier,0x1000);

    /* Bind independent image DMA objects for both window channels. */
    for(ULONG i=0;i<3;i++) {
        ULONG desc[6]={0x45,0,0,0,0,0};
        uint32_t slot,entry[2];
        ULONG handle=0xd1500001+i;
        ULONG descriptor=0x2000+i*0x20;
        ULONG64 instanceVa=dev->GspVramGpuVa+dev->NativeDisplayInstance;
        if(i==0) {
            ULONG64 physical=dev->GspDmaPhysical.QuadPart+GSP_DISPLAY_PUSH_OFFSET+0x3000;
            /* NV_DMA_TARGET_NODE_PHYSICAL_PCI_COHERENT (3), writable (4).
               The notifier must use snooped system-memory transactions. */
            desc[0]=0x47;desc[1]=(ULONG)(physical>>8);desc[2]=(ULONG)(physical>>40);
            desc[3]=(ULONG)((physical+0xfff)>>8);desc[4]=(ULONG)((physical+0xfff)>>40);
        } else {
            ULONG j=i-1;
            ULONG64 end=offsets[j]+(ULONG64)pitches[j]*heights[j]-1;
            if((j==0 && end>=dev->NativeDisplayInstance) ||
               (j==1 && (offsets[1]<0x2000000 || end>dev->GspFbFreeLimit))) {
                LogStr("NVX [NATIVE]: DMA descriptor limit check failed for window=");
                LogHex(j);LogStr(" end=");LogHex(end);LogStr("\n");
                return FALSE;
            }
            desc[3]=(ULONG)(end>>8);
        }
        if(!NvxDisplayRamhtEntry(handle,i?users[i-1]:0,descriptor,
                                 dev->GspClientHandle,&slot,entry) ||
           !NvxGspUpload(dev,instanceVa+descriptor,desc,sizeof(desc)) ||
           !NvxGspUpload(dev,instanceVa+slot*8,entry,sizeof(entry))) {
            LogStr("NVX [NATIVE]: RAMHT or DMA bind failed for handle=");LogHex(handle);LogStr("\n");
            return FALSE;
        }
        {
            ULONG legacy=NvxDisplayRamhtV169Slot(handle,i?users[i-1]:0);
            if(legacy!=slot && !NvxGspUpload(dev,instanceVa+legacy*8,entry,sizeof(entry)))
                return FALSE;
            LogStr("NVX [RAMHT]: handle=");LogHex(handle);
            LogStr(" clientSlot=");LogHex(slot);LogStr(" v169Slot=");LogHex(legacy);LogStr("\n");
        }
    }
    n=NvxDisplayGetWords(VideoPortReadRegisterUlong((PULONG)(bar0+0x680000)));
    if(n>600) {
        LogStr("NVX [NATIVE]: core pushbuffer limit exceeded: ");LogHex(n);LogStr("\n");
        return FALSE;
    }
    for(ULONG i=0;i<2;i++) {
        ULONG owner=VideoPortReadRegisterUlong((PULONG)(bar0+0x681000+windowIds[i]*0x80))&15;
        if(owner!=i) {
            NvxNativeMethod(core,&n,NVX_C67D_WINDOW_SET_CONTROL(windowIds[i]),i);
            ownersChanged=TRUE;
        }
    }
    if(ownersChanged) {
        /* Ownership must be committed with NO window/cursor interlocks. */
        NvxNativeMethod(core,&n,0x208,0xd1500001);
        NvxNativeMethod(core,&n,0x20c,0x1000);
        NvxNativeMethod(core,&n,0x218,0);
        NvxNativeMethod(core,&n,0x21c,0);
        NvxNativeMethod(core,&n,0x200,1);
        if(!NvxNativeFlushDma(core,0x1000) ||
           !NvxNativeFlushDma((PVOID)notifier,16)) return FALSE;
        VideoPortWriteRegisterUlong((PULONG)(bar0+0x680000),NvxDisplayPutBytes(n));
        (void)NvxNativeWaitNotifier(notifier);
        LogStr("NVX [DISPLAY-VERIFY]: ownership updateNotifier=");LogHex(notifier[0]);LogStr("\n");
        if((notifier[0]>>30)!=2) {
            NvxNativeLogChannelState(dev);
            (void)NvxGspVerifyDisplays(dev);return FALSE;
        }
        VideoPortZeroMemory((PVOID)notifier,16);
    }
    /* Full native state for both heads: do not replay partial VBE state. */
    for(ULONG window=0;window<8;window++) {
        NvxNativeMethod(core,&n,0x1004+window*0x80,0x1f);
        NvxNativeMethod(core,&n,0x1008+window*0x80,0);
        NvxNativeMethod(core,&n,0x1010+window*0x80,0x00117fff);
    }
    for(ULONG i=0;i<2;i++) {
        windows[i]=(PULONG)((PUCHAR)core+channels[i]*0x1000);
        counts[i]=NvxDisplayGetWords(VideoPortReadRegisterUlong((PULONG)(bar0+
                                    (i?0x692000:0x690000))));
        if(counts[i]>900) {
            LogStr("NVX [NATIVE]: window pushbuffer limit exceeded for head=");LogHex(i);LogStr("\n");
            return FALSE;
        }
        NvxNativeWindow(windows[i],&counts[i],widths[i],heights[i],pitches[i],
                        offsets[i],0xd1500002+i);
        if(!NvxNativeHead(core,&n,i,&modes[i],widths[i],heights[i],
                          dev->MonitorIds[i],dev->MonitorSor[i],dev->MonitorProtocol[i])) {
            LogStr("NVX [NATIVE]: NvxNativeHead failed for head=");LogHex(i);LogStr("\n");
            return FALSE;
        }
        LogStr("NVX [NATIVE]: head=");LogHex(i);LogStr(" nativeWidth=");LogHex(modes[i].hActive);
        LogStr(" nativeHeight=");LogHex(modes[i].vActive);LogStr(" surfaceWidth=");LogHex(widths[i]);
        LogStr(" surfaceHeight=");LogHex(heights[i]);LogStr("\n");
    }
    NvxNativeMethod(core,&n,0x208,0xd1500001);
    NvxNativeMethod(core,&n,0x20c,0x1000);
    NvxNativeMethod(core,&n,0x218,0);
    NvxNativeMethod(core,&n,0x21c,5); /* Window0 and Window2. */
    NvxNativeMethod(core,&n,0x200,1);
    if(!NvxNativeFlushDma(core,0x1000) ||
       !NvxNativeFlushDma(windows[0],0x1000) ||
       !NvxNativeFlushDma(windows[1],0x1000) ||
       !NvxNativeFlushDma((PVOID)notifier,16)) return FALSE;
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x690000),NvxDisplayPutBytes(counts[0]));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x692000),NvxDisplayPutBytes(counts[1]));
    VideoPortWriteRegisterUlong((PULONG)(bar0+0x680000),NvxDisplayPutBytes(n));
    (void)NvxNativeWaitNotifier(notifier);
    if((notifier[0]>>30)!=2) NvxNativeLogChannelState(dev);
    {
        BOOLEAN active=NvxGspVerifyDisplays(dev);
        LogStr("NVX [DISPLAY-VERIFY]: dual updateNotifier=");LogHex(notifier[0]);
        LogStr(" bothHeadsActiveAndLocked=");LogHex(active);LogStr("\n");
        dev->NativeDualActive=active && (notifier[0]>>30)==2;
        return dev->NativeDualActive;
    }
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
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2,params,sizeof(params))) {
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
    if(NvxGspControl(dev,dev->NativeDisplayHandle,NV5070_CTRL_CMD_SYSTEM_GET_CAPS_V2,caps,sizeof(caps))) {
        reply=(const ULONG *)dev->RpcRxRecord;
        dev->NativeDisplayCaps=reply[27]&255;
        LogStr("NVX [NATIVE]: display caps=");LogHex(dev->NativeDisplayCaps);LogStr("\n");
    }
    if(NvxGspControl(dev,dev->GspSubdeviceHandle,NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO,info,sizeof(info))) {
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
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS,heads,sizeof(heads))) return;
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->DisplayHeadCount=reply[28];
    if(!dev->DisplayHeadCount || dev->DisplayHeadCount>4) return;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK,mask,sizeof(mask))) return;
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->DisplayHeadMask=reply[27]&((1UL<<dev->DisplayHeadCount)-1);
    LogStr("NVX [MODEPLAN]: heads=");LogHex(dev->DisplayHeadCount);
    LogStr(" mask=");LogHex(dev->DisplayHeadMask);LogStr("\n");
    for(ULONG head=0;head<dev->DisplayHeadCount;head++) {
        ULONG active[4]={0,head,0,0};
        if(!(dev->DisplayHeadMask&(1UL<<head))) continue;
        if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE,active,sizeof(active))) return;
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
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SYSTEM_GET_HEAD_ROUTING_MAP,route,sizeof(route))) return;
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
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED,supported,sizeof(supported))) {
        LogStr("NVX [DISPLAY]: connector support query unavailable.\n");
        return;
    }
    reply=(const ULONG *)dev->RpcRxRecord;
    dev->GspSupportedDisplays=reply[27];
    LogStr("NVX [DISPLAY]: supported mask=");LogHex(dev->GspSupportedDisplays);
    LogStr(" DDC mask=");LogHex(reply[28]);LogStr("\n");
    if(!dev->GspSupportedDisplays) return;
    connected[2]=dev->GspSupportedDisplays;
    if(!NvxGspControl(dev,dev->GspDisplayHandle,NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE,connected,sizeof(connected))) {
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
            NVX_DISPLAY_TIMING tempTiming = dev->MonitorTiming[0];
            ULONG tempSor = dev->MonitorSor[0];
            ULONG tempProt = dev->MonitorProtocol[0];

            for (ULONG b = 0; b < 2048; b++) {
                UCHAR tmp = dev->MonitorEdid[0][b];
                dev->MonitorEdid[0][b] = dev->MonitorEdid[primIdx][b];
                dev->MonitorEdid[primIdx][b] = tmp;
            }

            dev->MonitorIds[0] = dev->MonitorIds[primIdx];
            dev->MonitorEdidBytes[0] = dev->MonitorEdidBytes[primIdx];
            dev->MonitorTiming[0] = dev->MonitorTiming[primIdx];
            dev->MonitorSor[0] = dev->MonitorSor[primIdx];
            dev->MonitorProtocol[0] = dev->MonitorProtocol[primIdx];

            dev->MonitorIds[primIdx] = tempId;
            dev->MonitorEdidBytes[primIdx] = tempBytes;
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
    /* Connector and EDID discovery does not depend on CE execution. */
    NvxGspDiscoverDisplays(dev);
    /* Private externally owned VASpace; page directory installed below. */
    va.flags=8;
    if(!NvxGspAlloc(dev,dev->GspDeviceHandle,0x90f10001,0x90f1,&va,sizeof(va))) return FALSE;
    dev->GspVaSpaceHandle=0x90f10001;
    LogStr("NVX [RM]: private GPU virtual address space allocated.\n");
    if(!NvxGspRegisterScratch(dev)) return FALSE;
    if(!NvxGspGetFbLayout(dev)) LogStr("NVX [FB]: static VRAM layout unavailable; retaining system-memory copy probe.\n");
#if NVX_ENABLE_GR_CHANNEL_PROBE
    if(!NvxGspAllocateGrBacking(dev))
        LogStr("NVX [GR]: continuing display bring-up without context backing.\n");
#endif
    if(!NvxGspMapScratch(dev)) return FALSE;
    if(!NvxGspSelectCopyEngine(dev)) return FALSE;
    if(!NvxGspAllocChannel(dev)) return FALSE;
    LogStr("NVX [RM]: dedicated channel and copy engine allocated; testing GPU execution.\n");
    if(!NvxGspCopySmokeTest(dev)) return FALSE;
#if NVX_ENABLE_GR_CHANNEL_PROBE
    if(dev->GspGrContextVirtual&&!NvxGspVerifyGrBacking(dev)) {
        LogStr("NVX [GR-MAP]: context mapping is unverified; GR promotion remains disabled.\n");
        if(dev->GspChannelFaulted) return FALSE;
    }
    LogStr("NVX [GR]: compile-time isolated GR probe enabled.\n");
    /* GR allocation failure is diagnostic only; preserve the known-good
       display/copy path while we validate the new engine independently. */
    if(!NvxGspCreateGraphicsProbe(dev))
        LogStr("NVX [GR]: bring-up probe incomplete; display remains enabled.\n");
#else
    LogStr("NVX [GR]: compile-time isolated GR probe disabled.\n");
#endif
    return TRUE;
}

