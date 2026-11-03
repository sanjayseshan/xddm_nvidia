/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * Text output handling: delegates glyph rasterization to GDI engine.
 */

#include "driver.h"

BOOL APIENTRY
DrvTextOut(
    SURFOBJ  *pso,
    STROBJ   *pstro,
    FONTOBJ  *pfo,
    CLIPOBJ  *pco,
    RECTL    *prclExtra,
    RECTL    *prclOpaque,
    BRUSHOBJ *pboFore,
    BRUSHOBJ *pboOpaque,
    POINTL   *pptlOrg,
    MIX       mix)
{
    RECTL uploadRect;
    BOOL result=EngTextOut(pso,pstro,pfo,pco,prclExtra,prclOpaque,
                           pboFore,pboOpaque,pptlOrg,mix);
    if(result&&pso&&pso->dhpdev&&pstro) {
        PPDEV ppdev=(PPDEV)pso->dhpdev;
        uploadRect=pstro->rclBkGround;
        if(prclOpaque) {
            if(uploadRect.left>=uploadRect.right||uploadRect.top>=uploadRect.bottom)
                uploadRect=*prclOpaque;
            else {
                if(prclOpaque->left<uploadRect.left) uploadRect.left=prclOpaque->left;
                if(prclOpaque->top<uploadRect.top) uploadRect.top=prclOpaque->top;
                if(prclOpaque->right>uploadRect.right) uploadRect.right=prclOpaque->right;
                if(prclOpaque->bottom>uploadRect.bottom) uploadRect.bottom=prclOpaque->bottom;
            }
        }
        if(prclExtra) {
            if(uploadRect.left>=uploadRect.right||uploadRect.top>=uploadRect.bottom)
                uploadRect=*prclExtra;
            else {
                if(prclExtra->left<uploadRect.left) uploadRect.left=prclExtra->left;
                if(prclExtra->top<uploadRect.top) uploadRect.top=prclExtra->top;
                if(prclExtra->right>uploadRect.right) uploadRect.right=prclExtra->right;
                if(prclExtra->bottom>uploadRect.bottom) uploadRect.bottom=prclExtra->bottom;
            }
        }
        (void)NvxUploadSurfaceRect(ppdev,pso,&uploadRect);
    }
    return result;
}
