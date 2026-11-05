#include "nvx_vbe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static UCHAR low[0x100000];
static ULONG current, scenario, controllerCalls;
static int frees, derefs;
static BOOLEAN channelReleaseAllowed=TRUE;
static ULONG channelReleases,biosModeSets;
BOOLEAN NvxGspReleaseDisplayChannels(PVOID extension)
{
    PNVX_DEVICE_EXTENSION dev=extension;
    channelReleases++;
    if(!channelReleaseAllowed) return FALSE;
    dev->NativeDisplayAttempted=FALSE;
    dev->NativeDisplayChannelsReady=FALSE;
    dev->NativeDualActive=FALSE;
    memset(dev->NativeDisplayChannels,0,sizeof(dev->NativeDisplayChannels));
    return TRUE;
}
VOID LogStr(const char *s) { (void)s; }
VOID LogHex(unsigned long long x) { (void)x; }
VOID VideoPortZeroMemory(PVOID p,ULONG n) { memset(p,0,n); }
static VOID deref(PVOID p) { (void)p;derefs++; }
static VP_STATUS allocate(PVOID p,PUSHORT s,PUSHORT o,PULONG n)
{ (void)p;*s=0x1200;*o=0;*n=512;return 0; }
static VP_STATUS release(PVOID p,USHORT s,USHORT o)
{ (void)p;(void)s;(void)o;frees++;return 0; }
static VP_STATUS readmem(PVOID p,USHORT s,USHORT o,PVOID dst,ULONG n)
{ (void)p; assert((ULONG)s*16+o+n<=sizeof(low)); memcpy(dst,low+(ULONG)s*16+o,n);return 0; }
static VP_STATUS writemem(PVOID p,USHORT s,USHORT o,PVOID src,ULONG n)
{ (void)p;assert((ULONG)s*16+o+n<=sizeof(low));memcpy(low+(ULONG)s*16+o,src,n);return 0; }
static VP_STATUS call(PVOID p,NVX_INT10_ARGS *a)
{
    (void)p;
    void *buffer=low+(ULONG)a->SegEs*16+a->Edi;
    if(a->Eax==0x4f00) {
        VBE_INFO_BLOCK *info=buffer;
        USHORT *list=(void *)(low+0x12080);
        controllerCalls++;
        memcpy(info->VbeSignature,"VESA",4);info->VbeVersion=0x300;
        info->VideoModePtr=0x12000080; /* list inside the reused BIOS buffer */
        list[0]=0x112;list[1]=0x115;list[2]=0x118;
        if(scenario>=4) {
            list[3]=0x11b;
            if(scenario==4 && controllerCalls>1) { list[4]=0x177;list[5]=0xffff; }
            else list[4]=0xffff;
        } else list[3]=0xffff;
    } else {
        VBE_MODE_INFO *v=buffer;
        assert(a->Eax==0x4f01);
        if(scenario==6 && a->Ecx==0x177) {a->Eax=0x014f;return 0;}
        v->ModeAttributes=(scenario>=5 && a->Ecx==0x177)?0x90:0x91;v->NumberOfPlanes=1;v->MemoryModel=6;
        v->BitsPerPixel=a->Ecx==0x112?24:32;
        v->XResolution=a->Ecx==0x112?640:a->Ecx==0x115?800:1920;
        v->YResolution=a->Ecx==0x112?480:a->Ecx==0x115?600:1080;
        v->BytesPerScanLine=v->XResolution*(v->BitsPerPixel/8);
        v->LinBytesPerScanLine=a->Ecx==0x112?2048:a->Ecx==0x115?4096:8192;
        v->PhysBasePtr=0xe0000000+(a->Ecx==0x112?0:a->Ecx==0x115?0x100000:0x200000);
        v->LinRedMaskSize=v->LinGreenMaskSize=v->LinBlueMaskSize=8;
        v->LinRedFieldPosition=16;v->LinGreenFieldPosition=8;
        if(scenario==2) v->LinBytesPerScanLine=1;
    }
    a->Eax=0xffff004f;return 0;
}
VP_STATUS VideoPortQueryServices(PVOID e,ULONG type,PVOID ptr)
{
    NVX_INT10_INTERFACE *api=ptr;(void)e;
    assert(type==4 && api->Size==72 && api->Version==1);
    if(scenario==1) return 87;
    api->Dereference=deref;api->Allocate=allocate;api->Free=release;
    api->Read=readmem;api->Write=writemem;api->Call=call;return 0;
}
VP_STATUS VideoPortInt10(PVOID e,VIDEO_X86_BIOS_ARGUMENTS *a)
{
    PNVX_DEVICE_EXTENSION dev=e;
    if(a->Eax==0x4f02) {
        assert(!dev->NativeDisplayChannelsReady && !dev->NativeDisplayAttempted);
        for(ULONG i=0;i<3;i++) assert(!dev->NativeDisplayChannels[i]);
        biosModeSets++;
        if(scenario==7 && (a->Ebx&0x3fff)==0x177) {a->Eax=0x014f;return 0;}
        if(scenario!=3) current=a->Ebx&0x3fff;
    }
    else { assert(a->Eax==0x4f03);a->Ebx=current; }
    a->Eax=0xffff004f;return 0;
}
int main(void)
{
    NVX_DEVICE_EXTENSION dev;
    assert(NvxVbeSuccess(0,0xffff004f));
    assert(!NvxVbeSuccess(0,0xffff014f));
    assert(!NvxVbeSuccess(87,0x004f));
    for(scenario=0;scenario<4;scenario++) {
        memset(&dev,0,sizeof(dev));current=3;frees=derefs=0;controllerCalls=0;
        dev.PhysicalBar1.QuadPart=0xe0000000;dev.Bar1Length=0x10000000;
        assert(NvxVbeInitialize(&dev)==(scenario==0));
        if(scenario==1) { assert(!frees && !derefs && !dev.NumModes);continue; }
        assert(frees==1 && derefs==1);
        if(scenario==2) { assert(!dev.NumModes);continue; }
        if(scenario==3) continue;
        assert(dev.NumModes==3 && dev.ActiveBpp==24 && current==0x112);
        assert(dev.Modes[0].ScreenStride==2048);
        assert(dev.Modes[2].ScreenStride==8192 && dev.Modes[2].VisScreenWidth==1920);
        assert(dev.ModeFramebuffer[2].QuadPart==0xe0200000);
        assert(NvxVbeSetMode(&dev,2) && dev.ActiveWidth==1920 && dev.ActiveBpp==32);
        dev.NativeDisplayAttempted=TRUE;dev.NativeDisplayChannelsReady=TRUE;
        dev.NativeDualActive=TRUE;dev.NativeDisplayChannels[0]=0xc6700100;
        assert(NvxVbeSetMode(&dev,2));
        assert(channelReleases==1 && !dev.NativeDisplayChannelsReady && !dev.NativeDualActive);
        dev.NativeDisplayAttempted=TRUE;dev.NativeDisplayChannelsReady=TRUE;
        dev.NativeDisplayChannels[0]=0xc6700100;
        channelReleaseAllowed=FALSE;
        ULONG sets=biosModeSets;
        assert(!NvxVbeSetMode(&dev,0));
        assert(biosModeSets==sets && dev.CurrentModeIndex==2);
        channelReleaseAllowed=TRUE;
        assert(NvxVbeSetMode(&dev,2));
        assert(channelReleases==3 && !dev.NativeDisplayAttempted);
        assert(!NvxVbeSetMode(&dev,3) && dev.CurrentModeIndex==2);
        scenario=3;
        assert(!NvxVbeSetMode(&dev,0) && dev.CurrentModeIndex==2 && dev.ActiveWidth==1920);
        scenario=0;
    }
    scenario=4;memset(&dev,0,sizeof(dev));current=3;frees=derefs=0;controllerCalls=0;
    dev.PhysicalBar1.QuadPart=0xe0000000;dev.Bar1Length=0x10000000;
    assert(NvxVbeInitialize(&dev));
    assert(controllerCalls==2&&frees==2&&derefs==2&&dev.NumModes==5);
    for(scenario=5;scenario<=7;scenario++) {
        memset(&dev,0,sizeof(dev));current=3;frees=derefs=0;controllerCalls=0;
        dev.PhysicalBar1.QuadPart=0xe0000000;dev.Bar1Length=0x10000000;
        assert(NvxVbeInitialize(&dev));
        assert(controllerCalls==2 && frees==2 && derefs==2);
        assert(dev.NumModes==(scenario==5?5U:4U));
        if(scenario==5) assert(dev.ModeVbeIds[4]==0x177 && dev.Modes[4].VisScreenWidth==1920);
    }
    puts("VBE tests passed: modes, format/bounds, native-channel teardown before BIOS reset and blocked reset on teardown failure.");
    return 0;
}
