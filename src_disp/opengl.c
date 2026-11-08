/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * OpenGL Pipeline: DDI pixel format negotiation and swap buffer synchronization.
 */

#include "driver.h"

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
        /* Format 1: Hardware double-buffered OpenGL */
        ppfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SUPPORT_GDI | PFD_GENERIC_FORMAT | PFD_GENERIC_ACCELERATED;
    } else {
        /* Format 2: Single-buffered OpenGL */
        ppfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_SUPPORT_GDI | PFD_GENERIC_FORMAT | PFD_GENERIC_ACCELERATED;
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
