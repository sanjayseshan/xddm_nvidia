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
    BOOL result=EngTextOut(pso,pstro,pfo,pco,prclExtra,prclOpaque,
                           pboFore,pboOpaque,pptlOrg,mix);
    if(result&&pso&&pso->dhpdev&&pstro&&(!pco||pco->iDComplexity==DC_TRIVIAL)) {
        PPDEV ppdev=(PPDEV)pso->dhpdev;
        (void)NvxUploadSurfaceRect(ppdev,pso,&pstro->rclBkGround);
    }
    return result;
}
