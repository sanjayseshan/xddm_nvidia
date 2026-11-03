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

    /* Publish the whole authoritative bitmap to isolate missed dirty
       bounds and direct GDI writes from pixel production. */
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
    BOOL result=EngBitBlt(psoTrg,psoSrc,psoMask,pco,pxlo,prclTrg,pptlSrc,
                           pptlMask,pbo,pptlBrush,rop4);
    if(result && psoTrg && psoTrg->dhpdev)
        return NvxUploadSurfaceRect((PPDEV)psoTrg->dhpdev,psoTrg,prclTrg,
                                    NVX_UPLOAD_KIND_BITBLT);
    return result;
}
