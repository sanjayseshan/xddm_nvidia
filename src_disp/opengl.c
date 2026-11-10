/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * OpenGL Pipeline: DDI pixel format negotiation and swap buffer synchronization.
 */

#include "driver.h"
#include "opengl_icd.h"

ULONG APIENTRY
DrvEscape(SURFOBJ *pso,ULONG iEsc,ULONG cjIn,PVOID pvIn,
          ULONG cjOut,PVOID pvOut)
{
    PPDEV ppdev;
    ULONG returned=0,code=0,status;

    /* Windows 2000/XP OpenGL32 discovers an ICD through these legacy
       DrvEscape queries. The ICD is selected by the `nvxdisp` registry key. */
    if(iEsc==QUERYESCSUPPORT) {
        ULONG requested;
        if(!pvIn||cjIn<sizeof(requested)) return 0;
        memcpy(&requested,pvIn,sizeof(requested));
        returned=requested==OPENGL_GETINFO?1:0;
        if(requested==OPENGL_GETINFO)
            NvxDispLogEvent(0x200,iEsc,requested,returned);
        return returned;
    }
    if(iEsc==OPENGL_GETINFO) {
        ULONG subEscape;
        if(!pvIn||cjIn<sizeof(subEscape)||!pvOut||
           !pso||!pso->dhpdev) return 0;
        memcpy(&subEscape,pvIn,sizeof(subEscape));
        returned=NvxBuildOpenGlDriverInfo(subEscape,pvOut,cjOut);
        NvxDispLogEvent(0x201,subEscape,returned,
                        returned?(ULONG)((NVX_OPENGL_DRIVER_INFO *)pvOut)->Version:0);
        return returned;
    }

    if(!pso||!pso->dhpdev) return 0;
    ppdev=(PPDEV)pso->dhpdev;
    if(!ppdev->hDriver) return 0;
    switch(iEsc) {
    case NVX_ESCAPE_3D_QUERY_CAPS:
        if(cjIn||cjOut<sizeof(NVX_3D_QUERY_CAPS)||!pvOut) return 0;
        code=IOCTL_NVX_3D_QUERY_CAPS;break;
    case NVX_ESCAPE_3D_ALLOC:
        if(cjIn!=sizeof(NVX_3D_ALLOC_BUFFER)||!pvIn||
           cjOut<sizeof(NVX_3D_ALLOC_BUFFER)||!pvOut) return 0;
        code=IOCTL_NVX_3D_ALLOC_BUFFER;break;
    case NVX_ESCAPE_3D_SUBMIT:
        if(cjIn!=sizeof(NVX_3D_SUBMIT)||!pvIn||
           cjOut<sizeof(NVX_3D_SUBMIT)||!pvOut) return 0;
        code=IOCTL_NVX_3D_SUBMIT;break;
    case NVX_ESCAPE_3D_WAIT:
        if(cjIn!=sizeof(NVX_3D_WAIT_FENCE)||!pvIn||
           cjOut<sizeof(NVX_3D_WAIT_FENCE)||!pvOut) return 0;
        code=IOCTL_NVX_3D_WAIT_FENCE;break;
    case NVX_ESCAPE_3D_FREE:
        if(cjIn!=sizeof(NVX_3D_FREE_BUFFER)||!pvIn||cjOut) return 0;
        code=IOCTL_NVX_3D_FREE_BUFFER;break;
    case NVX_ESCAPE_3D_UPLOAD: {
        NVX_3D_UPLOAD_BUFFER upload;
        if(!pvIn||cjIn<sizeof(upload)||cjOut) return 0;
        memcpy(&upload,pvIn,sizeof(upload));
        if(upload.Size!=sizeof(upload)||
           upload.Version!=NVX_3D_ABI_VERSION||!upload.BufferId||
           !upload.Bytes||upload.Bytes>NVX_3D_MAX_UPLOAD_BYTES||
           cjIn!=sizeof(upload)+upload.Bytes) return 0;
        code=IOCTL_NVX_3D_UPLOAD_BUFFER;break;
    }
    case NVX_ESCAPE_3D_READBACK:
        if(cjIn!=sizeof(NVX_3D_READBACK)||!pvIn||!pvOut||
           cjOut<NVX_3D_READBACK_BYTES) return 0;
        code=IOCTL_NVX_3D_READBACK_BUFFER;break;
    default:
        return 0;
    }
    status=EngDeviceIoControl(ppdev->hDriver,code,pvIn,cjIn,pvOut,cjOut,
                              &returned);
    if(status) return 0;
    return returned?returned:1;
}

/*
 * DrvDescribePixelFormat:
 * Enumerates supported OpenGL pixel formats for the GDI/OpenGL ICD subsystem.
 */
LONG APIENTRY
DrvDescribePixelFormat(
    DHPDEV dhpdev,
    LONG iPixelFormat,
    ULONG cjDesc,
    PPIXELFORMATDESCRIPTOR ppfd)
{
    PPDEV ppdev = (PPDEV)dhpdev;
    if (!ppdev) return 0;

    /* Query total number of supported formats */
    if (!ppfd || cjDesc == 0) {
        return 2;
    }

    if (iPixelFormat < 1 || iPixelFormat > 2) {
        return 0;
    }

    if (cjDesc < sizeof(PIXELFORMATDESCRIPTOR)) {
        return 0;
    }

    memset(ppfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
    ppfd->nSize = sizeof(PIXELFORMATDESCRIPTOR);
    ppfd->nVersion = 1;
    ppfd->iPixelType = PFD_TYPE_RGBA;
    ppfd->cColorBits = 32;
    ppfd->cRedBits = 8;
    ppfd->cRedShift = 16;
    ppfd->cGreenBits = 8;
    ppfd->cGreenShift = 8;
    ppfd->cBlueBits = 8;
    ppfd->cBlueShift = 0;
    ppfd->cAlphaBits = 8;
    ppfd->cAlphaShift = 24;
    ppfd->cAccumBits = 0;
    ppfd->cDepthBits = 24;
    ppfd->cStencilBits = 8;
    ppfd->cAuxBuffers = 0;
    ppfd->iLayerType = PFD_MAIN_PLANE;

    if (iPixelFormat == 1) {
        /* ICD-backed double-buffered format; presentation copies the back
           surface into the scanout surface. */
        ppfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                        PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
    } else {
        ppfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                        PFD_SUPPORT_GDI;
    }

    return 2;
}

/*
 * DrvSetPixelFormat:
 * Confirms pixel format selection on the surface.
 */
BOOL APIENTRY
DrvSetPixelFormat(
    SURFOBJ *pso,
    LONG iPixelFormat,
    HWND hwnd)
{
    (void)pso;
    (void)hwnd;
    if (iPixelFormat < 1 || iPixelFormat > 2)
        return FALSE;
    return TRUE;
}

/*
 * DrvSwapBuffers:
 * Swaps / presents back-buffer contents directly to the physical scanout surface.
 */
BOOL APIENTRY
DrvSwapBuffers(
    SURFOBJ *pso,
    WNDOBJ *pwo)
{
    PPDEV ppdev;
    RECTL rcl;

    if (!pso || !pso->dhpdev)
        return FALSE;
    ppdev = (PPDEV)pso->dhpdev;

    if (pwo) {
        rcl = pwo->rclClient;
    } else {
        rcl.left = 0;
        rcl.top = 0;
        rcl.right = ppdev->ScreenWidth;
        rcl.bottom = ppdev->ScreenHeight;
    }

    if (rcl.left < 0) rcl.left = 0;
    if (rcl.top < 0) rcl.top = 0;
    if (rcl.right > (LONG)ppdev->ScreenWidth) rcl.right = (LONG)ppdev->ScreenWidth;
    if (rcl.bottom > (LONG)ppdev->ScreenHeight) rcl.bottom = (LONG)ppdev->ScreenHeight;
    if (rcl.left >= rcl.right || rcl.top >= rcl.bottom)
        return TRUE;

    return NvxUploadSurfaceRect(ppdev, pso, &rcl, NVX_UPLOAD_KIND_BITBLT);
}
