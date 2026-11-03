/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * DrvBitBlt & DrvCopyBits hardware acceleration routines.
 */

#include "driver.h"

/*
 * First GPU-rendered XDDM primitive: a solid PATCOPY to the primary bitmap.
 * GDI keeps an authoritative system-memory shadow, while the GPU copy engine
 * writes the corresponding scanout pixels directly. Unsupported operations
 * continue through EngBitBlt and the normal upload path.
 */
static BOOL
NvxTrySolidPatCopy(PPDEV ppdev, SURFOBJ *pso, CLIPOBJ *pco,
                   BRUSHOBJ *pbo, RECTL *prcl, ROP4 rop4)
{
    NVX_ACCEL_FILL_PARAMS params;
    RECTL rect;
    ULONG color, ulTemp, rowBytes;
    LONG y, x;
    PULONG shadow, presented;

    if (!ppdev || !pso || !pbo || !prcl || !ppdev->PresentationValid ||
        ppdev->BitsPerPixel != 32 || pso->dhpdev != (DHPDEV)ppdev ||
        pso->iType != STYPE_BITMAP || pso->pvBits != ppdev->ScreenPtr ||
        pso->iBitmapFormat != BMF_32BPP || pso->lDelta <= 0 ||
        (ULONG)pso->lDelta != ppdev->ScreenDelta || rop4 != 0xF0F0 ||
        (pco && pco->iDComplexity == DC_COMPLEX))
        return FALSE;

    color = BRUSHOBJ_ulGetBrushColor(pbo);
    if (color == CLR_INVALID)
        return FALSE;

    rect = *prcl;
    if (pco && pco->iDComplexity == DC_RECT) {
        if (rect.left < pco->rclBounds.left) rect.left = pco->rclBounds.left;
        if (rect.top < pco->rclBounds.top) rect.top = pco->rclBounds.top;
        if (rect.right > pco->rclBounds.right) rect.right = pco->rclBounds.right;
        if (rect.bottom > pco->rclBounds.bottom) rect.bottom = pco->rclBounds.bottom;
    } else if (pco && pco->iDComplexity != DC_TRIVIAL) {
        return FALSE;
    }
    if (rect.left < 0) rect.left = 0;
    if (rect.top < 0) rect.top = 0;
    if (rect.right > (LONG)ppdev->ScreenWidth) rect.right = (LONG)ppdev->ScreenWidth;
    if (rect.bottom > (LONG)ppdev->ScreenHeight) rect.bottom = (LONG)ppdev->ScreenHeight;
    if (rect.left >= rect.right || rect.top >= rect.bottom)
        return TRUE;

    rowBytes = (ULONG)(rect.right - rect.left) * 4;
    if (!rowBytes || rowBytes > 0x01000000UL || (rowBytes & 3))
        return FALSE;

    shadow = (PULONG)ppdev->ScreenPtr;
    presented = (PULONG)ppdev->PresentedPixels;
    params.Color = color;
    params.Bytes = rowBytes;
    for (y = rect.top; y < rect.bottom; ++y) {
        params.DstGpuVa = ppdev->ScreenGpuVaBase +
            (ULONG64)(ULONG)y * ppdev->ScreenDelta + (ULONG64)(ULONG)rect.left * 4;
        if (EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_FILL,
                               &params, sizeof(params), NULL, 0, &ulTemp)) {
            /* A partial GPU operation is repaired by the standard full diff upload. */
            ppdev->PresentationValid = FALSE;
            return FALSE;
        }
        for (x = rect.left; x < rect.right; ++x) {
            shadow[(ULONG)y * (ppdev->ScreenDelta / 4) + (ULONG)x] = color;
            presented[(ULONG)y * (ppdev->ScreenDelta / 4) + (ULONG)x] = color;
        }
    }
    ppdev->AccelFills++;
    return TRUE;
}

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

    /* Check the whole bitmap to retain v124's handling of direct GDI writes.
       Transfer only changed row runs; cursor motion should not upload 8MB. */
    r.left=0; r.top=0;
    r.right=(LONG)ppdev->ScreenWidth; r.bottom=(LONG)ppdev->ScreenHeight;
    if(r.left<0) r.left=0;
    if(r.top<0) r.top=0;
    if(r.right>(LONG)ppdev->ScreenWidth) r.right=(LONG)ppdev->ScreenWidth;
    if(r.bottom>(LONG)ppdev->ScreenHeight) r.bottom=(LONG)ppdev->ScreenHeight;
    if(r.left>=r.right||r.top>=r.bottom) return TRUE;

    bytesPerPixel=4;
    /* Include pitch padding when sending full-width rows. This keeps each
       staged row adjacent to the next and preserves its destination stride. */
    lineBytes=ppdev->ScreenDelta;
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
    for(y=r.top;y<r.bottom;) {
        if(ppdev->PresentationValid &&
           !memcmp((PUCHAR)pso->pvBits+(ULONG)y*lineBytes,
                   (PUCHAR)ppdev->PresentedPixels+(ULONG)y*lineBytes,lineBytes)) {
            y++;
            continue;
        }
        rows=1;
        while(rows<rowsPerBatch && y+(LONG)rows<r.bottom) {
            ULONG offset=((ULONG)y+rows)*lineBytes;
            if(ppdev->PresentationValid &&
               !memcmp((PUCHAR)pso->pvBits+offset,
                       (PUCHAR)ppdev->PresentedPixels+offset,lineBytes))
                break;
            rows++;
        }
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
        /* Cache the exact staged snapshot, and only after GPU completion. */
        memcpy((PUCHAR)ppdev->PresentedPixels+(ULONG)y*lineBytes,
               params->Data,batchBytes);
        y+=(LONG)rows;
    }
    if(success) ppdev->PresentationValid=TRUE;
    else ppdev->PresentationValid=FALSE;

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
    /* Restrict the GPU path to simple, opaque, rectangular solid fills. */
    if (!psoSrc && !psoMask && !pxlo &&
        NvxTrySolidPatCopy((PPDEV)(psoTrg ? psoTrg->dhpdev : NULL),
                           psoTrg, pco, pbo, prclTrg, rop4))
        return TRUE;

    BOOL result=EngBitBlt(psoTrg,psoSrc,psoMask,pco,pxlo,prclTrg,pptlSrc,
                           pptlMask,pbo,pptlBrush,rop4);
    if(result && psoTrg && psoTrg->dhpdev)
        return NvxUploadSurfaceRect((PPDEV)psoTrg->dhpdev,psoTrg,prclTrg,
                                    NVX_UPLOAD_KIND_BITBLT);
    return result;
}
