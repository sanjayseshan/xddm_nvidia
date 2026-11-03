/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * Text output handling: delegates glyph rasterization to GDI engine.
 */

#include "driver.h"

static VOID
NvxIncludeRect(RECTL *dst, const RECTL *src, BOOL *have)
{
    if(!src||src->left>=src->right||src->top>=src->bottom) return;
    if(!*have) {
        *dst=*src;
        *have=TRUE;
        return;
    }
    if(src->left<dst->left) dst->left=src->left;
    if(src->top<dst->top) dst->top=src->top;
    if(src->right>dst->right) dst->right=src->right;
    if(src->bottom>dst->bottom) dst->bottom=src->bottom;
}

/* rclBkGround is a string box, but glyph ink may extend beyond it. */
static VOID
NvxIncludeGlyphInk(STROBJ *pstro, RECTL *bounds, BOOL *have)
{
    ULONG count;
    PGLYPHPOS glyphs;
    BOOL more;

    STROBJ_vEnumStart(pstro);
    do {
        count=0;
        glyphs=NULL;
        more=STROBJ_bEnum(pstro,&count,&glyphs);
        if((ULONG)more==DDI_ERROR) break;
        for(ULONG i=0;i<count;i++) {
            GLYPHBITS *bits;
            RECTL glyphRect;
            if(!glyphs[i].pgdf||(bits=glyphs[i].pgdf->pgb)==NULL||
               bits->sizlBitmap.cx<=0||bits->sizlBitmap.cy<=0)
                continue;
            glyphRect.left=glyphs[i].ptl.x+bits->ptlOrigin.x;
            glyphRect.top=glyphs[i].ptl.y+bits->ptlOrigin.y;
            glyphRect.right=glyphRect.left+bits->sizlBitmap.cx;
            glyphRect.bottom=glyphRect.top+bits->sizlBitmap.cy;
            NvxIncludeRect(bounds,&glyphRect,have);
        }
    } while(more);
    /* EngTextOut must consume the same string from its first glyph. */
    STROBJ_vEnumStart(pstro);
}

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
    BOOL haveUploadRect=FALSE;
    if(pstro) {
        NvxIncludeRect(&uploadRect,&pstro->rclBkGround,&haveUploadRect);
        NvxIncludeGlyphInk(pstro,&uploadRect,&haveUploadRect);
    }
    NvxIncludeRect(&uploadRect,prclOpaque,&haveUploadRect);
    NvxIncludeRect(&uploadRect,prclExtra,&haveUploadRect);
    BOOL result=EngTextOut(pso,pstro,pfo,pco,prclExtra,prclOpaque,
                           pboFore,pboOpaque,pptlOrg,mix);
    if(!NVX_SOFTWARE_SCANOUT_DIAGNOSTIC&&result&&pso&&pso->dhpdev&&pstro&&haveUploadRect) {
        PPDEV ppdev=(PPDEV)pso->dhpdev;
        (void)NvxUploadSurfaceRect(ppdev,pso,&uploadRect,NVX_UPLOAD_KIND_TEXTOUT);
    }
    return result;
}
