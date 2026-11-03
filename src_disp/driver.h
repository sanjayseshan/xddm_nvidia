#ifndef _NVX_DISP_DRIVER_H_
#define _NVX_DISP_DRIVER_H_

#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>

/* Fake ddraw structs to satisfy winddi.h */
typedef struct _DD_SURFACE_LOCAL DD_SURFACE_LOCAL, *PDD_SURFACE_LOCAL;
typedef struct _DD_DIRECTDRAW_GLOBAL DD_DIRECTDRAW_GLOBAL, *PDD_DIRECTDRAW_GLOBAL;
typedef struct _DD_CALLBACKS DD_CALLBACKS;
typedef struct _DD_SURFACECALLBACKS DD_SURFACECALLBACKS;
typedef struct _DD_PALETTECALLBACKS DD_PALETTECALLBACKS;
typedef struct _DD_HALINFO DD_HALINFO;
typedef struct _VIDEOMEMORY VIDEOMEMORY;

#include <winddi.h>
#include <winioctl.h>
#include <ntddvdeo.h>

/* Custom Miniport Hardware Acceleration IOCTLs */
#define IOCTL_NVX_ACCEL_COPY             CTL_CODE(FILE_DEVICE_VIDEO, 0x200, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230800 */
#define IOCTL_NVX_ACCEL_FILL             CTL_CODE(FILE_DEVICE_VIDEO, 0x201, METHOD_BUFFERED, FILE_ANY_ACCESS) /* 0x00230804 */

typedef struct _NVX_ACCEL_COPY_PARAMS {
    ULONG64 DstGpuVa;
    ULONG64 SrcGpuVa;
    ULONG   Bytes;
} NVX_ACCEL_COPY_PARAMS, *PNVX_ACCEL_COPY_PARAMS;

typedef struct _NVX_ACCEL_FILL_PARAMS {
    ULONG64 DstGpuVa;
    ULONG   Color;
    ULONG   Bytes;
} NVX_ACCEL_FILL_PARAMS, *PNVX_ACCEL_FILL_PARAMS;

#define ALLOC_TAG 'XVND'

typedef struct _PDEV
{
    HANDLE   hDriver;
    HDEV     hDevEng;
    HSURF    hSurfEng;
    ULONG    ModeIndex;
    ULONG    ScreenWidth;
    ULONG    ScreenHeight;
    ULONG    ScreenDelta;
    BYTE     BitsPerPixel;
    ULONG    RedMask;
    ULONG    GreenMask;
    ULONG    BlueMask;
    PVOID    ScreenPtr;
    HPALETTE DefaultPalette;
    PALETTEENTRY *PaletteEntries;
    ULONG    iDitherFormat;
    ULONG64  ScreenGpuVaBase; /* 0x20000000 */
    ULONG    AccelCopies;
    ULONG    AccelFills;
} PDEV, *PPDEV;

/* DDI Prototypes */
BOOL APIENTRY DrvEnableDriver(ULONG iEngineVersion, ULONG cj, PDRVENABLEDATA pded);
VOID APIENTRY DrvDisableDriver(VOID);
DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *pdm, LPWSTR pwszLogAddress, ULONG cPat, HSURF *phsurfPatterns, ULONG cjCaps, ULONG *pdevcaps, ULONG cjDevInfo, DEVINFO *pdi, HDEV hdev, LPWSTR pwszDeviceName, HANDLE hDriver);
VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev);
VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev);
HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev);
VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev);
BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL bEnable);
ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm);
BOOL APIENTRY DrvBitBlt(SURFOBJ *psoTrg, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclTrg, POINTL *pptlSrc, POINTL *pptlMask, BRUSHOBJ *pbo, POINTL *pptlBrush, ROP4 rop4);
BOOL APIENTRY DrvCopyBits(SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclDest, POINTL *pptlSrc);
BOOL APIENTRY DrvTextOut(SURFOBJ *pso, STROBJ *pstro, FONTOBJ *pfo, CLIPOBJ *pco, RECTL *prclExtra, RECTL *prclOpaque, BRUSHOBJ *pboFore, BRUSHOBJ *pboOpaque, POINTL *pptlOrg, MIX mix);
ULONG APIENTRY DrvDitherColor(DHPDEV dhpdev, ULONG iMode, ULONG rgb, ULONG *pul);
BOOL APIENTRY DrvSetPalette(DHPDEV dhpdev, PALOBJ *ppalo, ULONG fl, ULONG iStart, ULONG cColors);
VOID APIENTRY DrvSynchronize(DHPDEV dhpdev, RECTL *prcl);
ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo, LONG xHot, LONG yHot, LONG x, LONG y, RECTL *prcl, FLONG fl);
VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl);

#endif /* _NVX_DISP_DRIVER_H_ */
