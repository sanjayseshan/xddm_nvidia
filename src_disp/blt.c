/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * DrvBitBlt & DrvCopyBits hardware acceleration routines.
 */

#include "driver.h"

/*
 * GPU-rendered XDDM solid PATCOPY fills on the primary bitmap.
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
    ULONG color, ulTemp, rowBytes, rowsPerBatch, rows, batchBytes;
    LONG y, x;
    PULONG shadow, presented;

    if (!ppdev || !pso || !prcl || !ppdev->PresentationValid ||
        ppdev->BitsPerPixel != 32 || pso->dhpdev != (DHPDEV)ppdev ||
        pso->iType != STYPE_BITMAP || pso->pvBits != ppdev->ScreenPtr ||
        pso->iBitmapFormat != BMF_32BPP || pso->lDelta <= 0 ||
        (ULONG)pso->lDelta != ppdev->ScreenDelta ||
        (pco && pco->iDComplexity == DC_COMPLEX))
        return FALSE;

    /* Keep the proven path restricted to PATCOPY; other ROPs use GDI. */
    if (rop4 != 0xF0F0 || !pbo)
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
    if (rect.left == 0 && rect.right == (LONG)ppdev->ScreenWidth &&
        (ULONG64)ppdev->ScreenWidth * 4 == ppdev->ScreenDelta) {
        /* Full-width rows are contiguous in VRAM. Batch them to reduce CE
           queue submissions for fills such as a desktop-wide clear. */
        rowsPerBatch = 0x01000000UL / ppdev->ScreenDelta;
        if (!rowsPerBatch)
            rowsPerBatch = 1;
        for (y = rect.top; y < rect.bottom;) {
            rows = (ULONG)(rect.bottom - y);
            if (rows > rowsPerBatch)
                rows = rowsPerBatch;
            batchBytes = rows * ppdev->ScreenDelta;
            params.DstGpuVa = ppdev->ScreenGpuVaBase +
                (ULONG64)(ULONG)y * ppdev->ScreenDelta;
            params.Bytes = batchBytes;
            if (EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_FILL,
                                   &params, sizeof(params), NULL, 0, &ulTemp)) {
                ppdev->PresentationValid = FALSE;
                return FALSE;
            }
            for (x = 0; x < (LONG)(batchBytes / 4); ++x) {
                ULONG index = (ULONG)y * (ppdev->ScreenDelta / 4) + (ULONG)x;
                shadow[index] = color;
                presented[index] = color;
            }
            y += (LONG)rows;
        }
    } else {
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
    PPDEV ppdev;
    NVX_ACCEL_COPY_PARAMS params;
    ULONG width, height, rowBytes, ulTemp, row;
    ULONG srcX, srcY, dstX, dstY, y, x, index, sourceIndex, targetIndex;
    BOOL overlaps;

    /* Accelerate same-primary-surface copies when the surface is already
       coherent. Keep unsupported clips/translations on the GDI path. */
    ppdev = (PPDEV)(psoDest ? psoDest->dhpdev : NULL);
    if (ppdev && psoDest && psoSrc && prclDest && pptlSrc &&
        ppdev->PresentationValid && ppdev->BitsPerPixel == 32 &&
        psoDest->dhpdev == (DHPDEV)ppdev && psoSrc->dhpdev == (DHPDEV)ppdev &&
        psoDest->iType == STYPE_BITMAP && psoSrc->iType == STYPE_BITMAP &&
        psoDest->pvBits == ppdev->ScreenPtr && psoSrc->pvBits == ppdev->ScreenPtr &&
        psoDest->iBitmapFormat == BMF_32BPP && psoSrc->iBitmapFormat == BMF_32BPP &&
        psoDest->lDelta > 0 && psoSrc->lDelta > 0 &&
        (ULONG)psoDest->lDelta == ppdev->ScreenDelta &&
        (ULONG)psoSrc->lDelta == ppdev->ScreenDelta && !pco && !pxlo &&
        prclDest->left >= 0 && prclDest->top >= 0 &&
        prclDest->right <= (LONG)ppdev->ScreenWidth &&
        prclDest->bottom <= (LONG)ppdev->ScreenHeight &&
        pptlSrc->x >= 0 && pptlSrc->y >= 0 &&
        prclDest->right > prclDest->left && prclDest->bottom > prclDest->top) {
        width = (ULONG)(prclDest->right - prclDest->left);
        height = (ULONG)(prclDest->bottom - prclDest->top);
        srcX = (ULONG)pptlSrc->x;
        srcY = (ULONG)pptlSrc->y;
        dstX = (ULONG)prclDest->left;
        dstY = (ULONG)prclDest->top;
        if (srcX <= ppdev->ScreenWidth && width <= ppdev->ScreenWidth - srcX &&
            srcY <= ppdev->ScreenHeight && height <= ppdev->ScreenHeight - srcY &&
            width <= 0x003FFFFFUL) {
            rowBytes = width * 4;
            overlaps = srcX < dstX + width && dstX < srcX + width &&
                srcY < dstY + height && dstY < srcY + height;
            /* CE overlap behavior is not assumed for horizontal overlap. */
            if (!overlaps || srcX == dstX) {
                for (row = 0; row < height; ++row) {
                    y = (dstY > srcY) ? (height - 1 - row) : row;
                    params.SrcGpuVa = ppdev->ScreenGpuVaBase +
                        (ULONG64)(srcY + y) * ppdev->ScreenDelta + (ULONG64)srcX * 4;
                    params.DstGpuVa = ppdev->ScreenGpuVaBase +
                        (ULONG64)(dstY + y) * ppdev->ScreenDelta + (ULONG64)dstX * 4;
                    params.Bytes = rowBytes;
                    if (EngDeviceIoControl(ppdev->hDriver, IOCTL_NVX_ACCEL_COPY,
                                           &params, sizeof(params), NULL, 0, &ulTemp))
                        break;
                }
                if (row == height) {
                    PULONG shadow = (PULONG)ppdev->ScreenPtr;
                    PULONG presented = (PULONG)ppdev->PresentedPixels;
                    for (row = 0; row < height; ++row) {
                        y = (dstY > srcY) ? (height - 1 - row) : row;
                        sourceIndex = (srcY + y) * (ppdev->ScreenDelta / 4) + srcX;
                        targetIndex = (dstY + y) * (ppdev->ScreenDelta / 4) + dstX;
                        if (dstX > srcX && overlaps) {
                            for (x = width; x > 0; --x) {
                                index = x - 1;
                                shadow[targetIndex + index] = shadow[sourceIndex + index];
                                presented[targetIndex + index] = shadow[sourceIndex + index];
                            }
                        } else {
                            for (x = 0; x < width; ++x) {
                                shadow[targetIndex + x] = shadow[sourceIndex + x];
                                presented[targetIndex + x] = shadow[sourceIndex + x];
                            }
                        }
                    }
                    ppdev->AccelCopies++;
                    return TRUE;
                }
                /* Partial CE copies are repaired by the software fallback. */
                ppdev->PresentationValid = FALSE;
            }
        }
    }

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
