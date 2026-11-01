/* VBE fallback: publish the BIOS's actual format, stride and physical base.
 * No RM or display-engine register programming is performed here. */
#include "nvx_vbe.h"
#include <stddef.h>

_Static_assert(sizeof(NVX_INT10_ARGS) == 32, "INT10 register ABI");
_Static_assert(sizeof(NVX_INT10_INTERFACE) == 72, "INT10 service ABI");
_Static_assert(sizeof(VBE_INFO_BLOCK) == 512, "VBE controller ABI");
_Static_assert(sizeof(VBE_MODE_INFO) == 256, "VBE mode ABI");

BOOLEAN NvxVbeSuccess(VP_STATUS status, ULONG eax)
{
    /* VBE specifies AX, not the undefined high half of EAX. */
    return status == NO_ERROR && (eax & 0xffff) == 0x004f;
}

static BOOLEAN AddMode(PNVX_DEVICE_EXTENSION dev, ULONG id, const VBE_MODE_INFO *v,
                       USHORT version)
{
    PVIDEO_MODE_INFORMATION m;
    ULONG stride = v->BytesPerScanLine;
    ULONG rb=v->RedMaskSize, gb=v->GreenMaskSize, bb=v->BlueMaskSize;
    ULONG rp=v->RedFieldPosition, gp=v->GreenFieldPosition, bp=v->BlueFieldPosition;
    ULONG64 bytes, base=v->PhysBasePtr;
    ULONG i;
    if (version >= 0x300 && v->LinBytesPerScanLine) {
        stride = v->LinBytesPerScanLine;
        rb=v->LinRedMaskSize;gb=v->LinGreenMaskSize;bb=v->LinBlueMaskSize;
        rp=v->LinRedFieldPosition;gp=v->LinGreenFieldPosition;bp=v->LinBlueFieldPosition;
    }
    if ((v->ModeAttributes & 0x91) != 0x91 || v->MemoryModel != 6 ||
        v->NumberOfPlanes != 1 || v->XResolution < 640 || v->YResolution < 480 ||
        (v->BitsPerPixel != 16 && v->BitsPerPixel != 24 && v->BitsPerPixel != 32) ||
        stride < (ULONG)v->XResolution * (v->BitsPerPixel / 8) ||
        !rb || !gb || !bb || rb>8 || gb>8 || bb>8 ||
        rp+rb>v->BitsPerPixel || gp+gb>v->BitsPerPixel || bp+bb>v->BitsPerPixel)
        return FALSE;
    bytes=(ULONG64)stride*v->YResolution;
    if (!base || base<(ULONG64)dev->PhysicalBar1.QuadPart ||
        base+bytes>(ULONG64)dev->PhysicalBar1.QuadPart+dev->Bar1Length) return FALSE;
    for(i=0;i<dev->NumModes;i++) if(dev->ModeVbeIds[i]==id) return TRUE;
    if (dev->NumModes==MAX_NVX_MODES) return FALSE;
    i=dev->NumModes++;
    m=&dev->Modes[i];
    VideoPortZeroMemory(m,sizeof(*m));
    m->Length=sizeof(*m);m->ModeIndex=i;
    m->VisScreenWidth=v->XResolution;m->VisScreenHeight=v->YResolution;
    m->ScreenStride=stride;m->NumberOfPlanes=1;m->BitsPerPlane=v->BitsPerPixel;
    m->Frequency=60; /* VBE's default refresh, no custom CRTC requested. */
    m->NumberRedBits=rb;m->NumberGreenBits=gb;m->NumberBlueBits=bb;
    m->RedMask=((1UL<<rb)-1)<<rp;m->GreenMask=((1UL<<gb)-1)<<gp;
    m->BlueMask=((1UL<<bb)-1)<<bp;
    m->AttributeFlags=VIDEO_MODE_COLOR|VIDEO_MODE_GRAPHICS;
    m->VideoMemoryBitmapWidth=stride/(v->BitsPerPixel/8);
    m->VideoMemoryBitmapHeight=v->YResolution;
    dev->ModeVbeIds[i]=id;
    dev->ModeFramebuffer[i].QuadPart=base;
    LogStr("NVX [VBE]: Mode ");LogHex(id);LogStr(" ");LogHex(v->XResolution);
    LogStr("x");LogHex(v->YResolution);LogStr(" bpp ");LogHex(v->BitsPerPixel);
    LogStr(" stride ");LogHex(stride);LogStr(" physical ");LogHex(base);LogStr("\n");
    return TRUE;
}

BOOLEAN NvxVbeSetMode(PVOID extension, ULONG index)
{
    PNVX_DEVICE_EXTENSION dev=extension;
    VIDEO_X86_BIOS_ARGUMENTS a;
    VP_STATUS status;
    ULONG id;
    if(index>=dev->NumModes) return FALSE;
    id=dev->ModeVbeIds[index];
    VideoPortZeroMemory(&a,sizeof(a));a.Eax=0x4f02;a.Ebx=id|0x4000;
    status=VideoPortInt10(extension,&a);
    LogStr("NVX [VBE]: SET_MODE ");LogHex(id);LogStr(" status ");LogHex(status);
    LogStr(" EAX ");LogHex(a.Eax);LogStr("\n");
    if(!NvxVbeSuccess(status,a.Eax)) return FALSE;
    VideoPortZeroMemory(&a,sizeof(a));a.Eax=0x4f03;
    status=VideoPortInt10(extension,&a);
    LogStr("NVX [VBE]: CURRENT_MODE EAX ");LogHex(a.Eax);LogStr(" BX ");LogHex(a.Ebx);LogStr("\n");
    if(!NvxVbeSuccess(status,a.Eax) || (a.Ebx&0x3fff)!=id) return FALSE;
    dev->CurrentModeIndex=index;
    dev->ActiveWidth=dev->Modes[index].VisScreenWidth;
    dev->ActiveHeight=dev->Modes[index].VisScreenHeight;
    dev->ActiveBpp=dev->Modes[index].BitsPerPlane;
    dev->ActiveFreq=dev->Modes[index].Frequency;
    return TRUE;
}

BOOLEAN NvxVbeInitialize(PVOID extension)
{
    PNVX_DEVICE_EXTENSION dev=extension;
    NVX_INT10_INTERFACE api;
    NVX_INT10_ARGS a;
    VBE_INFO_BLOCK info;
    VBE_MODE_INFO mode;
    USHORT seg=0,off=0,list[512];
    ULONG size=512,i,count=0,initial=0;
    VP_STATUS status;
    BOOLEAN allocated=FALSE;
    dev->NumModes=0;
    VideoPortZeroMemory(&api,sizeof(api));api.Size=sizeof(api);api.Version=1;
    status=VideoPortQueryServices(extension,4,&api); /* VideoPortServicesInt10 */
    LogStr("NVX [VBE]: INT10 service status ");LogHex(status);LogStr("\n");
    if(status!=NO_ERROR) return FALSE;
    if(!api.Allocate || !api.Free || !api.Read || !api.Write || !api.Call) goto done;
    if(api.Allocate(api.Context,&seg,&off,&size)!=NO_ERROR) goto done;
    allocated=TRUE;
    if(size<512) goto done;
    VideoPortZeroMemory(&info,sizeof(info));
    info.VbeSignature[0]='V';info.VbeSignature[1]='B';info.VbeSignature[2]='E';info.VbeSignature[3]='2';
    if(api.Write(api.Context,seg,off,&info,sizeof(info))!=NO_ERROR) goto done;
    VideoPortZeroMemory(&a,sizeof(a));a.Eax=0x4f00;a.SegEs=seg;a.Edi=off;
    status=api.Call(api.Context,&a);
    LogStr("NVX [VBE]: CONTROLLER EAX ");LogHex(a.Eax);LogStr("\n");
    if(!NvxVbeSuccess(status,a.Eax) || api.Read(api.Context,seg,off,&info,sizeof(info))!=NO_ERROR ||
       info.VbeSignature[0]!='V' || info.VbeSignature[1]!='E' ||
       info.VbeSignature[2]!='S' || info.VbeSignature[3]!='A' || info.VbeVersion<0x200) goto done;
    /* Copy the list before reusing the BIOS buffer; BIOS may place it there. */
    for(i=0;i<512;i++) {
        ULONG linear=((info.VideoModePtr>>16)<<4)+(info.VideoModePtr&0xffff)+i*2;
        if(linear>0xffffe || api.Read(api.Context,(USHORT)(linear>>4),(USHORT)(linear&15),&list[i],2)!=NO_ERROR) goto done;
        if(list[i]==0xffff) { count=i;break; }
    }
    if(!count) goto done;
    for(i=0;i<count && dev->NumModes<MAX_NVX_MODES;i++) {
        VideoPortZeroMemory(&mode,sizeof(mode));
        if(api.Write(api.Context,seg,off,&mode,sizeof(mode))!=NO_ERROR) break;
        VideoPortZeroMemory(&a,sizeof(a));a.Eax=0x4f01;a.Ecx=list[i];a.SegEs=seg;a.Edi=off;
        status=api.Call(api.Context,&a);
        if(NvxVbeSuccess(status,a.Eax) && api.Read(api.Context,seg,off,&mode,sizeof(mode))==NO_ERROR)
            AddMode(dev,list[i],&mode,info.VbeVersion);
    }
done:
    if(allocated) api.Free(api.Context,seg,off);
    if(api.Dereference) api.Dereference(api.Context);
    if(!dev->NumModes) {
        LogStr("NVX [VBE]: No verified linear modes; refusing guessed framebuffer geometry.\n");
        return FALSE;
    }
    for(i=1;i<dev->NumModes;i++) {
        const VIDEO_MODE_INFORMATION *m=&dev->Modes[i], *b=&dev->Modes[initial];
        ULONG64 area=(ULONG64)m->VisScreenWidth*m->VisScreenHeight;
        ULONG64 best=(ULONG64)b->VisScreenWidth*b->VisScreenHeight;
        if(area<best || (area==best && m->BitsPerPlane>b->BitsPerPlane)) initial=i;
    }
    return NvxVbeSetMode(extension,initial);
}
