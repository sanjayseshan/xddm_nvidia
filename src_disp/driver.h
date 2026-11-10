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
#include "../src_driver/nvx_3d_abi.h"

/* Custom Miniport Hardware Acceleration IOCTLs */
#define IOCTL_NVX_ACCEL_COPY             CTL_CODE(FILE_DEVICE_VIDEO, 0x200, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_ACCEL_FILL             CTL_CODE(FILE_DEVICE_VIDEO, 0x201, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_ACCEL_UPLOAD           CTL_CODE(FILE_DEVICE_VIDEO, 0x202, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_SCANOUT_GPU_VA         CTL_CODE(FILE_DEVICE_VIDEO, 0x203, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_DIAGNOSTIC_TRACE       CTL_CODE(FILE_DEVICE_VIDEO, 0x204, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_DIAGNOSTIC_STATE       CTL_CODE(FILE_DEVICE_VIDEO, 0x205, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_GET_LOG                CTL_CODE(FILE_DEVICE_VIDEO, 0x206, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_3D_QUERY_CAPS          CTL_CODE(FILE_DEVICE_VIDEO, 0x207, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_3D_ALLOC_BUFFER        CTL_CODE(FILE_DEVICE_VIDEO, 0x208, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_3D_SUBMIT              CTL_CODE(FILE_DEVICE_VIDEO, 0x209, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_3D_WAIT_FENCE          CTL_CODE(FILE_DEVICE_VIDEO, 0x20A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NVX_3D_FREE_BUFFER         CTL_CODE(FILE_DEVICE_VIDEO, 0x20B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define NVX_ACCEL_UPLOAD_MAX_BYTES       0x0000F000UL
#define NVX_UPLOAD_KIND_COPYBITS         1UL
#define NVX_UPLOAD_KIND_BITBLT           2UL
#define NVX_UPLOAD_KIND_TEXTOUT          3UL
#define NVX_ACCEL_UPLOAD_HEADER_BYTES    16UL

typedef struct _NVX_DIAGNOSTIC_TRACE {
    ULONG Stage;
    ULONG Value0;
    ULONG Value1;
    ULONG Value2;
} NVX_DIAGNOSTIC_TRACE, *PNVX_DIAGNOSTIC_TRACE;
typedef struct _NVX_DIAGNOSTIC_STATE {
    ULONG Flags;
    ULONG NumModes;
    ULONG CurrentModeIndex;
    ULONG MonitorCount;
    ULONG64 GspVramGpuVa;
} NVX_DIAGNOSTIC_STATE, *PNVX_DIAGNOSTIC_STATE;
VOID NvxDispLogEvent(ULONG stage, ULONG value0, ULONG value1, ULONG value2);
VOID NvxDispFlushMiniportLog(HANDLE hDriver);
VOID NvxDispCloseLog(VOID);
/* Diagnostic mode is available for isolating direct BAR1 surface writes. */
#define NVX_SOFTWARE_SCANOUT_DIAGNOSTIC  0

typedef struct _NVX_ACCEL_COPY_PARAMS {
    ULONG64 DstGpuVa;
    ULONG64 SrcGpuVa;
    ULONG   BytesPerLine;
    ULONG   SrcPitch;
    ULONG   DstPitch;
    ULONG   LineCount;
} NVX_ACCEL_COPY_PARAMS, *PNVX_ACCEL_COPY_PARAMS;

typedef struct _NVX_ACCEL_FILL_PARAMS {
    ULONG64 DstGpuVa;
    ULONG   Color;
    ULONG   Bytes;
} NVX_ACCEL_FILL_PARAMS, *PNVX_ACCEL_FILL_PARAMS;

typedef struct _NVX_ACCEL_UPLOAD_PARAMS {
    ULONG64 DstGpuVa;
    ULONG   Bytes;
    ULONG   Reserved;
    UCHAR   Data[1];
} NVX_ACCEL_UPLOAD_PARAMS, *PNVX_ACCEL_UPLOAD_PARAMS;

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
    PVOID    ScreenPtr; /* coherent GDI desktop bitmap */
    PVOID    MappedFramebuffer;
    PVOID    PresentedPixels; /* last successfully uploaded bitmap */
    BOOL     PresentationValid;
    HPALETTE DefaultPalette;
    PALETTEENTRY *PaletteEntries;
    ULONG    iDitherFormat;
    ULONG64  ScreenGpuVaBase; /* 0x20000000 */
    ULONG    AccelCopies;
    ULONG    AccelFills;
    PVOID    CopyScratch; /* temporary source snapshot for safe CopyBits fallback */
    ULONG    CopyScratchBytes;
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
BOOL NvxUploadSurfaceRect(PPDEV ppdev, SURFOBJ *pso, RECTL *prcl, ULONG kind);
BOOL APIENTRY DrvTextOut(SURFOBJ *pso, STROBJ *pstro, FONTOBJ *pfo, CLIPOBJ *pco, RECTL *prclExtra, RECTL *prclOpaque, BRUSHOBJ *pboFore, BRUSHOBJ *pboOpaque, POINTL *pptlOrg, MIX mix);
ULONG APIENTRY DrvDitherColor(DHPDEV dhpdev, ULONG iMode, ULONG rgb, ULONG *pul);
BOOL APIENTRY DrvSetPalette(DHPDEV dhpdev, PALOBJ *ppalo, ULONG fl, ULONG iStart, ULONG cColors);
VOID APIENTRY DrvSynchronize(DHPDEV dhpdev, RECTL *prcl);
ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo, LONG xHot, LONG yHot, LONG x, LONG y, RECTL *prcl, FLONG fl);
VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl);
LONG APIENTRY DrvDescribePixelFormat(DHPDEV dhpdev, LONG iPixelFormat, ULONG cjDesc, PPIXELFORMATDESCRIPTOR ppfd);
BOOL APIENTRY DrvSetPixelFormat(SURFOBJ *pso, LONG iPixelFormat, HWND hwnd);
BOOL APIENTRY DrvSwapBuffers(SURFOBJ *pso, WNDOBJ *pwo);
ULONG APIENTRY DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                         ULONG cjOut, PVOID pvOut);

#endif /* _NVX_DISP_DRIVER_H_ */
