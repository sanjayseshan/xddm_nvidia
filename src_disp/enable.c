/*
 * NVIDIA GA104 XDDM Display Driver
 *
 * Implements DDI entry points, physical device creation, and GDI hook routing.
 */

#include "driver.h"

static DRVFN DrvFunctionTable[] =
{
    {INDEX_DrvEnablePDEV,       (PFN)DrvEnablePDEV},
    {INDEX_DrvCompletePDEV,     (PFN)DrvCompletePDEV},
    {INDEX_DrvDisablePDEV,      (PFN)DrvDisablePDEV},
    {INDEX_DrvEnableSurface,    (PFN)DrvEnableSurface},
    {INDEX_DrvDisableSurface,   (PFN)DrvDisableSurface},
    {INDEX_DrvAssertMode,       (PFN)DrvAssertMode},
    {INDEX_DrvGetModes,         (PFN)DrvGetModes},
    {INDEX_DrvSetPalette,       (PFN)DrvSetPalette},
    {INDEX_DrvSetPointerShape,  (PFN)DrvSetPointerShape},
    {INDEX_DrvMovePointer,      (PFN)DrvMovePointer},
    {INDEX_DrvBitBlt,           (PFN)DrvBitBlt},
    {INDEX_DrvCopyBits,         (PFN)DrvCopyBits},
    {INDEX_DrvTextOut,          (PFN)DrvTextOut},
    {INDEX_DrvDitherColor,      (PFN)DrvDitherColor},
    {INDEX_DrvSynchronize,      (PFN)DrvSynchronize},
    {INDEX_DrvEscape,           (PFN)DrvEscape},
#if defined(NVX_ENABLE_EXPERIMENTAL_OPENGL_DDI) && NVX_ENABLE_EXPERIMENTAL_OPENGL_DDI
    /* Scaffolding only: leave Windows' generic renderer in charge until an
       ICD/MCD and a real buffer-swap implementation exist. */
    {INDEX_DrvDescribePixelFormat, (PFN)DrvDescribePixelFormat},
    {INDEX_DrvSetPixelFormat,   (PFN)DrvSetPixelFormat},
    {INDEX_DrvSwapBuffers,      (PFN)DrvSwapBuffers},
#endif
    {INDEX_DrvDisableDriver,    (PFN)DrvDisableDriver}
};

BOOL APIENTRY
DrvEnableDriver(
    ULONG iEngineVersion,
    ULONG cj,
    PDRVENABLEDATA pded)
{
    NvxDispLogEvent(0,iEngineVersion,cj,0);
    if (cj < sizeof(DRVENABLEDATA) || !pded)
    {
        NvxDispLogEvent(16,FALSE,cj,0);
        return FALSE;
    }

    pded->c = sizeof(DrvFunctionTable) / sizeof(DRVFN);
    pded->pdrvfn = DrvFunctionTable;
#ifdef _WIN64
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5_01;
#else
    /*
     * Windows XP 32-bit win32k!ldevInitDrivers specifically checks:
     *   cmpl $0x30101, pded->iDriverVersion
     *   ja failure
     *   cmpl $0x20000, pded->iDriverVersion
     *   jb failure
     * Official Microsoft 32-bit display drivers (vga.dll, vga64k.dll, cirrus.dll)
     * all return DDI_DRIVER_VERSION_NT4 (0x00020000).
     */
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT4;
#endif

    NvxDispLogEvent(16,TRUE,pded->c,pded->iDriverVersion);
    return TRUE;
}

VOID APIENTRY
DrvDisableDriver(VOID)
{
    NvxDispLogEvent(17,0,0,0);
    NvxDispCloseLog();
}
