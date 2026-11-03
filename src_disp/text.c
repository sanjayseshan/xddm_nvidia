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
    return EngTextOut(pso, pstro, pfo, pco, prclExtra, prclOpaque, pboFore, pboOpaque, pptlOrg, mix);
}
