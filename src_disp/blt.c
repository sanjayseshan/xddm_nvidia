/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * DrvBitBlt & DrvCopyBits hardware acceleration routines.
 */

#include "driver.h"

/* Upload software-rendered primary-surface rows through coherent DMA staging. */
BOOL
NvxUploadSurfaceRect(PPDEV ppdev, SURFOBJ *pso, RECTL *prcl, ULONG kind)
{
    RECTL r;
    LONG y;
    ULONG bytesPerPixel, lineBytes, batchBytes, rowsPerBatch, rows, requestBytes, ulTemp;
    PNVX_ACCEL_UPLOAD_PARAMS params;
    BOOL success=TRUE;

    if(!ppdev||!pso||!prcl||pso->dhpdev!=(DHPDEV)ppdev||
       pso->iType!=STYPE_BITMAP||!pso->pvBits||pso->pvBits!=ppdev->ScreenPtr||
       pso->iBitmapFormat!=BMF_32BPP||ppdev->BitsPerPixel!=32||
       pso->lDelta<=0||(ULONG)pso->lDelta!=ppdev->ScreenDelta)
        return FALSE;

    r=*prcl;
    if(r.left<0) r.left=0;
    if(r.top<0) r.top=0;
    if(r.right>(LONG)ppdev->ScreenWidth) r.right=(LONG)ppdev->ScreenWidth;
    if(r.bottom>(LONG)ppdev->ScreenHeight) r.bottom=(LONG)ppdev->ScreenHeight;
    if(r.left>=r.right||r.top>=r.bottom) return TRUE;

    bytesPerPixel=4;
    lineBytes=(ULONG)(r.right-r.left)*bytesPerPixel;
    if(!lineBytes||lineBytes>NVX_ACCEL_UPLOAD_MAX_BYTES) return FALSE;
    /* The CE copy is linear: only adjacent rows with no pitch padding can be
       coalesced. Partial-width spans must be submitted one row at a time so
       the next row starts at ScreenDelta rather than immediately after pixels. */
    rowsPerBatch=(r.left==0&&lineBytes==ppdev->ScreenDelta)
        ?NVX_ACCEL_UPLOAD_MAX_BYTES/lineBytes:1;
    if(!rowsPerBatch) rowsPerBatch=1;
    requestBytes=FIELD_OFFSET(NVX_ACCEL_UPLOAD_PARAMS,Data)+lineBytes*rowsPerBatch;
    params=(PNVX_ACCEL_UPLOAD_PARAMS)EngAllocMem(0,requestBytes,ALLOC_TAG);
    if(!params) return FALSE;

    params->Reserved=kind;
    for(y=r.top;y<r.bottom;y+=(LONG)rowsPerBatch) {
        rows=(ULONG)((r.bottom-y)<(LONG)rowsPerBatch?(r.bottom-y):(LONG)rowsPerBatch);
        batchBytes=lineBytes*rows;
        params->DstGpuVa=ppdev->ScreenGpuVaBase+
            (ULONG64)(ULONG)y*ppdev->ScreenDelta+(ULONG64)(ULONG)r.left*bytesPerPixel;
        params->Bytes=batchBytes;
        memcpy(params->Data,(PUCHAR)pso->pvBits+(ULONG)y*(ULONG)pso->lDelta+
               (ULONG)r.left*bytesPerPixel,batchBytes);
        requestBytes=FIELD_OFFSET(NVX_ACCEL_UPLOAD_PARAMS,Data)+batchBytes;
        if(EngDeviceIoControl(ppdev->hDriver,IOCTL_NVX_ACCEL_UPLOAD,
                              params,requestBytes,NULL,0,&ulTemp)) {
            success=FALSE;
            break;
        }
    }

    EngFreeMem(params);
    return success;
}

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
    BOOL result = EngCopyBits(psoDest,psoSrc,pco,pxlo,prclDest,pptlSrc);
    if (result && psoDest && psoDest->dhpdev)
        return NvxUploadSurfaceRect((PPDEV)psoDest->dhpdev,psoDest,prclDest,
                                    NVX_UPLOAD_KIND_COPYBITS);
    return result;
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
    if (NVX_SOFTWARE_SCANOUT_DIAGNOSTIC)
        return EngBitBlt(psoTrg,psoSrc,psoMask,pco,pxlo,prclTrg,pptlSrc,
                         pptlMask,pbo,pptlBrush,rop4);
    /* Every operation must update the authoritative bitmap before GPU work. */
    if (!EngBitBlt(psoTrg,psoSrc,psoMask,pco,pxlo,prclTrg,pptlSrc,
                   pptlMask,pbo,pptlBrush,rop4))
        return FALSE;
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

    }

    /* Present pixels already rasterized into the coherent bitmap. */
    return NvxUploadSurfaceRect(ppdev,psoTrg,prclTrg,NVX_UPLOAD_KIND_BITBLT);
}
