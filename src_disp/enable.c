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
    {INDEX_DrvDisableDriver,    (PFN)DrvDisableDriver}
};

BOOL APIENTRY
DrvEnableDriver(
    ULONG iEngineVersion,
    ULONG cj,
    PDRVENABLEDATA pded)
{
    if (cj < sizeof(DRVENABLEDATA) || !pded)
        return FALSE;

    pded->c = sizeof(DrvFunctionTable) / sizeof(DRVFN);
    pded->pdrvfn = DrvFunctionTable;
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5;

    return TRUE;
}

VOID APIENTRY
DrvDisableDriver(VOID)
{
}
