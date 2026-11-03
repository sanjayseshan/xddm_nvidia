/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * Screen modes, PDEV initialization, Palette and Surface management.
 */

#include "driver.h"

#define DEVICE_NAME L"nvxdisp"

static LOGFONTW SystemFont = { 16, 7, 0, 0, 700, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"System" };
static LOGFONTW AnsiVariableFont = { 12, 9, 0, 0, 400, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_STROKE_PRECIS, PROOF_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"MS Sans Serif" };
static LOGFONTW AnsiFixedFont = { 12, 9, 0, 0, 400, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_STROKE_PRECIS, PROOF_QUALITY, FIXED_PITCH | FF_DONTCARE, L"Courier" };

const PALETTEENTRY BASEPALETTE[20] =
{
    { 0x00, 0x00, 0x00, 0x00 },
    { 0x80, 0x00, 0x00, 0x00 },
    { 0x00, 0x80, 0x00, 0x00 },
    { 0x80, 0x80, 0x00, 0x00 },
    { 0x00, 0x00, 0x80, 0x00 },
    { 0x80, 0x00, 0x80, 0x00 },
    { 0x00, 0x80, 0x80, 0x00 },
    { 0xC0, 0xC0, 0xC0, 0x00 },
    { 0xC0, 0xDC, 0xC0, 0x00 },
    { 0xD4, 0xD0, 0xC8, 0x00 },
    { 0xFF, 0xFB, 0xF0, 0x00 },
    { 0x3A, 0x6E, 0xA5, 0x00 },
    { 0x80, 0x80, 0x80, 0x00 },
    { 0xFF, 0x00, 0x00, 0x00 },
    { 0x00, 0xFF, 0x00, 0x00 },
    { 0xFF, 0xFF, 0x00, 0x00 },
    { 0x00, 0x00, 0xFF, 0x00 },
    { 0xFF, 0x00, 0xFF, 0x00 },
    { 0x00, 0xFF, 0xFF, 0x00 },
    { 0xFF, 0xFF, 0xFF, 0x00 },
};

static DWORD
GetAvailableModes(
    HANDLE hDriver,
    PVIDEO_MODE_INFORMATION *ModeInfo,
    DWORD *ModeInfoSize)
{
    ULONG ulTemp;
    VIDEO_NUM_MODES Modes;
    PVIDEO_MODE_INFORMATION ModeInfoPtr;

    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL,
                           0, &Modes, sizeof(VIDEO_NUM_MODES), &ulTemp))
    {
        return 0;
    }

    if (Modes.NumModes == 0)
        return 0;

    *ModeInfoSize = Modes.ModeInformationLength;
    *ModeInfo = (PVIDEO_MODE_INFORMATION)EngAllocMem(0, Modes.NumModes * Modes.ModeInformationLength, ALLOC_TAG);
    if (!*ModeInfo)
        return 0;

    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0,
                           *ModeInfo, Modes.NumModes * Modes.ModeInformationLength,
                           &ulTemp))
    {
        EngFreeMem(*ModeInfo);
        *ModeInfo = NULL;
        return 0;
    }

    ulTemp = Modes.NumModes;
    ModeInfoPtr = *ModeInfo;

    while (ulTemp--)
    {
        if ((ModeInfoPtr->NumberOfPlanes != 1) ||
            !(ModeInfoPtr->AttributeFlags & VIDEO_MODE_GRAPHICS) ||
            ((ModeInfoPtr->BitsPerPlane != 8) &&
             (ModeInfoPtr->BitsPerPlane != 16) &&
             (ModeInfoPtr->BitsPerPlane != 24) &&
             (ModeInfoPtr->BitsPerPlane != 32)))
        {
            ModeInfoPtr->Length = 0;
        }

        ModeInfoPtr = (PVIDEO_MODE_INFORMATION)(((PUCHAR)ModeInfoPtr) + Modes.ModeInformationLength);
    }

    return Modes.NumModes;
}

ULONG APIENTRY
DrvGetModes(
    IN HANDLE hDriver,
    IN ULONG cjSize,
    OUT DEVMODEW *pdm)
{
    ULONG ModeCount;
    ULONG ModeInfoSize;
    PVIDEO_MODE_INFORMATION ModeInfo, ModeInfoPtr;
    ULONG OutputSize;

    ModeCount = GetAvailableModes(hDriver, &ModeInfo, &ModeInfoSize);
    if (ModeCount == 0)
        return 0;

    if (!pdm)
    {
        EngFreeMem(ModeInfo);
        return ModeCount * sizeof(DEVMODEW);
    }

    OutputSize = 0;
    ModeInfoPtr = ModeInfo;

    while (ModeCount-- > 0)
    {
        if (ModeInfoPtr->Length == 0)
        {
            ModeInfoPtr = (PVIDEO_MODE_INFORMATION)(((ULONG_PTR)ModeInfoPtr) + ModeInfoSize);
            continue;
        }

        memset(pdm, 0, sizeof(DEVMODEW));
        memcpy(pdm->dmDeviceName, DEVICE_NAME, sizeof(DEVICE_NAME));
        pdm->dmSpecVersion = DM_SPECVERSION;
        pdm->dmDriverVersion = DM_SPECVERSION;
        pdm->dmSize = sizeof(DEVMODEW);
        pdm->dmDriverExtra = 0;
        pdm->dmBitsPerPel = ModeInfoPtr->NumberOfPlanes * ModeInfoPtr->BitsPerPlane;
        pdm->dmPelsWidth = ModeInfoPtr->VisScreenWidth;
        pdm->dmPelsHeight = ModeInfoPtr->VisScreenHeight;
        pdm->dmDisplayFrequency = ModeInfoPtr->Frequency;
        pdm->dmDisplayFlags = 0;
        pdm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                        DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;

        ModeInfoPtr = (PVIDEO_MODE_INFORMATION)(((ULONG_PTR)ModeInfoPtr) + ModeInfoSize);
        pdm = (LPDEVMODEW)(((ULONG_PTR)pdm) + sizeof(DEVMODEW));
        OutputSize += sizeof(DEVMODEW);
    }

    EngFreeMem(ModeInfo);
    return OutputSize;
}

static BOOL
InitPalette(PPDEV ppdev, PDEVINFO pDevInfo)
{
    ULONG ColorLoop;
    PALETTEENTRY *PaletteEntryPtr;

    if (ppdev->BitsPerPixel > 8)
    {
        ppdev->DefaultPalette = pDevInfo->hpalDefault =
            EngCreatePalette(PAL_BITFIELDS, 0, NULL,
                             ppdev->RedMask, ppdev->GreenMask, ppdev->BlueMask);
    }
    else
    {
        ppdev->PaletteEntries = (PALETTEENTRY *)EngAllocMem(0, sizeof(PALETTEENTRY) * 256, ALLOC_TAG);
        if (!ppdev->PaletteEntries)
            return FALSE;

        for (ColorLoop = 256, PaletteEntryPtr = ppdev->PaletteEntries;
             ColorLoop != 0;
             ColorLoop--, PaletteEntryPtr++)
        {
            PaletteEntryPtr->peRed   = ((ColorLoop >> 5) & 7) * 255 / 7;
            PaletteEntryPtr->peGreen = ((ColorLoop >> 3) & 3) * 255 / 3;
            PaletteEntryPtr->peBlue  = (ColorLoop & 7) * 255 / 7;
            PaletteEntryPtr->peFlags = 0;
        }

        memcpy(ppdev->PaletteEntries, BASEPALETTE, 10 * sizeof(PALETTEENTRY));
        memcpy(ppdev->PaletteEntries + 246, BASEPALETTE + 10, 10 * sizeof(PALETTEENTRY));

        ppdev->DefaultPalette = pDevInfo->hpalDefault =
            EngCreatePalette(PAL_INDEXED, 256, (PULONG)ppdev->PaletteEntries, 0, 0, 0);
    }

    return (ppdev->DefaultPalette != NULL);
}

DHPDEV APIENTRY
DrvEnablePDEV(
    DEVMODEW *pdm,
    LPWSTR pwszLogAddress,
    ULONG cPat,
    HSURF *phsurfPatterns,
    ULONG cjCaps,
    ULONG *pdevcaps,
    ULONG cjDevInfo,
    DEVINFO *pdi,
    HDEV hdev,
    LPWSTR pwszDeviceName,
    HANDLE hDriver)
{
    PPDEV ppdev;
    ULONG ModeCount;
    ULONG ModeInfoSize;
    PVIDEO_MODE_INFORMATION ModeInfo, ModeInfoPtr, SelectedMode = NULL;
    PGDIINFO pGdiInfo = (PGDIINFO)pdevcaps;

    ppdev = (PPDEV)EngAllocMem(FL_ZERO_MEMORY, sizeof(PDEV), ALLOC_TAG);
    if (!ppdev)
        return NULL;

    ppdev->hDriver = hDriver;
    ppdev->ScreenGpuVaBase = 0; /* Queried after the adapter is mapped. */

    ModeCount = GetAvailableModes(hDriver, &ModeInfo, &ModeInfoSize);
    if (ModeCount == 0)
    {
        EngFreeMem(ppdev);
        return NULL;
    }

    /* Recover a saved low-resolution startup choice when this board has a
       BIOS-validated 1080p format. The user requested a 1080p primary. */
    if(pdm->dmPelsWidth<=800 && pdm->dmPelsHeight<=600) {
        PVIDEO_MODE_INFORMATION candidate=ModeInfo;
        for(ULONG i=0;i<ModeCount;i++) {
            if(candidate->Length && candidate->VisScreenWidth==1920 &&
               candidate->VisScreenHeight==1080 && candidate->BitsPerPlane==32)
                SelectedMode=candidate;
            candidate=(PVIDEO_MODE_INFORMATION)((PUCHAR)candidate+ModeInfoSize);
        }
    }
    if(SelectedMode) {
        /* Keep recovered native primary selection. */
    } else
    if (pdm->dmPelsWidth == 0 && pdm->dmPelsHeight == 0 &&
        pdm->dmBitsPerPel == 0 && pdm->dmDisplayFrequency == 0)
    {
        ModeInfoPtr = ModeInfo;
        while (ModeCount-- > 0)
        {
            if (ModeInfoPtr->Length == 0)
            {
                ModeInfoPtr = (PVIDEO_MODE_INFORMATION)(((PUCHAR)ModeInfoPtr) + ModeInfoSize);
                continue;
            }
            SelectedMode = ModeInfoPtr;
            break;
        }
    }
    else
    {
        ModeInfoPtr = ModeInfo;
        while (ModeCount-- > 0)
        {
            if (ModeInfoPtr->Length > 0 &&
                pdm->dmPelsWidth == ModeInfoPtr->VisScreenWidth &&
                pdm->dmPelsHeight == ModeInfoPtr->VisScreenHeight &&
                pdm->dmBitsPerPel == (ModeInfoPtr->BitsPerPlane * ModeInfoPtr->NumberOfPlanes) &&
                pdm->dmDisplayFrequency == ModeInfoPtr->Frequency)
            {
                SelectedMode = ModeInfoPtr;
                break;
            }

            ModeInfoPtr = (PVIDEO_MODE_INFORMATION)(((PUCHAR)ModeInfoPtr) + ModeInfoSize);
        }
    }

    if (!SelectedMode)
    {
        EngFreeMem(ModeInfo);
        EngFreeMem(ppdev);
        return NULL;
    }

    ppdev->ModeIndex    = SelectedMode->ModeIndex;
    ppdev->ScreenWidth  = SelectedMode->VisScreenWidth;
    ppdev->ScreenHeight = SelectedMode->VisScreenHeight;
    ppdev->ScreenDelta  = SelectedMode->ScreenStride;
    ppdev->BitsPerPixel = (BYTE)(SelectedMode->BitsPerPlane * SelectedMode->NumberOfPlanes);
    ppdev->RedMask      = SelectedMode->RedMask;
    ppdev->GreenMask    = SelectedMode->GreenMask;
    ppdev->BlueMask     = SelectedMode->BlueMask;

    pGdiInfo->ulVersion         = GDI_DRIVER_VERSION;
    pGdiInfo->ulTechnology      = DT_RASDISPLAY;
    pGdiInfo->ulHorzSize        = SelectedMode->XMillimeter;
    pGdiInfo->ulVertSize        = SelectedMode->YMillimeter;
    pGdiInfo->ulHorzRes         = SelectedMode->VisScreenWidth;
    pGdiInfo->ulVertRes         = SelectedMode->VisScreenHeight;
    pGdiInfo->ulPanningHorzRes  = SelectedMode->VisScreenWidth;
    pGdiInfo->ulPanningVertRes  = SelectedMode->VisScreenHeight;
    pGdiInfo->cBitsPixel        = SelectedMode->BitsPerPlane;
    pGdiInfo->cPlanes           = SelectedMode->NumberOfPlanes;
    pGdiInfo->ulVRefresh        = SelectedMode->Frequency;
    pGdiInfo->ulBltAlignment    = 1;
    pGdiInfo->ulLogPixelsX      = pdm->dmLogPixels;
    pGdiInfo->ulLogPixelsY      = pdm->dmLogPixels;
    pGdiInfo->flTextCaps        = TC_RA_ABLE;
    pGdiInfo->flRaster          = 0;
    pGdiInfo->ulDACRed          = SelectedMode->NumberRedBits;
    pGdiInfo->ulDACGreen        = SelectedMode->NumberGreenBits;
    pGdiInfo->ulDACBlue         = SelectedMode->NumberBlueBits;
    pGdiInfo->ulAspectX         = 0x24;
    pGdiInfo->ulAspectY         = 0x24;
    pGdiInfo->ulAspectXY        = 0x33;
    pGdiInfo->xStyleStep        = 1;
    pGdiInfo->yStyleStep        = 1;
    pGdiInfo->denStyleStep      = 3;
    pGdiInfo->ptlPhysOffset.x   = 0;
    pGdiInfo->ptlPhysOffset.y   = 0;
    pGdiInfo->szlPhysSize.cx    = 0;
    pGdiInfo->szlPhysSize.cy    = 0;

    pGdiInfo->ciDevice.Red.x            = 6700;
    pGdiInfo->ciDevice.Red.y            = 3300;
    pGdiInfo->ciDevice.Green.x          = 2100;
    pGdiInfo->ciDevice.Green.y          = 7100;
    pGdiInfo->ciDevice.Blue.x           = 1400;
    pGdiInfo->ciDevice.Blue.y           = 800;
    pGdiInfo->ciDevice.AlignmentWhite.x = 3127;
    pGdiInfo->ciDevice.AlignmentWhite.y = 3290;
    pGdiInfo->ciDevice.AlignmentWhite.Y = 0;
    pGdiInfo->ciDevice.RedGamma         = 20000;
    pGdiInfo->ciDevice.GreenGamma       = 20000;
    pGdiInfo->ciDevice.BlueGamma        = 20000;
    pGdiInfo->ciDevice.Red.Y            = 0;
    pGdiInfo->ciDevice.Green.Y          = 0;
    pGdiInfo->ciDevice.Blue.Y           = 0;
    pGdiInfo->ciDevice.Cyan.x           = 0;
    pGdiInfo->ciDevice.Cyan.y           = 0;
    pGdiInfo->ciDevice.Cyan.Y           = 0;
    pGdiInfo->ciDevice.Magenta.x        = 0;
    pGdiInfo->ciDevice.Magenta.y        = 0;
    pGdiInfo->ciDevice.Magenta.Y        = 0;
    pGdiInfo->ciDevice.Yellow.x         = 0;
    pGdiInfo->ciDevice.Yellow.y         = 0;
    pGdiInfo->ciDevice.Yellow.Y         = 0;
    pGdiInfo->ciDevice.MagentaInCyanDye = 0;
    pGdiInfo->ciDevice.YellowInCyanDye  = 0;
    pGdiInfo->ciDevice.CyanInMagentaDye = 0;
    pGdiInfo->ciDevice.YellowInMagentaDye = 0;
    pGdiInfo->ciDevice.CyanInYellowDye  = 0;
    pGdiInfo->ciDevice.MagentaInYellowDye = 0;
    pGdiInfo->ulDevicePelsDPI           = 0;
    pGdiInfo->ulPrimaryOrder            = PRIMARY_ORDER_CBA;
    pGdiInfo->ulHTPatternSize           = HT_PATSIZE_4x4_M;
    pGdiInfo->flHTFlags                 = HT_FLAG_ADDITIVE_PRIMS;

    pdi->flGraphicsCaps  = GCAPS_OPAQUERECT | GCAPS_MONO_DITHER;
    pdi->lfDefaultFont   = SystemFont;
    pdi->lfAnsiVarFont   = AnsiVariableFont;
    pdi->lfAnsiFixFont   = AnsiFixedFont;
    pdi->cFonts          = 0;
    pdi->cxDither        = 0;
    pdi->cyDither        = 0;
    pdi->hpalDefault     = 0;
    pdi->flGraphicsCaps2 = 0;

    if (ppdev->BitsPerPixel == 8)
    {
        pGdiInfo->ulNumColors      = 20;
        pGdiInfo->ulNumPalReg      = 256;
        pGdiInfo->ulHTOutputFormat = HT_FORMAT_8BPP;
        pdi->flGraphicsCaps       |= GCAPS_PALMANAGED;
        pdi->iDitherFormat         = BMF_8BPP;
    }
    else
    {
        pGdiInfo->ulNumColors = (ULONG)-1;
        pGdiInfo->ulNumPalReg = 0;
        switch (ppdev->BitsPerPixel)
        {
            case 16:
                pGdiInfo->ulHTOutputFormat = HT_FORMAT_16BPP;
                pdi->iDitherFormat         = BMF_16BPP;
                break;
            case 24:
                pGdiInfo->ulHTOutputFormat = HT_FORMAT_24BPP;
                pdi->iDitherFormat         = BMF_24BPP;
                break;
            default:
                pGdiInfo->ulHTOutputFormat = HT_FORMAT_32BPP;
                pdi->iDitherFormat         = BMF_32BPP;
                break;
        }
    }

    if (!InitPalette(ppdev, pdi))
    {
        EngFreeMem(ModeInfo);
        EngFreeMem(ppdev);
        return NULL;
    }

    EngFreeMem(ModeInfo);
    return (DHPDEV)ppdev;
}

VOID APIENTRY
DrvCompletePDEV(
    DHPDEV dhpdev,
    HDEV hdev)
{
    ((PPDEV)dhpdev)->hDevEng = hdev;
}

VOID APIENTRY
DrvDisablePDEV(
    DHPDEV dhpdev)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    if (ppdev->DefaultPalette)
        EngDeletePalette(ppdev->DefaultPalette);
    if (ppdev->PaletteEntries)
        EngFreeMem(ppdev->PaletteEntries);
    EngFreeMem(ppdev);
}

HSURF APIENTRY
DrvEnableSurface(
    IN DHPDEV dhpdev)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    HSURF hSurface;
    ULONG BitmapType;
    SIZEL ScreenSize;
    VIDEO_MEMORY VideoMemory;
    VIDEO_MEMORY_INFORMATION VideoMemoryInfo;
    ULONG ulTemp;
    FLONG flHooks;

    if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                           &(ppdev->ModeIndex), sizeof(ULONG), NULL, 0,
                           &ulTemp))
    {
        return NULL;
    }

    VideoMemory.RequestedVirtualAddress = NULL;
    if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY,
                           &VideoMemory, sizeof(VIDEO_MEMORY),
                           &VideoMemoryInfo, sizeof(VIDEO_MEMORY_INFORMATION),
                           &ulTemp))
    {
        return NULL;
    }

    if(EngDeviceIoControl(ppdev->hDriver,IOCTL_NVX_SCANOUT_GPU_VA,NULL,0,
                          &ppdev->ScreenGpuVaBase,sizeof(ppdev->ScreenGpuVaBase),&ulTemp)
       || ulTemp!=sizeof(ppdev->ScreenGpuVaBase) || !ppdev->ScreenGpuVaBase) {
        DrvDisableSurface(dhpdev);
        return NULL;
    }
    ppdev->MappedFramebuffer = VideoMemoryInfo.FrameBufferBase;
    /* BAR1 readback is unreliable after GSP initialization. Keep authoritative
       GDI pixels in ordinary system memory and present them through CE DMA. */
    if (!ppdev->ScreenDelta || ppdev->ScreenHeight > (~0UL / ppdev->ScreenDelta))
    {
        DrvDisableSurface(dhpdev);
        return NULL;
    }
    ppdev->ScreenPtr = EngAllocMem(0, ppdev->ScreenDelta * ppdev->ScreenHeight, ALLOC_TAG);
    if (!ppdev->ScreenPtr) {
        DrvDisableSurface(dhpdev);
        return NULL;
    }
    memset(ppdev->ScreenPtr, 0, ppdev->ScreenDelta * ppdev->ScreenHeight);
    ppdev->PresentedPixels=EngAllocMem(FL_ZERO_MEMORY,
        ppdev->ScreenDelta * ppdev->ScreenHeight,ALLOC_TAG);
    if(!ppdev->PresentedPixels) {
        DrvDisableSurface(dhpdev);
        return NULL;
    }
    ppdev->PresentationValid=FALSE;

    switch (ppdev->BitsPerPixel)
    {
        case 8:  BitmapType = BMF_8BPP; break;
        case 16: BitmapType = BMF_16BPP; break;
        case 24: BitmapType = BMF_24BPP; break;
        case 32: BitmapType = BMF_32BPP; break;
        default: DrvDisableSurface(dhpdev); return NULL;
    }

    ppdev->iDitherFormat = BitmapType;
    ScreenSize.cx = ppdev->ScreenWidth;
    ScreenSize.cy = ppdev->ScreenHeight;

    /* Create the primary GDI bitmap on coherent system memory. */
    hSurface = (HSURF)EngCreateBitmap(ScreenSize, ppdev->ScreenDelta, BitmapType,
                                      (ppdev->ScreenDelta > 0) ? BMF_TOPDOWN : 0,
                                      ppdev->ScreenPtr);
    if (!hSurface) {
        DrvDisableSurface(dhpdev);
        return NULL;
    }

    /* Keep the GDI hooks: on v109 the unhooked primary surface stayed black. */
    flHooks = HOOK_BITBLT | HOOK_COPYBITS | HOOK_TEXTOUT | HOOK_SYNCHRONIZE;

    if (!EngAssociateSurface(hSurface, ppdev->hDevEng, flHooks))
    {
        EngDeleteSurface(hSurface);
        DrvDisableSurface(dhpdev);
        return NULL;
    }

    ppdev->hSurfEng = hSurface;
    return hSurface;
}

VOID APIENTRY
DrvDisableSurface(
    IN DHPDEV dhpdev)
{
    DWORD ulTemp;
    VIDEO_MEMORY VideoMemory;
    PPDEV ppdev = (PPDEV)dhpdev;

    if (ppdev->hSurfEng)
    {
        EngDeleteSurface(ppdev->hSurfEng);
        ppdev->hSurfEng = NULL;
    }

    VideoMemory.RequestedVirtualAddress = ppdev->MappedFramebuffer;
    EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
                       &VideoMemory, sizeof(VIDEO_MEMORY), NULL, 0, &ulTemp);
    if (ppdev->ScreenPtr) EngFreeMem(ppdev->ScreenPtr);
    if (ppdev->PresentedPixels) EngFreeMem(ppdev->PresentedPixels);
    ppdev->PresentedPixels=NULL;
    ppdev->PresentationValid=FALSE;
    ppdev->ScreenPtr = NULL;
    ppdev->MappedFramebuffer = NULL;
}

BOOL APIENTRY
DrvAssertMode(
    IN DHPDEV dhpdev,
    IN BOOL bEnable)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    ULONG ulTemp;

    ppdev->PresentationValid=FALSE;
    if (bEnable)
    {
        return !EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                                   &(ppdev->ModeIndex), sizeof(ULONG), NULL, 0, &ulTemp);
    }
    else
    {
        return !EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_RESET_DEVICE,
                                   NULL, 0, NULL, 0, &ulTemp);
    }
}

BOOL APIENTRY
DrvSetPalette(
    IN DHPDEV dhpdev,
    IN PALOBJ *ppalo,
    IN ULONG fl,
    IN ULONG iStart,
    IN ULONG cColors)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    PVIDEO_CLUT pScreenClut;
    ULONG ulTemp;
    BOOL bResult;

    pScreenClut = (PVIDEO_CLUT)EngAllocMem(0, sizeof(VIDEO_CLUT) + (sizeof(ULONG) * 256), ALLOC_TAG);
    if (!pScreenClut)
        return FALSE;

    pScreenClut->FirstEntry = (USHORT)iStart;
    pScreenClut->NumEntries = (USHORT)cColors;

    if (ppalo)
    {
        PALOBJ_cGetColors(ppalo, iStart, cColors, (PULONG)pScreenClut->LookupTable);
    }
    else
    {
        memcpy(pScreenClut->LookupTable, ppdev->PaletteEntries + iStart, cColors * sizeof(PALETTEENTRY));
    }

    bResult = !EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_COLOR_REGISTERS,
                                  pScreenClut, sizeof(VIDEO_CLUT) + (sizeof(ULONG) * cColors),
                                  NULL, 0, &ulTemp);

    EngFreeMem(pScreenClut);
    return bResult;
}

ULONG APIENTRY
DrvDitherColor(
    IN DHPDEV dhpdev,
    IN ULONG iMode,
    IN ULONG rgb,
    OUT ULONG *pul)
{
    return DCR_HALFTONE;
}

VOID APIENTRY
DrvSynchronize(
    IN DHPDEV dhpdev,
    IN RECTL *prcl)
{
}

ULONG APIENTRY
DrvSetPointerShape(
    SURFOBJ *pso,
    SURFOBJ *psoMask,
    SURFOBJ *psoColor,
    XLATEOBJ *pxlo,
    LONG xHot,
    LONG yHot,
    LONG x,
    LONG y,
    RECTL *prcl,
    FLONG fl)
{
    ULONG result=EngSetPointerShape(pso,psoMask,psoColor,pxlo,xHot,yHot,x,y,prcl,fl);
    if(pso && pso->dhpdev) {
        PPDEV ppdev=(PPDEV)pso->dhpdev;
        RECTL full={0,0,(LONG)ppdev->ScreenWidth,(LONG)ppdev->ScreenHeight};
        (void)NvxUploadSurfaceRect(ppdev,pso,&full,NVX_UPLOAD_KIND_COPYBITS);
    }
    return result;
}

VOID APIENTRY
DrvMovePointer(
    SURFOBJ *pso,
    LONG x,
    LONG y,
    RECTL *prcl)
{
    EngMovePointer(pso,x,y,prcl);
    if(pso && pso->dhpdev) {
        PPDEV ppdev=(PPDEV)pso->dhpdev;
        RECTL full={0,0,(LONG)ppdev->ScreenWidth,(LONG)ppdev->ScreenHeight};
        (void)NvxUploadSurfaceRect(ppdev,pso,&full,NVX_UPLOAD_KIND_COPYBITS);
    }
}
