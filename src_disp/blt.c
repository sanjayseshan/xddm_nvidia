/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * DrvBitBlt & DrvCopyBits hardware acceleration routines.
 */

#include "driver.h"

/*
 * DrvCopyBits:
 * Screen-to-screen bitmap copy acceleration.
 */
BOOL APIENTRY
DrvCopyBits(
    SURFOBJ *psoDest,
    SURFOBJ *psoSrc,
    CLIPOBJ *pco,
    XLATEOBJ *pxlo,
    RECTL *prclDest,
    POINTL *pptlSrc)
{
    PPDEV ppdev;
    ULONG ulTemp;
    NVX_ACCEL_COPY_PARAMS copyParams;
    LONG width, height;
    LONG bytesPerPixel;
    LONG y;

    if (!psoDest || !psoDest->dhpdev)
        return EngCopyBits(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);

    ppdev = (PPDEV)psoDest->dhpdev;

    /*
     * Check if this is a screen-to-screen blit within the primary surface:
     * - Dest and Src must be the primary surface (same dhpdev)
     * - No color translation (pxlo is NULL or trivial)
     * - Trivial clipping (CLIPOBJ is NULL or DCO_ALL)
     */
    if (psoSrc && psoSrc->dhpdev == (DHPDEV)ppdev &&
        (!pxlo || (pxlo->flXlate & XO_TRIVIAL)) &&
        (!pco || pco->iDComplexity == DC_TRIVIAL))
    {
        width = prclDest->right - prclDest->left;
        height = prclDest->bottom - prclDest->top;

        if (width > 0 && height > 0)
        {
            bytesPerPixel = (ppdev->BitsPerPixel + 7) / 8;

            /* Check if we can do full rectangle or line-by-line copy */
            if (width * bytesPerPixel == (LONG)ppdev->ScreenDelta &&
                prclDest->left == 0 && pptlSrc->x == 0)
            {
                /* Contiguous multi-line copy */
                copyParams.DstGpuVa = ppdev->ScreenGpuVaBase +
                    (ULONG64)prclDest->top * ppdev->ScreenDelta;
                copyParams.SrcGpuVa = ppdev->ScreenGpuVaBase +
                    (ULONG64)pptlSrc->y * ppdev->ScreenDelta;
                copyParams.Bytes = width * height * bytesPerPixel;

                if (!EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_COPY,
                                        &copyParams, sizeof(copyParams),
                                        NULL, 0, &ulTemp))
                {
                    ppdev->AccelCopies++;
                    return TRUE;
                }
            }
            else
            {
                /* Scanline-by-scanline copy */
                /* Handle vertical direction to prevent overlap corruption */
                LONG lineBytes = width * bytesPerPixel;
                BOOL success = TRUE;

                if (prclDest->top <= pptlSrc->y)
                {
                    /* Top to bottom */
                    for (y = 0; y < height; y++)
                    {
                        copyParams.DstGpuVa = ppdev->ScreenGpuVaBase +
                            (ULONG64)(prclDest->top + y) * ppdev->ScreenDelta +
                            (ULONG64)prclDest->left * bytesPerPixel;
                        copyParams.SrcGpuVa = ppdev->ScreenGpuVaBase +
                            (ULONG64)(pptlSrc->y + y) * ppdev->ScreenDelta +
                            (ULONG64)pptlSrc->x * bytesPerPixel;
                        copyParams.Bytes = lineBytes;

                        if (EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_COPY,
                                               &copyParams, sizeof(copyParams),
                                               NULL, 0, &ulTemp))
                        {
                            success = FALSE;
                            break;
                        }
                    }
                }
                else
                {
                    /* Bottom to top */
                    for (y = height - 1; y >= 0; y--)
                    {
                        copyParams.DstGpuVa = ppdev->ScreenGpuVaBase +
                            (ULONG64)(prclDest->top + y) * ppdev->ScreenDelta +
                            (ULONG64)prclDest->left * bytesPerPixel;
                        copyParams.SrcGpuVa = ppdev->ScreenGpuVaBase +
                            (ULONG64)(pptlSrc->y + y) * ppdev->ScreenDelta +
                            (ULONG64)pptlSrc->x * bytesPerPixel;
                        copyParams.Bytes = lineBytes;

                        if (EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_COPY,
                                               &copyParams, sizeof(copyParams),
                                               NULL, 0, &ulTemp))
                        {
                            success = FALSE;
                            break;
                        }
                    }
                }

                if (success)
                {
                    ppdev->AccelCopies++;
                    return TRUE;
                }
            }
        }
    }

    /* Fallback to software EngCopyBits */
    return EngCopyBits(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);
}

/*
 * DrvBitBlt:
 * General bit-block transfer, supporting accelerated fills (PATCOPY, BLACKNESS, WHITENESS)
 * and screen-to-screen copies (SRCCOPY).
 */
BOOL APIENTRY
DrvBitBlt(
    SURFOBJ *psoTrg,
    SURFOBJ *psoSrc,
    SURFOBJ *psoMask,
    CLIPOBJ *pco,
    XLATEOBJ *pxlo,
    RECTL *prclTrg,
    POINTL *pptlSrc,
    POINTL *pptlMask,
    BRUSHOBJ *pbo,
    POINTL *pptlBrush,
    ROP4 rop4)
{
    PPDEV ppdev;
    ULONG ulTemp;
    LONG width, height;
    LONG bytesPerPixel;
    LONG y;

    if (!psoTrg || !psoTrg->dhpdev)
    {
        return EngBitBlt(psoTrg, psoSrc, psoMask, pco, pxlo,
                         prclTrg, pptlSrc, pptlMask, pbo, pptlBrush, rop4);
    }

    ppdev = (PPDEV)psoTrg->dhpdev;
    bytesPerPixel = (ppdev->BitsPerPixel + 7) / 8;
    width = prclTrg->right - prclTrg->left;
    height = prclTrg->bottom - prclTrg->top;

    if (width <= 0 || height <= 0)
        return TRUE;

    /*
     * NEVER accelerate when there is a mask (psoMask != NULL):
     * Icons, transparent cursors, text glyphs use psoMask.
     * Acceleration should only handle unmasked, trivial-clipping operations!
     */
    if (!psoMask && (!pco || pco->iDComplexity == DC_TRIVIAL))
    {
        /*
         * 1. Check for Solid Color Fills:
         * BLACKNESS (rop4 == 0x0000)
         * WHITENESS (rop4 == 0xFFFF)
         * PATCOPY   (rop4 == 0xF0F0 with solid brush)
         * Only accelerate if brush is actually solid (iSolidColor != 0xFFFFFFFF)
         */
        if (rop4 == 0x0000 || rop4 == 0xFFFF ||
            (rop4 == 0xF0F0 && pbo && pbo->iSolidColor != 0xFFFFFFFF))
        {
            NVX_ACCEL_FILL_PARAMS fillParams;
            ULONG fillColor = 0;

            if (rop4 == 0x0000)
                fillColor = 0x00000000;
            else if (rop4 == 0xFFFF)
                fillColor = 0xFFFFFFFF;
            else
                fillColor = pbo->iSolidColor;

            /* Check if contiguous multi-line fill (e.g. desktop background clear) */
            if (width * bytesPerPixel == (LONG)ppdev->ScreenDelta && prclTrg->left == 0)
            {
                fillParams.DstGpuVa = ppdev->ScreenGpuVaBase +
                    (ULONG64)prclTrg->top * ppdev->ScreenDelta;
                fillParams.Color = fillColor;
                fillParams.Bytes = width * height * bytesPerPixel;

                if (!EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_FILL,
                                        &fillParams, sizeof(fillParams),
                                        NULL, 0, &ulTemp))
                {
                    ppdev->AccelFills++;
                    return TRUE;
                }
            }
            else if (width >= 8 && height >= 2)
            {
                /* Scanline-by-scanline fill for non-trivial rectangles (>= 8px wide) */
                LONG lineBytes = width * bytesPerPixel;
                BOOL success = TRUE;

                for (y = 0; y < height; y++)
                {
                    fillParams.DstGpuVa = ppdev->ScreenGpuVaBase +
                        (ULONG64)(prclTrg->top + y) * ppdev->ScreenDelta +
                        (ULONG64)prclTrg->left * bytesPerPixel;
                    fillParams.Color = fillColor;
                    fillParams.Bytes = lineBytes;

                    if (EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_FILL,
                                           &fillParams, sizeof(fillParams),
                                           NULL, 0, &ulTemp))
                    {
                        success = FALSE;
                        break;
                    }
                }

                if (success)
                {
                    ppdev->AccelFills++;
                    return TRUE;
                }
            }
        }

        /*
         * 2. Check for Screen-to-Screen Copy (SRCCOPY, rop4 == 0xCCCC)
         */
        if (rop4 == 0xCCCC && psoSrc && psoSrc->dhpdev == (DHPDEV)ppdev &&
            (!pxlo || (pxlo->flXlate & XO_TRIVIAL)))
        {
            return DrvCopyBits(psoTrg, psoSrc, pco, pxlo, prclTrg, pptlSrc);
        }
    }

    /* Fallback to software EngBitBlt for masked blits, icons, glyphs, complex ROPs */
    return EngBitBlt(psoTrg, psoSrc, psoMask, pco, pxlo,
                     prclTrg, pptlSrc, pptlMask, pbo, pptlBrush, rop4);
}
