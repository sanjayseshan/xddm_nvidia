/*
 * Antigravity GPU Acceleration Verification Suite
 * Tests: 2D GDI paths, OpenGL renderer identity/presentation, and D3D9 availability.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>
#include "../src_driver/nvx_3d_abi.h"
#if defined(NVX_TEST_NOUVEAU_PUSHBUF)
#include "../mesa_patches/nvx_nouveau_pushbuf.h"
#include "../mesa_patches/nvx_winsys_win32.h"
#endif

#define NVX_OPENGL_GETINFO_ESCAPE 4353
#define NVX_OPENGL_GETINFO_DRVNAME 0

typedef struct NVX_OPENGL_INFO {
    DWORD version;
    DWORD driverVersion;
    WCHAR driverName[MAX_PATH + 1];
} NVX_OPENGL_INFO;

static HANDLE hCom1 = INVALID_HANDLE_VALUE;
static FILE *fLog = NULL;
static int failures = 0;
static int software3d = 0;
static int d3dUnavailable = 0;
static int mesaGpuFramebufferVerified = 0;
static unsigned mesaGpuBridgeBatchCount = 0;
#if defined(NVX_MESA_GALLIUM_BRIDGE_PROBE) && NVX_MESA_GALLIUM_BRIDGE_PROBE
static int mesaGpuBridgeProbe = 1;
#else
static int mesaGpuBridgeProbe = 0;
#endif
static unsigned openGlTraceCount;
static void LogMsg(const char *fmt, ...);
static uint64_t nvx_test_client_token;
static uint64_t nvx_test_allocation_token;
static uint64_t nvx_test_request_token;

static uint64_t
NvxTestGetClientToken(HDC hdc)
{
    NVX_3D_CLIENT_SESSION session={0};
    if(nvx_test_client_token) return nvx_test_client_token;
    session.Size=sizeof(session);
    session.Version=NVX_3D_ABI_VERSION;
    session.Operation=NVX_3D_CLIENT_OPEN;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_CLIENT_OPEN,sizeof(session),
                  (LPCSTR)&session,sizeof(session),(LPSTR)&session)||
       !Nvx3dValidateClientSession(&session,NVX_3D_CLIENT_OPEN)||
       !session.ClientToken)
        return 0;
    nvx_test_client_token=session.ClientToken;
    return nvx_test_client_token;
}

static void
NvxTestCloseClient(HDC hdc)
{
    NVX_3D_CLIENT_SESSION session={0};
    if(!nvx_test_client_token) return;
    session.Size=sizeof(session);
    session.Version=NVX_3D_ABI_VERSION;
    session.Operation=NVX_3D_CLIENT_CLOSE;
    session.ClientToken=nvx_test_client_token;
    if(ExtEscape(hdc,NVX_ESCAPE_3D_CLIENT_CLOSE,sizeof(session),
                 (LPCSTR)&session,0,NULL))
        nvx_test_client_token=0;
}

static int
NvxTestExtEscape(HDC hdc, int escape, int input_bytes, LPCSTR input,
                 int output_bytes, LPSTR output)
{
    if(escape==NVX_ESCAPE_3D_ALLOC && input &&
       input_bytes==(int)sizeof(NVX_3D_ALLOC_BUFFER)) {
        NVX_3D_ALLOC_BUFFER *request=(NVX_3D_ALLOC_BUFFER *)input;
        request->ClientToken=NvxTestGetClientToken(hdc);
        if(!request->ClientToken) return 0;
        if(++nvx_test_allocation_token==0) ++nvx_test_allocation_token;
        request->AllocationToken=nvx_test_allocation_token;
    } else if(escape==NVX_ESCAPE_3D_SUBMIT && input &&
              input_bytes>=(int)sizeof(NVX_3D_SUBMIT)) {
        NVX_3D_SUBMIT *request=(NVX_3D_SUBMIT *)input;
        request->ClientToken=NvxTestGetClientToken(hdc);
        if(!request->ClientToken) return 0;
        if(++nvx_test_request_token==0) ++nvx_test_request_token;
        request->RequestToken=nvx_test_request_token;
    } else if(escape==NVX_ESCAPE_3D_FREE && input &&
              input_bytes==(int)sizeof(NVX_3D_FREE_BUFFER)) {
        NVX_3D_FREE_BUFFER *request=(NVX_3D_FREE_BUFFER *)input;
        request->ClientToken=NvxTestGetClientToken(hdc);
    } else if(escape==NVX_ESCAPE_3D_UPLOAD && input &&
              input_bytes>=(int)sizeof(NVX_3D_UPLOAD_BUFFER)) {
        NVX_3D_UPLOAD_BUFFER *request=(NVX_3D_UPLOAD_BUFFER *)input;
        request->ClientToken=NvxTestGetClientToken(hdc);
    } else if(escape==NVX_ESCAPE_3D_DOWNLOAD && input &&
              input_bytes==(int)sizeof(NVX_3D_DOWNLOAD_BUFFER)) {
        NVX_3D_DOWNLOAD_BUFFER *request=(NVX_3D_DOWNLOAD_BUFFER *)input;
        request->ClientToken=NvxTestGetClientToken(hdc);
    }
    return ExtEscape(hdc,escape,input_bytes,input,output_bytes,output);
}

#define ExtEscape NvxTestExtEscape

static void DumpMesaBridgeTrace(void)
{
    typedef struct MESA_BRIDGE_STATUS {
        DWORD size;
        DWORD version;
        LONG screenCount;
        LONG callbackCount;
        LONG gpuBatchCount;
        LONG nvc0ClearCount;
        LONG lastPrimitive;
        LONG lastCount;
        LONG lastTriangleCount;
        LONG lastClearMs;
        LONG lastDrawMs;
        LONG lastReadbackMs;
        LONG lastUploadBits[8];
        char lastMessage[96];
        LONG depthStageCount;
        char depthStages[3][96];
        LONG nouveauDeviceReady;
        LONG nouveauDeviceId;
        LONG nouveauChipset;
        LONG nouveauApertureLow;
        LONG nouveauApertureHigh;
        DWORD xf86drmUnsupportedCount;
        DWORD xf86drmLastUnsupportedKind;
        DWORD xf86drmLastUnsupportedOperation;
        DWORD xf86drmChannelAllocCount;
        DWORD xf86drmLastChannelAllocResult;
        DWORD xf86drmLastChannelHandle;
        DWORD xf86drmGemNewCount;
        DWORD xf86drmLastGemNewResult;
        DWORD xf86drmLastGemNewDomain;
        DWORD xf86drmLastGemNewTileMode;
        DWORD xf86drmLastGemNewTileFlags;
        uint64_t xf86drmLastGemNewSize;
        DWORD xf86drmSclassCalls;
        DWORD xf86drmLastSclassResult;
        DWORD xf86drmLastSclassCount;
        DWORD xf86drmLastSclass[16];
    } MESA_BRIDGE_STATUS;
    typedef BOOL (WINAPI *PFN_QUERY_BRIDGE_STATUS)(MESA_BRIDGE_STATUS *, DWORD);
    HMODULE icd = GetModuleHandleA("nvxogl64.dll");
    PFN_QUERY_BRIDGE_STATUS query = icd ?
        (PFN_QUERY_BRIDGE_STATUS)GetProcAddress(icd, "NvxQueryBridgeStatus") : NULL;
    MESA_BRIDGE_STATUS status;
    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (!query || !query(&status, sizeof(status))) {
        LogMsg("[MESA_BRIDGE_TRACE] ICD status export unavailable (icd=%s)\n",
               icd ? "loaded" : "not loaded");
        return;
    }
    mesaGpuBridgeBatchCount = (unsigned)status.gpuBatchCount;
    mesaGpuFramebufferVerified = mesaGpuBridgeBatchCount != 0;
    LogMsg("[MESA_BRIDGE_STATUS] screens=%ld callbacks=%ld gpu_batches=%ld nvc0_clears=%ld version=%lu last_primitive=%ld last_count=%ld last_triangles=%ld clear_ms=%ld draw_ms=%ld readback_ms=%ld last=%s\n",
           status.screenCount, status.callbackCount, status.gpuBatchCount,
           status.nvc0ClearCount, (unsigned long)status.version,
           status.lastPrimitive,
           status.lastCount, status.lastTriangleCount, status.lastClearMs,
           status.lastDrawMs, status.lastReadbackMs, status.lastMessage);
    LogMsg("[NVX_NOUVEAU_DEVICE] ready=%ld PCI=%04lx chipset=%04lx VRAM-aperture=%08lx%08lx\n",
           status.nouveauDeviceReady,
           (unsigned long)(DWORD)status.nouveauDeviceId,
           (unsigned long)(DWORD)status.nouveauChipset,
           (unsigned long)(DWORD)status.nouveauApertureHigh,
           (unsigned long)(DWORD)status.nouveauApertureLow);
    LogMsg("[NVX_XF86DRM] unsupported_count=%lu last_kind=%lu last_operation=%08lx channel_allocs=%lu channel_result=%lu channel=%08lx%s\n",
           (unsigned long)status.xf86drmUnsupportedCount,
           (unsigned long)status.xf86drmLastUnsupportedKind,
           (unsigned long)status.xf86drmLastUnsupportedOperation,
           (unsigned long)status.xf86drmChannelAllocCount,
           (unsigned long)status.xf86drmLastChannelAllocResult,
           (unsigned long)status.xf86drmLastChannelHandle,
           status.xf86drmLastUnsupportedKind == 2 &&
              status.xf86drmLastUnsupportedOperation == 2 ?
              " (NOUVEAU_CHANNEL_ALLOC)" : "");
    LogMsg("[NVX_XF86DRM_GEM_NEW] count=%lu result=%lu size=%08lx%08lx domain=%08lx tile_mode=%08lx tile_flags=%08lx\n",
           (unsigned long)status.xf86drmGemNewCount,
           (unsigned long)status.xf86drmLastGemNewResult,
           (unsigned long)(DWORD)(status.xf86drmLastGemNewSize >> 32),
           (unsigned long)(DWORD)status.xf86drmLastGemNewSize,
           (unsigned long)status.xf86drmLastGemNewDomain,
           (unsigned long)status.xf86drmLastGemNewTileMode,
           (unsigned long)status.xf86drmLastGemNewTileFlags);
    LogMsg("[NVX_XF86DRM_SCLASS] calls=%lu result=%lu count=%lu classes=%08lx,%08lx,%08lx,%08lx\n",
           (unsigned long)status.xf86drmSclassCalls,
           (unsigned long)status.xf86drmLastSclassResult,
           (unsigned long)status.xf86drmLastSclassCount,
           (unsigned long)status.xf86drmLastSclass[0],
           (unsigned long)status.xf86drmLastSclass[1],
           (unsigned long)status.xf86drmLastSclass[2],
           (unsigned long)status.xf86drmLastSclass[3]);
    LogMsg("[MESA_BRIDGE_UPLOAD_BITS] %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
           (unsigned long)(DWORD)status.lastUploadBits[0],
           (unsigned long)(DWORD)status.lastUploadBits[1],
           (unsigned long)(DWORD)status.lastUploadBits[2],
           (unsigned long)(DWORD)status.lastUploadBits[3],
           (unsigned long)(DWORD)status.lastUploadBits[4],
           (unsigned long)(DWORD)status.lastUploadBits[5],
           (unsigned long)(DWORD)status.lastUploadBits[6],
           (unsigned long)(DWORD)status.lastUploadBits[7]);
    for (LONG i = 0; i < status.depthStageCount && i < 3; i++)
        LogMsg("[MESA_BRIDGE_DEPTH_STAGE] %s\n", status.depthStages[i]);
}

#if defined(_WIN64)
enum OPENGL_TRACE_KIND {
    OPENGL_TRACE_LOAD = 1,
    OPENGL_TRACE_REG_OPEN,
    OPENGL_TRACE_REG_QUERY,
    OPENGL_TRACE_ESCAPE,
    OPENGL_TRACE_PROC
};

typedef struct OPENGL_TRACE_EVENT {
    enum OPENGL_TRACE_KIND kind;
    LONG result;
    char detail[96];
} OPENGL_TRACE_EVENT;

static OPENGL_TRACE_EVENT openGlTrace[1024];

static void TraceOpenGlCall(enum OPENGL_TRACE_KIND kind, LONG result,
                            const char *detail)
{
    OPENGL_TRACE_EVENT *event;
    if (openGlTraceCount >= sizeof(openGlTrace) / sizeof(openGlTrace[0]))
        return;
    event = &openGlTrace[openGlTraceCount++];
    event->kind = kind;
    event->result = result;
    size_t length = detail ? strlen(detail) : 0;
    if (length >= sizeof(event->detail)) length = sizeof(event->detail) - 1;
    if (length) memcpy(event->detail, detail, length);
    event->detail[length] = '\0';
}

static void WideToAscii(char *dst, size_t dstBytes, LPCWSTR src)
{
    size_t i = 0;
    if (!dstBytes) return;
    while (src && src[i] && i + 1 < dstBytes) {
        dst[i] = src[i] < 0x80 ? (char)src[i] : '?';
        ++i;
    }
    dst[i] = '\0';
}

typedef HMODULE (WINAPI *PFN_LOAD_LIBRARY_W)(LPCWSTR);
typedef LONG (WINAPI *PFN_REG_OPEN_KEY_EX_W)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *PFN_REG_QUERY_VALUE_EX_W)(HKEY, LPCWSTR, LPDWORD, LPDWORD,
                                                LPBYTE, LPDWORD);
typedef int (WINAPI *PFN_EXT_ESCAPE)(HDC, int, int, LPCSTR, int, LPSTR);
typedef FARPROC (WINAPI *PFN_GET_PROC_ADDRESS)(HMODULE, LPCSTR);
typedef BOOL (APIENTRY *PFN_DRV_VALIDATE_VERSION)(ULONG);
typedef VOID (APIENTRY *PFN_DRV_SET_CALLBACK_PROCS)(INT, PROC *);
typedef LONG (APIENTRY *PFN_DRV_DESCRIBE_PIXEL_FORMAT)(HDC, INT, ULONG,
                                                       PIXELFORMATDESCRIPTOR *);
typedef BOOL (APIENTRY *PFN_DRV_SET_PIXEL_FORMAT)(HDC, LONG);
typedef ULONG_PTR (APIENTRY *PFN_DRV_CREATE_CONTEXT)(HDC);
typedef ULONG_PTR (APIENTRY *PFN_DRV_CREATE_LAYER_CONTEXT)(HDC, INT);
typedef PVOID (APIENTRY *PFN_DRV_SET_CONTEXT)(HDC, ULONG_PTR, PVOID);
typedef BOOL (APIENTRY *PFN_DRV_RELEASE_CONTEXT)(ULONG_PTR);
typedef BOOL (APIENTRY *PFN_DRV_DELETE_CONTEXT)(ULONG_PTR);
typedef BOOL (APIENTRY *PFN_DRV_SWAP_BUFFERS)(HDC);
typedef PROC (APIENTRY *PFN_DRV_GET_PROC_ADDRESS)(LPCSTR);

static PFN_LOAD_LIBRARY_W realLoadLibraryW;
static PFN_REG_OPEN_KEY_EX_W realRegOpenKeyExW;
static PFN_REG_QUERY_VALUE_EX_W realRegQueryValueExW;
static PFN_EXT_ESCAPE realExtEscape;
static PFN_GET_PROC_ADDRESS realGetProcAddress;
static PFN_DRV_VALIDATE_VERSION realDrvValidateVersion;
static PFN_DRV_SET_CALLBACK_PROCS realDrvSetCallbackProcs;
static PFN_DRV_DESCRIBE_PIXEL_FORMAT realDrvDescribePixelFormat;
static PFN_DRV_SET_PIXEL_FORMAT realDrvSetPixelFormat;
static PFN_DRV_CREATE_CONTEXT realDrvCreateContext;
static PFN_DRV_CREATE_LAYER_CONTEXT realDrvCreateLayerContext;
static PFN_DRV_SET_CONTEXT realDrvSetContext;
static PFN_DRV_RELEASE_CONTEXT realDrvReleaseContext;
static PFN_DRV_DELETE_CONTEXT realDrvDeleteContext;
static PFN_DRV_SWAP_BUFFERS realDrvSwapBuffers;
static PFN_DRV_GET_PROC_ADDRESS realDrvGetProcAddress;
static BOOL traceWrapIcdCallbacks;

static PROC APIENTRY TraceDrvGetProcAddress(LPCSTR name)
{
    PROC result = realDrvGetProcAddress(name);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvGetProcAddress name=%s result=%p",
              name ? name : "(null)", result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result != NULL, text);
    return result;
}

static BOOL APIENTRY TraceDrvValidateVersion(ULONG version)
{
    BOOL result = realDrvValidateVersion(version);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvValidateVersion version=%lu result=%u",
              (unsigned long)version, (unsigned)result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result, text);
    LogMsg("-> %s\n", text);
    return result;
}

static VOID APIENTRY TraceDrvSetCallbackProcs(INT count, PROC *callbacks)
{
    LogMsg("-> CALL DrvSetCallbackProcs count=%d callbacks=%p\n", count, callbacks);
    TraceOpenGlCall(OPENGL_TRACE_PROC, count, "CALL DrvSetCallbackProcs");
    realDrvSetCallbackProcs(count, callbacks);
}

static LONG APIENTRY TraceDrvDescribePixelFormat(HDC hdc, INT format, ULONG bytes,
                                                 PIXELFORMATDESCRIPTOR *pfd)
{
    LONG result = realDrvDescribePixelFormat(hdc, format, bytes, pfd);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvDescribePixelFormat i=%d bytes=%lu result=%ld",
              format, (unsigned long)bytes, result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result, text);
    LogMsg("-> %s\n", text);
    return result;
}

static BOOL APIENTRY TraceDrvSetPixelFormat(HDC hdc, LONG format)
{
    BOOL result = realDrvSetPixelFormat(hdc, format);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvSetPixelFormat i=%ld result=%u",
              format, (unsigned)result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result, text);
    LogMsg("-> %s\n", text);
    return result;
}

static ULONG_PTR APIENTRY TraceDrvCreateContext(HDC hdc)
{
    ULONG_PTR result = realDrvCreateContext(hdc);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvCreateContext hdc=%p result=%p",
              hdc, (void *)(UINT_PTR)result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result != 0, text);
    LogMsg("-> %s\n", text);
    return result;
}

static ULONG_PTR APIENTRY TraceDrvCreateLayerContext(HDC hdc, INT layer)
{
    ULONG_PTR result = realDrvCreateLayerContext(hdc, layer);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvCreateLayerContext layer=%d result=%p",
              layer, (void *)(UINT_PTR)result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result != 0, text);
    return result;
}

static PVOID APIENTRY TraceDrvSetContext(HDC hdc, ULONG_PTR context, PVOID table)
{
    PVOID result = realDrvSetContext(hdc, context, table);
    char text[96];
    _snprintf(text, sizeof(text), "CALL DrvSetContext ctx=%p result=%p",
              (void *)(UINT_PTR)context, result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_PROC, result != NULL, text);
    LogMsg("-> %s\n", text);
    return result;
}

static BOOL APIENTRY TraceDrvReleaseContext(ULONG_PTR context)
{
    BOOL result = realDrvReleaseContext(context);
    TraceOpenGlCall(OPENGL_TRACE_PROC, result, "CALL DrvReleaseContext");
    return result;
}

static BOOL APIENTRY TraceDrvDeleteContext(ULONG_PTR context)
{
    BOOL result = realDrvDeleteContext(context);
    TraceOpenGlCall(OPENGL_TRACE_PROC, result, "CALL DrvDeleteContext");
    return result;
}

static BOOL APIENTRY TraceDrvSwapBuffers(HDC hdc)
{
    BOOL result = realDrvSwapBuffers(hdc);
    TraceOpenGlCall(OPENGL_TRACE_PROC, result, "CALL DrvSwapBuffers");
    return result;
}

static HMODULE WINAPI TraceLoadLibraryW(LPCWSTR name)
{
    HMODULE result = realLoadLibraryW(name);
    char text[96];
    WideToAscii(text, sizeof(text), name);
    TraceOpenGlCall(OPENGL_TRACE_LOAD, result ? 1 : (LONG)GetLastError(), text);
    return result;
}

static LONG WINAPI TraceRegOpenKeyExW(HKEY root, LPCWSTR subkey, DWORD options,
                                     REGSAM access, PHKEY resultKey)
{
    LONG result = realRegOpenKeyExW(root, subkey, options, access, resultKey);
    char text[96];
    WideToAscii(text, sizeof(text), subkey);
    TraceOpenGlCall(OPENGL_TRACE_REG_OPEN, result, text);
    return result;
}

static LONG WINAPI TraceRegQueryValueExW(HKEY key, LPCWSTR value, LPDWORD reserved,
                                        LPDWORD type, LPBYTE data, LPDWORD bytes)
{
    LONG result = realRegQueryValueExW(key, value, reserved, type, data, bytes);
    char text[96];
    WideToAscii(text, sizeof(text), value);
    TraceOpenGlCall(OPENGL_TRACE_REG_QUERY, result, text);
    return result;
}

static int WINAPI TraceExtEscape(HDC hdc, int escape, int inputBytes,
                                LPCSTR input, int outputBytes, LPSTR output)
{
    int result = realExtEscape(hdc, escape, inputBytes, input, outputBytes, output);
    char text[96];
    DWORD subEscape = 0;
    if (input && inputBytes >= (int)sizeof(subEscape))
        memcpy(&subEscape, input, sizeof(subEscape));
    _snprintf(text, sizeof(text), "escape=%d sub=%lu bytes=%d", escape,
              (unsigned long)subEscape, result);
    text[sizeof(text) - 1] = '\0';
    TraceOpenGlCall(OPENGL_TRACE_ESCAPE, result, text);
    return result;
}

static FARPROC WINAPI TraceGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC result = realGetProcAddress(module, name);
    char modulePath[MAX_PATH];
    char *base = modulePath;
    BOOL isNvxIcd = FALSE;
    if (module && GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
        char *p;
        for (p = modulePath; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        isNvxIcd = _stricmp(base, "nvxogl64.dll") == 0;
    }
    if (name && strncmp(name, "Drv", 3) == 0) {
        char detail[96];
        _snprintf(detail, sizeof(detail), "%s module=%s icd=%u", name,
                  isNvxIcd ? base : "other", (unsigned)isNvxIcd);
        detail[sizeof(detail) - 1] = '\0';
        TraceOpenGlCall(OPENGL_TRACE_PROC, result ? 1 : 0, detail);
        if (isNvxIcd && result) {
            if (strcmp(name, "DrvValidateVersion") == 0) {
                realDrvValidateVersion = (PFN_DRV_VALIDATE_VERSION)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvValidateVersion;
            }
            if (strcmp(name, "DrvSetCallbackProcs") == 0) {
                realDrvSetCallbackProcs = (PFN_DRV_SET_CALLBACK_PROCS)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvSetCallbackProcs;
            }
            if (strcmp(name, "DrvDescribePixelFormat") == 0) {
                realDrvDescribePixelFormat = (PFN_DRV_DESCRIBE_PIXEL_FORMAT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvDescribePixelFormat;
            }
            if (strcmp(name, "DrvSetPixelFormat") == 0) {
                realDrvSetPixelFormat = (PFN_DRV_SET_PIXEL_FORMAT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvSetPixelFormat;
            }
            if (strcmp(name, "DrvCreateContext") == 0) {
                realDrvCreateContext = (PFN_DRV_CREATE_CONTEXT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvCreateContext;
            }
            if (strcmp(name, "DrvCreateLayerContext") == 0) {
                realDrvCreateLayerContext = (PFN_DRV_CREATE_LAYER_CONTEXT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvCreateLayerContext;
            }
            if (strcmp(name, "DrvSetContext") == 0) {
                realDrvSetContext = (PFN_DRV_SET_CONTEXT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvSetContext;
            }
            if (strcmp(name, "DrvReleaseContext") == 0) {
                realDrvReleaseContext = (PFN_DRV_RELEASE_CONTEXT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvReleaseContext;
            }
            if (strcmp(name, "DrvDeleteContext") == 0) {
                realDrvDeleteContext = (PFN_DRV_DELETE_CONTEXT)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvDeleteContext;
            }
            if (strcmp(name, "DrvSwapBuffers") == 0) {
                realDrvSwapBuffers = (PFN_DRV_SWAP_BUFFERS)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvSwapBuffers;
            }
            if (strcmp(name, "DrvGetProcAddress") == 0) {
                realDrvGetProcAddress = (PFN_DRV_GET_PROC_ADDRESS)result;
                if (traceWrapIcdCallbacks)
                    return (FARPROC)TraceDrvGetProcAddress;
            }
        }
    }
    return result;
}

static BOOL HookOpenGl32Import(const char *dllName, const char *functionName,
                               FARPROC hook, FARPROC *original)
{
    HMODULE module = GetModuleHandleA("opengl32.dll");
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_IMPORT_DESCRIPTOR imports;
    DWORD importRva;
    if (!module) return FALSE;
    dos = (PIMAGE_DOS_HEADER)module;
    nt = (PIMAGE_NT_HEADERS)((BYTE *)module + dos->e_lfanew);
    importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva) return FALSE;
    imports = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)module + importRva);
    for (; imports->Name; ++imports) {
        const char *importDll = (const char *)module + imports->Name;
        PIMAGE_THUNK_DATA64 names;
        PIMAGE_THUNK_DATA64 addresses;
        if (_stricmp(importDll, dllName) != 0 || !imports->OriginalFirstThunk)
            continue;
        names = (PIMAGE_THUNK_DATA64)((BYTE *)module + imports->OriginalFirstThunk);
        addresses = (PIMAGE_THUNK_DATA64)((BYTE *)module + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            PIMAGE_IMPORT_BY_NAME entry;
            DWORD oldProtect;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            entry = (PIMAGE_IMPORT_BY_NAME)((BYTE *)module + names->u1.AddressOfData);
            if (strcmp((const char *)entry->Name, functionName) != 0) continue;
            *original = (FARPROC)(UINT_PTR)addresses->u1.Function;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                                PAGE_EXECUTE_READWRITE, &oldProtect))
                return FALSE;
            addresses->u1.Function = (ULONGLONG)(UINT_PTR)hook;
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                           oldProtect, &oldProtect);
            return TRUE;
        }
    }
    return FALSE;
}

static void InstallOpenGl32TraceHooks(void)
{
    BOOL ok = TRUE;
    char wrapValue[2];
    char getProcTraceValue[2];
    traceWrapIcdCallbacks = GetEnvironmentVariableA(
        "NVX_TRACE_WRAP_ICD_CALLBACKS", wrapValue, sizeof(wrapValue)) > 0;
    if (GetEnvironmentVariableA("NVX_TRACE_GETPROCADDRESS",
                                getProcTraceValue,
                                sizeof(getProcTraceValue)) > 0
#if defined(NVX_TRACE_ICD_CALLBACKS)
        || TRUE
#endif
        ) {
#if defined(NVX_TRACE_ICD_CALLBACKS)
        traceWrapIcdCallbacks = TRUE;
#endif
        ok &= HookOpenGl32Import("KERNEL32.dll", "GetProcAddress",
                                 (FARPROC)TraceGetProcAddress,
                                 (FARPROC *)&realGetProcAddress);
    }
    ok &= HookOpenGl32Import("KERNEL32.dll", "LoadLibraryW",
                             (FARPROC)TraceLoadLibraryW, (FARPROC *)&realLoadLibraryW);
    ok &= HookOpenGl32Import("ADVAPI32.dll", "RegOpenKeyExW",
                             (FARPROC)TraceRegOpenKeyExW, (FARPROC *)&realRegOpenKeyExW);
    ok &= HookOpenGl32Import("ADVAPI32.dll", "RegQueryValueExW",
                             (FARPROC)TraceRegQueryValueExW, (FARPROC *)&realRegQueryValueExW);
    ok &= HookOpenGl32Import("GDI32.dll", "ExtEscape",
                             (FARPROC)TraceExtEscape, (FARPROC *)&realExtEscape);
    LogMsg("-> WGL dispatcher import tracing: %s\n", ok ? "installed" : "partial");
}

static void DumpOpenGlTrace(unsigned start)
{
    static const char *kindNames[] = { "?", "LoadLibraryW", "RegOpenKeyExW",
        "RegQueryValueExW", "ExtEscape", "GetProcAddress" };
    unsigned i;
    for (i = start; i < openGlTraceCount; ++i) {
        OPENGL_TRACE_EVENT *event = &openGlTrace[i];
        LogMsg("-> WGL trace %s result=%ld detail=%s\n",
               kindNames[event->kind], event->result, event->detail);
    }
}
#else
static void InstallOpenGl32TraceHooks(void) {}
static void DumpOpenGlTrace(unsigned start) { (void)start; }
#endif

static void LogMsg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len > 0) {
        printf("%s", buf);
        fflush(stdout);

        OutputDebugStringA(buf);

        if (fLog) {
            fputs(buf, fLog);
            fflush(fLog);
        }

        if (hCom1 != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hCom1, buf, (DWORD)strlen(buf), &written, NULL);
        }
    }
}

static unsigned ReadRawMesaTargetGreenPixels(uint32_t draw_kind,
                                              uint32_t *blue_pixels,
                                              uint64_t *target_va,
                                              uint64_t *buffer_id,
                                              uint32_t *buffer_bytes,
                                              int *available)
{
    typedef int (WINAPI *PFN_READ_TARGET_RAW)(uint32_t, uint32_t *,
                                               uint32_t *, uint32_t *,
                                               uint32_t *,
                                               uint64_t *, uint64_t *);
    HMODULE icd = GetModuleHandleA("nvxogl64.dll");
    PFN_READ_TARGET_RAW readTarget = icd ?
        (PFN_READ_TARGET_RAW)GetProcAddress(
            icd, "NvxXf86DrmReadDrawTargetRaw") : NULL;
    unsigned green = 0;
    unsigned blueRgba = 0, blueBgra = 0;

    if (buffer_id) *buffer_id = 0;
    if (target_va) *target_va = 0;
    if (available) *available = readTarget != NULL;
    if (buffer_bytes) *buffer_bytes = 0;
    if (blue_pixels) *blue_pixels = 0;
    if (!readTarget)
        return 0;
    if (!readTarget(draw_kind, &green, &blueRgba, &blueBgra, buffer_bytes,
                    target_va, buffer_id))
        return 0;
    if (blue_pixels) *blue_pixels = blueRgba + blueBgra;
    return green;
}

static void ClearMesaIcdTrace(void)
{
    DWORD deleteError;
    SetEnvironmentVariableA("NVX_MESA_ICD_TRACE", "1");
    if (!DeleteFileA("C:\\test_accel\\nvxogl_trace.txt"))
        deleteError = GetLastError();
    else
        deleteError = ERROR_SUCCESS;
    LogMsg("[MESA_ICD_TRACE_RESET] delete_error=%lu\n", deleteError);
}

static void DumpMesaIcdTrace(void)
{
    char buffer[4096];
    DWORD bytesRead = 0;
    HANDLE file = CreateFileA("C:\\test_accel\\nvxogl_trace.txt",
                              GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LogMsg("-> Mesa ICD callback trace: no callbacks recorded (file unavailable)\n");
        return;
    }
    if (ReadFile(file, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
        buffer[bytesRead] = '\0';
        LogMsg("-> Mesa ICD callback trace:\n%s\n", buffer);
    }
    CloseHandle(file);
}

static void DumpMesaRuntimeTrace(const char *path, const char *label)
{
    char buffer[513];
    DWORD total = 0;
    HANDLE file = CreateFileA(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LogMsg("[%s] unavailable (error=%lu)\n", label, GetLastError());
        return;
    }

    LogMsg("[%s] begin\n", label);
    for (;;) {
        DWORD bytesRead = 0;
        DWORD request = sizeof(buffer) - 1;
        if (total >= 8192)
            break;
        if (request > 8192 - total)
            request = 8192 - total;
        if (!ReadFile(file, buffer, request, &bytesRead, NULL) || !bytesRead)
            break;
        buffer[bytesRead] = '\0';
        LogMsg("%s", buffer);
        total += bytesRead;
    }
    if (total >= 8192)
        LogMsg("\n[%s] truncated at 8192 bytes\n", label);
    else
        LogMsg("\n[%s] end (%lu bytes)\n", label, (unsigned long)total);
    CloseHandle(file);
}

static void DumpMesaRuntimeTraces(void)
{
    DumpMesaRuntimeTrace("C:\\test_accel\\mesa_bridge.log",
                         "MESA_BRIDGE_RUNTIME_TRACE");
    DumpMesaRuntimeTrace("C:\\test_accel\\nvx_nouveau_trace.log",
                         "NOUVEAU_RUNTIME_TRACE");
}

static void InitLogging(void)
{
    hCom1 = CreateFileA("\\\\.\\COM1", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    fLog = _fsopen("C:\\test_accel.log", "a+", _SH_DENYNO);
}

static void CloseLogging(void)
{
    if (fLog) { fclose(fLog); fLog = NULL; }
    if (hCom1 != INVALID_HANDLE_VALUE) { CloseHandle(hCom1); hCom1 = INVALID_HANDLE_VALUE; }
}

static double GetTimeSeconds(void)
{
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}

/* ---------------- 2D GDI Acceleration Tests ---------------- */
static void Test2D(HWND hwnd, HDC hdc)
{
    LogMsg("\n==================================================\n");
    LogMsg(" [TEST 1/3] 2D GDI Hardware Acceleration\n");
    LogMsg("==================================================\n");

    RECT client;
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    LogMsg("-> Test Window Client Area: %dx%d\n", width, height);

    /* Test 1A: Solid Fills (WHITENESS, BLACKNESS, PATCOPY) */
    HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
    HBRUSH blueBrush = CreateSolidBrush(RGB(0, 0, 255));
    HBRUSH greenBrush = CreateSolidBrush(RGB(0, 255, 0));

    const int FILL_ITERS = 100;
    double t0 = GetTimeSeconds();

    for (int i = 0; i < FILL_ITERS; i++) {
        /* Alternate colors */
        HBRUSH cur = (i % 3 == 0) ? redBrush : ((i % 3 == 1) ? greenBrush : blueBrush);
        SelectObject(hdc, cur);
        PatBlt(hdc, 0, 0, width, height, PATCOPY);

        /* Test WHITENESS and BLACKNESS in sub-rectangles */
        BitBlt(hdc, 10, 10, 100, 100, NULL, 0, 0, WHITENESS);
        BitBlt(hdc, 120, 10, 100, 100, NULL, 0, 0, BLACKNESS);
    }
    GdiFlush();
    double t1 = GetTimeSeconds();

    double dt = t1 - t0;
    double fillMpix = ((double)width * height * FILL_ITERS) / (dt * 1000000.0);
    LogMsg("-> Solid Fills (%d iterations): %.4f s (%.2f MPix/s)\n", FILL_ITERS, dt, fillMpix);

    /* Verify fill pixel */
    COLORREF cWhite = GetPixel(hdc, 15, 15);
    COLORREF cBlack = GetPixel(hdc, 125, 15);
    LogMsg("-> Fill Verification: Black=0x%06X (expected 0x000000), White=0x%06X (expected 0xFFFFFF)\n",
           (unsigned int)cBlack, (unsigned int)cWhite);

    if (cBlack != RGB(0,0,0) || cWhite != RGB(255,255,255)) {
        LogMsg("-> [FAIL] Fill pixel readback mismatch\n");
        failures++;
    }

    /* Test 1B: Screen-to-Screen SRCCOPY Blits */
    const int BLT_ITERS = 100;
    double tBlt0 = GetTimeSeconds();

    for (int i = 0; i < BLT_ITERS; i++) {
        /* Blit left half to right half */
        BitBlt(hdc, width / 2, 0, width / 2, height, hdc, 0, 0, SRCCOPY);
    }
    GdiFlush();
    double tBlt1 = GetTimeSeconds();

    double dtBlt = tBlt1 - tBlt0;
    double bltMpix = ((double)(width / 2) * height * BLT_ITERS) / (dtBlt * 1000000.0);
    LogMsg("-> Screen-to-Screen Blits (%d iterations): %.4f s (%.2f MPix/s)\n", BLT_ITERS, dtBlt, bltMpix);

    /* Vertically disjoint spans exercise the pitched GPU-copy fast path;
       side-by-side rectangles share a bounding span and use the GDI fallback. */
    if (!BitBlt(hdc, 0, height/2+10, width, height/2-10, hdc, 0, 0, SRCCOPY)) {
        LogMsg("-> [FAIL] Vertical SRCCOPY API failure\n"); failures++;
    }
    GdiFlush();
    if (GetPixel(hdc, 15, height/2+25) != RGB(255,255,255) ||
        GetPixel(hdc, 125, height/2+25) != RGB(0,0,0)) {
        LogMsg("-> [FAIL] Vertical SRCCOPY pixel mismatch\n"); failures++;
    } else LogMsg("-> [PASS] Vertical SRCCOPY pixels (GPU-eligible disjoint spans).\n");

    DeleteObject(redBrush);
    DeleteObject(blueBrush);
    DeleteObject(greenBrush);

    cWhite = GetPixel(hdc, width/2+15, 15);
    cBlack = GetPixel(hdc, width/2+125, 15);
    if (cWhite != RGB(255,255,255) || cBlack != RGB(0,0,0)) {
        LogMsg("-> [FAIL] SRCCOPY pixel readback mismatch: white=%08lX black=%08lX\n",
            (unsigned long)cWhite, (unsigned long)cBlack);
        failures++;
    } else {
        LogMsg("-> [PASS] SRCCOPY pixel readback; correlate GPU Copy submissions with miniport log.\n");
    }
}

/* ---------------- OpenGL 3D Pipeline Tests ---------------- */
/* Direct DrvSetContext calls bypass GDI's installation of opengl32's client
 * dispatch table. Resolve GL entry points from the ICD for that diagnostic
 * path; ordinary WGL continues through the system opengl32 imports. */
typedef struct NVX_DIRECT_GL_API {
    __typeof__(&glBegin) Begin;
    __typeof__(&glEnd) End;
    __typeof__(&glVertex3f) Vertex3f;
    __typeof__(&glColor3f) Color3f;
    __typeof__(&glColor4f) Color4f;
    __typeof__(&glPushMatrix) PushMatrix;
    __typeof__(&glPopMatrix) PopMatrix;
    __typeof__(&glRotatef) Rotatef;
    __typeof__(&glGetString) GetString;
    __typeof__(&glViewport) Viewport;
    __typeof__(&glMatrixMode) MatrixMode;
    __typeof__(&glLoadIdentity) LoadIdentity;
    __typeof__(&glDisable) Disable;
    __typeof__(&glEnable) Enable;
    __typeof__(&glClearColor) ClearColor;
    __typeof__(&glClear) Clear;
    __typeof__(&glGetError) GetError;
    __typeof__(&glReadBuffer) ReadBuffer;
    __typeof__(&glReadPixels) ReadPixels;
    __typeof__(&glFinish) Finish;
    __typeof__(&glClearDepth) ClearDepth;
    __typeof__(&glDepthFunc) DepthFunc;
    __typeof__(&glDepthMask) DepthMask;
    __typeof__(&glFrustum) Frustum;
    __typeof__(&glTranslatef) Translatef;
} NVX_DIRECT_GL_API;

static NVX_DIRECT_GL_API nvxDirectGl;

#define glBegin(...)       (nvxDirectGl.Begin ? nvxDirectGl.Begin(__VA_ARGS__) : (glBegin)(__VA_ARGS__))
#define glEnd(...)         (nvxDirectGl.End ? nvxDirectGl.End(__VA_ARGS__) : (glEnd)(__VA_ARGS__))
#define glVertex3f(...)    (nvxDirectGl.Vertex3f ? nvxDirectGl.Vertex3f(__VA_ARGS__) : (glVertex3f)(__VA_ARGS__))
#define glColor3f(...)     (nvxDirectGl.Color3f ? nvxDirectGl.Color3f(__VA_ARGS__) : (glColor3f)(__VA_ARGS__))
#define glColor4f(...)     (nvxDirectGl.Color4f ? nvxDirectGl.Color4f(__VA_ARGS__) : (glColor4f)(__VA_ARGS__))
#define glPushMatrix(...)  (nvxDirectGl.PushMatrix ? nvxDirectGl.PushMatrix(__VA_ARGS__) : (glPushMatrix)(__VA_ARGS__))
#define glPopMatrix(...)   (nvxDirectGl.PopMatrix ? nvxDirectGl.PopMatrix(__VA_ARGS__) : (glPopMatrix)(__VA_ARGS__))
#define glRotatef(...)     (nvxDirectGl.Rotatef ? nvxDirectGl.Rotatef(__VA_ARGS__) : (glRotatef)(__VA_ARGS__))
#define glGetString(...)   (nvxDirectGl.GetString ? nvxDirectGl.GetString(__VA_ARGS__) : (glGetString)(__VA_ARGS__))
#define glViewport(...)    (nvxDirectGl.Viewport ? nvxDirectGl.Viewport(__VA_ARGS__) : (glViewport)(__VA_ARGS__))
#define glMatrixMode(...)  (nvxDirectGl.MatrixMode ? nvxDirectGl.MatrixMode(__VA_ARGS__) : (glMatrixMode)(__VA_ARGS__))
#define glLoadIdentity(...) (nvxDirectGl.LoadIdentity ? nvxDirectGl.LoadIdentity(__VA_ARGS__) : (glLoadIdentity)(__VA_ARGS__))
#define glDisable(...)     (nvxDirectGl.Disable ? nvxDirectGl.Disable(__VA_ARGS__) : (glDisable)(__VA_ARGS__))
#define glEnable(...)      (nvxDirectGl.Enable ? nvxDirectGl.Enable(__VA_ARGS__) : (glEnable)(__VA_ARGS__))
#define glClearColor(...)  (nvxDirectGl.ClearColor ? nvxDirectGl.ClearColor(__VA_ARGS__) : (glClearColor)(__VA_ARGS__))
#define glClear(...)       (nvxDirectGl.Clear ? nvxDirectGl.Clear(__VA_ARGS__) : (glClear)(__VA_ARGS__))
#define glGetError(...)    (nvxDirectGl.GetError ? nvxDirectGl.GetError(__VA_ARGS__) : (glGetError)(__VA_ARGS__))
#define glReadBuffer(...)  (nvxDirectGl.ReadBuffer ? nvxDirectGl.ReadBuffer(__VA_ARGS__) : (glReadBuffer)(__VA_ARGS__))
#define glReadPixels(...)  (nvxDirectGl.ReadPixels ? nvxDirectGl.ReadPixels(__VA_ARGS__) : (glReadPixels)(__VA_ARGS__))
#define glFinish(...)      (nvxDirectGl.Finish ? nvxDirectGl.Finish(__VA_ARGS__) : (glFinish)(__VA_ARGS__))
#define glClearDepth(...)  (nvxDirectGl.ClearDepth ? nvxDirectGl.ClearDepth(__VA_ARGS__) : (glClearDepth)(__VA_ARGS__))
#define glDepthFunc(...)   (nvxDirectGl.DepthFunc ? nvxDirectGl.DepthFunc(__VA_ARGS__) : (glDepthFunc)(__VA_ARGS__))
#define glDepthMask(...)   (nvxDirectGl.DepthMask ? nvxDirectGl.DepthMask(__VA_ARGS__) : (glDepthMask)(__VA_ARGS__))
#define glFrustum(...)     (nvxDirectGl.Frustum ? nvxDirectGl.Frustum(__VA_ARGS__) : (glFrustum)(__VA_ARGS__))
#define glTranslatef(...)  (nvxDirectGl.Translatef ? nvxDirectGl.Translatef(__VA_ARGS__) : (glTranslatef)(__VA_ARGS__))

static void DrawCube(float angle)
{
    glPushMatrix();
    glRotatef(angle, 1.0f, 1.0f, 0.0f);

    glBegin(GL_QUADS);
    /* Front face (Red) */
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f,  1.0f);
    glVertex3f( 1.0f, -1.0f,  1.0f);
    glVertex3f( 1.0f,  1.0f,  1.0f);
    glVertex3f(-1.0f,  1.0f,  1.0f);

    /* Back face (Green) */
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f(-1.0f,  1.0f, -1.0f);
    glVertex3f( 1.0f,  1.0f, -1.0f);
    glVertex3f( 1.0f, -1.0f, -1.0f);

    /* Top face (Blue) */
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(-1.0f,  1.0f, -1.0f);
    glVertex3f(-1.0f,  1.0f,  1.0f);
    glVertex3f( 1.0f,  1.0f,  1.0f);
    glVertex3f( 1.0f,  1.0f, -1.0f);

    /* Bottom face (Yellow) */
    glColor3f(1.0f, 1.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f( 1.0f, -1.0f, -1.0f);
    glVertex3f( 1.0f, -1.0f,  1.0f);
    glVertex3f(-1.0f, -1.0f,  1.0f);

    /* Right face (Cyan) */
    glColor3f(0.0f, 1.0f, 1.0f);
    glVertex3f( 1.0f, -1.0f, -1.0f);
    glVertex3f( 1.0f,  1.0f, -1.0f);
    glVertex3f( 1.0f,  1.0f,  1.0f);
    glVertex3f( 1.0f, -1.0f,  1.0f);

    /* Left face (Magenta) */
    glColor3f(1.0f, 0.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f(-1.0f, -1.0f,  1.0f);
    glVertex3f(-1.0f,  1.0f,  1.0f);
    glVertex3f(-1.0f,  1.0f, -1.0f);
    glEnd();

    glPopMatrix();
}

#if defined(NVX_MESA_DEPTH_MODE_OVERRIDE) && NVX_MESA_DEPTH_MODE_OVERRIDE == 2
static void TestSelectedZetaFullSurface(HDC hdc);
#endif

static void TestOpenGL(HWND hwnd, HDC hdc)
{
    typedef ULONG_PTR (APIENTRY *PFN_DIRECT_DRV_CREATE_CONTEXT)(HDC);
    typedef BOOL (APIENTRY *PFN_DIRECT_DRV_DELETE_CONTEXT)(ULONG_PTR);
    typedef PVOID (APIENTRY *PFN_DIRECT_DRV_SET_CONTEXT)(HDC, ULONG_PTR, PVOID);
    typedef BOOL (APIENTRY *PFN_DIRECT_DRV_SET_PIXEL_FORMAT)(HDC, LONG);
    typedef LONG (APIENTRY *PFN_DIRECT_DRV_DESCRIBE_PIXEL_FORMAT)(
        HDC, INT, ULONG, PIXELFORMATDESCRIPTOR *);
    typedef BOOL (APIENTRY *PFN_DIRECT_DRV_SWAP_BUFFERS)(HDC);
    typedef PROC (APIENTRY *PFN_DIRECT_DRV_GET_PROC_ADDRESS)(LPCSTR);
    ULONG_PTR directDdiHandle = 0;
    HMODULE directIcdModule = NULL;
    BOOL directDdiCurrent = FALSE;
    PFN_DIRECT_DRV_DELETE_CONTEXT directDeleteContext = NULL;
    PFN_DIRECT_DRV_SET_CONTEXT directSetContext = NULL;
    PFN_DIRECT_DRV_SET_PIXEL_FORMAT directSetPixelFormat = NULL;
    PFN_DIRECT_DRV_DESCRIBE_PIXEL_FORMAT directDescribePixelFormat = NULL;
    PFN_DIRECT_DRV_SWAP_BUFFERS directSwapBuffers = NULL;
    PFN_DIRECT_DRV_GET_PROC_ADDRESS directGetProcAddress = NULL;
    /* The bridge appends per-process results. Remove any prior guest run so
       batch-count deltas cannot be satisfied by stale hardware submissions. */
    BOOL bridgeTraceDeleted = DeleteFileA("C:\\test_accel\\mesa_bridge.log");
    DWORD bridgeTraceDeleteError = bridgeTraceDeleted ? ERROR_SUCCESS : GetLastError();
    LogMsg("[MESA_BRIDGE_TRACE_RESET] delete_error=%lu\n", bridgeTraceDeleteError);
    (void)DeleteFileA("C:\\test_accel\\nvx_nouveau_trace.log");
    ClearMesaIcdTrace();
    InstallOpenGl32TraceHooks();
    unsigned wglTraceStart = openGlTraceCount;
    LogMsg("\n==================================================\n");
    LogMsg(" [TEST 2/3] OpenGL Renderer and Presentation Audit\n");
    LogMsg("==================================================\n");

    DWORD escapeCode = NVX_OPENGL_GETINFO_ESCAPE;
    LONG escapeSupported = ExtEscape(hdc, QUERYESCSUPPORT, sizeof(escapeCode),
                                     (LPCSTR)&escapeCode, 0, NULL);
    LogMsg("-> OPENGL_GETINFO escape support: %ld\n", escapeSupported);
    if (escapeSupported > 0) {
        DWORD subEscape = NVX_OPENGL_GETINFO_DRVNAME;
        NVX_OPENGL_INFO driverInfo;
        memset(&driverInfo, 0, sizeof(driverInfo));
        LONG infoBytes = ExtEscape(hdc, NVX_OPENGL_GETINFO_ESCAPE,
                                   sizeof(subEscape), (LPCSTR)&subEscape,
                                   sizeof(driverInfo), (LPSTR)&driverInfo);
        LogMsg("-> OPENGL_GETINFO name query bytes=%ld version=%lu driverVersion=%lu name=%ls\n",
               infoBytes, (unsigned long)driverInfo.version,
               (unsigned long)driverInfo.driverVersion, driverInfo.driverName);
        LONG legacyInfoBytes = ExtEscape(hdc, NVX_OPENGL_GETINFO_ESCAPE,
                                         sizeof(subEscape), (LPCSTR)&subEscape,
                                         sizeof(DWORD) * 2 + 256 * sizeof(WCHAR),
                                         (LPSTR)&driverInfo);
        LogMsg("-> Legacy 520-byte OPENGL_GETINFO query bytes=%ld name=%ls\n",
               legacyInfoBytes, driverInfo.driverName);
    }
    DumpOpenGlTrace(wglTraceStart);
    wglTraceStart = openGlTraceCount;

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int format = ChoosePixelFormat(hdc, &pfd);
    LogMsg("-> ChoosePixelFormat returned: %d\n", format);
    DumpOpenGlTrace(wglTraceStart);
    wglTraceStart = openGlTraceCount;
    if (!format) {
        LogMsg("-> [FAIL] ChoosePixelFormat failed (err=%lu)\n", GetLastError()); failures++;
        return;
    }

    PIXELFORMATDESCRIPTOR actualPfd;
    memset(&actualPfd, 0, sizeof(actualPfd));
    int described = DescribePixelFormat(hdc, format, sizeof(actualPfd), &actualPfd);
    LogMsg("-> Selected format descriptor before SetPixelFormat: count=%d flags=0x%08lx\n",
           described, (unsigned long)actualPfd.dwFlags);
    if (described <= 0) {
        LogMsg("-> [FAIL] DescribePixelFormat failed before SetPixelFormat (err=%lu)\n",
               GetLastError()); failures++;
        return;
    }
    if (!SetPixelFormat(hdc, format, &actualPfd)) {
        LogMsg("-> [FAIL] SetPixelFormat failed (err=%lu)\n", GetLastError()); failures++;
        return;
    }
    DumpOpenGlTrace(wglTraceStart);
    wglTraceStart = openGlTraceCount;
    memset(&actualPfd, 0, sizeof(actualPfd));
    described = DescribePixelFormat(hdc, format, sizeof(actualPfd), &actualPfd);
    LogMsg("-> Pixel format requested=%d current=%d describe=%d flags=0x%08lx type=%u color=%u depth=%u\n",
           format, GetPixelFormat(hdc), described, (unsigned long)actualPfd.dwFlags,
           (unsigned)actualPfd.iPixelType, (unsigned)actualPfd.cColorBits,
           (unsigned)actualPfd.cDepthBits);

    HGLRC hglrc = wglCreateContext(hdc);
    DWORD wglCreateContextErr = GetLastError();
    DumpOpenGlTrace(wglTraceStart);
    DumpMesaIcdTrace();
    if (!hglrc) {
        LogMsg("-> wglCreateContext failed (err=%lu)\n", wglCreateContextErr);
        DumpMesaRuntimeTraces();
        LogMsg("-> ICD already loaded by WGL: %s\n",
               GetModuleHandleA("nvxogl64.dll") ? "yes" : "no");
        HMODULE icd = LoadLibraryA("nvxogl64.dll");
        directIcdModule = icd;
        LogMsg("-> Direct ICD LoadLibrary: %s (err=%lu)\n",
               icd ? "success" : "failed", GetLastError());
        if (icd) {
            const char *required[] = {
                "DrvCopyContext", "DrvCreateContext", "DrvCreateLayerContext",
                "DrvDeleteContext", "DrvDescribeLayerPlane", "DrvDescribePixelFormat",
                "DrvGetLayerPaletteEntries", "DrvGetProcAddress", "DrvRealizeLayerPalette",
                "DrvReleaseContext", "DrvSetContext", "DrvSetLayerPaletteEntries",
                "DrvSetPixelFormat", "DrvShareLists", "DrvSwapBuffers",
                "DrvSwapLayerBuffers", "DrvValidateVersion"
            };
            size_t i;
            for (i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
                LogMsg("-> ICD export %s: %s\n", required[i],
                       GetProcAddress(icd, required[i]) ? "present" : "missing");
            PFN_DIRECT_DRV_CREATE_CONTEXT createContext =
                (PFN_DIRECT_DRV_CREATE_CONTEXT)GetProcAddress(icd, "DrvCreateContext");
            directDeleteContext =
                (PFN_DIRECT_DRV_DELETE_CONTEXT)GetProcAddress(icd, "DrvDeleteContext");
            directSetContext =
                (PFN_DIRECT_DRV_SET_CONTEXT)GetProcAddress(icd, "DrvSetContext");
            directSetPixelFormat =
                (PFN_DIRECT_DRV_SET_PIXEL_FORMAT)GetProcAddress(icd, "DrvSetPixelFormat");
            directDescribePixelFormat =
                (PFN_DIRECT_DRV_DESCRIBE_PIXEL_FORMAT)GetProcAddress(icd, "DrvDescribePixelFormat");
            directSwapBuffers =
                (PFN_DIRECT_DRV_SWAP_BUFFERS)GetProcAddress(icd, "DrvSwapBuffers");
            directGetProcAddress =
                (PFN_DIRECT_DRV_GET_PROC_ADDRESS)GetProcAddress(icd, "DrvGetProcAddress");
            LogMsg("-> Direct context probe relies on DrvCreateContext to initialize the Mesa screen\n");
            if (createContext) {
                ULONG_PTR directContext;
#if defined(_WIN64)
                if (realDrvCreateContext) {
                    LogMsg("-> Calling the DrvCreateContext pointer resolved by XP WGL\n");
                    unsigned directTraceStart = openGlTraceCount;
                    directContext = TraceDrvCreateContext(hdc);
                    DumpOpenGlTrace(directTraceStart);
                } else {
                    directContext = createContext(hdc);
                }
#else
                directContext = createContext(hdc);
#endif
                LogMsg("-> Direct DrvCreateContext: %s (handle=%p)\n",
                       directContext ? "success" : "failed", (void *)(UINT_PTR)directContext);
                if (directContext && !hglrc && directSetContext) {
                    LONG directFormatCount = directDescribePixelFormat ?
                        directDescribePixelFormat(hdc, 0, 0, NULL) : 0;
                    int directFormat = 0;
                    for (LONG formatIndex = 1; formatIndex <= directFormatCount; ++formatIndex) {
                        PIXELFORMATDESCRIPTOR directPfd;
                        memset(&directPfd, 0, sizeof(directPfd));
                        if (directDescribePixelFormat(hdc, formatIndex,
                                sizeof(directPfd), &directPfd) > 0 &&
                            (directPfd.dwFlags & (PFD_DRAW_TO_WINDOW |
                                PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER)) ==
                                (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                                 PFD_DOUBLEBUFFER) &&
                            directPfd.iPixelType == PFD_TYPE_RGBA &&
                            directPfd.cColorBits >= 24) {
                            directFormat = (int)formatIndex;
                            break;
                        }
                    }
                    BOOL directFormatSet = directFormat > 0 && directSetPixelFormat &&
                        directSetPixelFormat(hdc, directFormat);
                    LogMsg("-> Direct ICD pixel formats=%ld chosen=%d set_result=%u (GDI format=%d)\n",
                           directFormatCount, directFormat, (unsigned)directFormatSet, format);
                    /* DHGLRC is an ICD-private handle, not a public HGLRC. Bind it
                       through the ICD DDI instead of passing it to GDI WGL. */
                    PVOID clientTable = directSetContext(hdc, directContext, NULL);
                    LogMsg("-> Direct DrvSetContext handle=%p result=%p\n",
                           (void *)(UINT_PTR)directContext, clientTable);
                    memset(&nvxDirectGl, 0, sizeof(nvxDirectGl));
#define NVX_LOAD_DIRECT_GL(field, symbol) \
                    nvxDirectGl.field = directGetProcAddress ? \
                        (__typeof__(nvxDirectGl.field))directGetProcAddress(symbol) : NULL
                    NVX_LOAD_DIRECT_GL(Begin, "glBegin");
                    NVX_LOAD_DIRECT_GL(End, "glEnd");
                    NVX_LOAD_DIRECT_GL(Vertex3f, "glVertex3f");
                    NVX_LOAD_DIRECT_GL(Color3f, "glColor3f");
                    NVX_LOAD_DIRECT_GL(Color4f, "glColor4f");
                    NVX_LOAD_DIRECT_GL(PushMatrix, "glPushMatrix");
                    NVX_LOAD_DIRECT_GL(PopMatrix, "glPopMatrix");
                    NVX_LOAD_DIRECT_GL(Rotatef, "glRotatef");
                    NVX_LOAD_DIRECT_GL(GetString, "glGetString");
                    NVX_LOAD_DIRECT_GL(Viewport, "glViewport");
                    NVX_LOAD_DIRECT_GL(MatrixMode, "glMatrixMode");
                    NVX_LOAD_DIRECT_GL(LoadIdentity, "glLoadIdentity");
                    NVX_LOAD_DIRECT_GL(Disable, "glDisable");
                    NVX_LOAD_DIRECT_GL(Enable, "glEnable");
                    NVX_LOAD_DIRECT_GL(ClearColor, "glClearColor");
                    NVX_LOAD_DIRECT_GL(Clear, "glClear");
                    NVX_LOAD_DIRECT_GL(GetError, "glGetError");
                    NVX_LOAD_DIRECT_GL(ReadBuffer, "glReadBuffer");
                    NVX_LOAD_DIRECT_GL(ReadPixels, "glReadPixels");
                    NVX_LOAD_DIRECT_GL(Finish, "glFinish");
                    NVX_LOAD_DIRECT_GL(ClearDepth, "glClearDepth");
                    NVX_LOAD_DIRECT_GL(DepthFunc, "glDepthFunc");
                    NVX_LOAD_DIRECT_GL(DepthMask, "glDepthMask");
                    NVX_LOAD_DIRECT_GL(Frustum, "glFrustum");
                    NVX_LOAD_DIRECT_GL(Translatef, "glTranslatef");
#undef NVX_LOAD_DIRECT_GL
                    unsigned resolvedGl = 0;
                    PROC *glSlots = (PROC *)&nvxDirectGl;
                    size_t glSlotCount = sizeof(nvxDirectGl) / sizeof(PROC);
                    for (size_t glIndex = 0; glIndex < glSlotCount; ++glIndex)
                        if (glSlots[glIndex]) ++resolvedGl;
                    LogMsg("-> Direct DrvGetProcAddress GL functions resolved=%u/%u\n",
                           resolvedGl, (unsigned)glSlotCount);
                    if (directFormat > 0 && clientTable && directSwapBuffers && directGetProcAddress &&
                        resolvedGl == glSlotCount) {
                        directDdiHandle = directContext;
                        directDdiCurrent = TRUE;
                        LogMsg("-> Direct ICD DDI context is current; GL calls use ICD dispatch\n");
                    } else if (directDeleteContext) {
                        if (directSetContext) directSetContext(NULL, 0, NULL);
                        memset(&nvxDirectGl, 0, sizeof(nvxDirectGl));
                        directDeleteContext(directContext);
                    }
                } else if (directContext && directDeleteContext) {
                    directDeleteContext(directContext);
                }
            }
            LogMsg("-> Mesa ICD callback trace after direct ICD probes:\n");
            DumpMesaIcdTrace();
            if (!hglrc && !directDdiCurrent) {
                wglTraceStart = openGlTraceCount;
                hglrc = wglCreateContext(hdc);
                DWORD retryErr = GetLastError();
                DumpOpenGlTrace(wglTraceStart);
                LogMsg("-> WGL retry after explicit ICD initialization: %s (err=%lu)\n",
                       hglrc ? "success" : "failed", retryErr);
            }
            if (!directDdiCurrent) {
                FreeLibrary(icd);
                directIcdModule = NULL;
            }
        }
        if (!hglrc && !directDdiCurrent) {
            LogMsg("-> [FAIL] XP WGL could not create an ICD context\n");
            failures++;
            if (directIcdModule) FreeLibrary(directIcdModule);
            return;
        }
    }

    if (!directDdiCurrent && !wglMakeCurrent(hdc, hglrc)) {
        LogMsg("-> [FAIL] wglMakeCurrent failed (err=%lu)\n", GetLastError()); failures++;
        if (hglrc) wglDeleteContext(hglrc);
        return;
    }

    LogMsg("-> nvxogl64.dll loaded in process: %s\n",
           GetModuleHandleA("nvxogl64.dll") ? "yes" : "no");

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);

    LogMsg("-> OpenGL Vendor:   %s\n", vendor ? vendor : "(null)");
    LogMsg("-> OpenGL Renderer: %s\n", renderer ? renderer : "(null)");
    LogMsg("-> OpenGL Version:  %s\n", version ? version : "(null)");

    if (!renderer || strstr(renderer, "GDI Generic") || strstr(renderer, "llvmpipe") ||
        strstr(renderer, "softpipe")) {
        software3d++;
        LogMsg("-> [SOFTWARE] OpenGL renderer is not GPU hardware acceleration.\n");
    }
    DescribePixelFormat(hdc, format, sizeof(pfd), &pfd);
    LogMsg("-> Selected pixel format flags: 0x%08lX\n", (unsigned long)pfd.dwFlags);

    RECT client;
    GetClientRect(hwnd, &client);
    int w = client.right - client.left;
    int h = client.bottom - client.top;

    glViewport(0, 0, w, h);
    if (mesaGpuBridgeProbe) {
        glViewport(0, 0, 576, 16);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
        glVertex3f(-1.00f, -0.75f, 0.0f);
        glVertex3f( 1.00f, -0.75f, 0.0f);
        glVertex3f( 1.00f,  0.75f, 0.0f);
        glVertex3f(-1.00f,  0.75f, 0.0f);
        glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
        glVertex3f( 0.80f, -0.75f, 0.0f);
        glVertex3f( 0.95f, -0.75f, 0.0f);
        glVertex3f( 0.95f,  0.75f, 0.0f);
        glVertex3f( 0.80f,  0.75f, 0.0f);
        glEnd();
        glFinish();
        GLenum bridgeError = glGetError();
        LogMsg("-> Submitted two flat-color GL_QUADS to Mesa bridge; GL error=0x%X\n",
               bridgeError);
        if (bridgeError != GL_NO_ERROR) failures++;
        if (!(directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc))) {
            LogMsg("[MESA_GPU_PIXEL_RESULT] FAIL (SwapBuffers error=%lu)\n",
                   GetLastError());
            failures++;
        } else {
            BYTE framePixels[576 * 16 * 4];
            unsigned greenPixels = 0, redPixels = 0, backgroundPixels = 0;
            unsigned greenByTile[9] = { 0 };
            unsigned rightRedPixels = 0;
            unsigned p;
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, 576, 16, GL_RGBA, GL_UNSIGNED_BYTE, framePixels);
            GLenum readError = glGetError();
            for (p = 0; p < 576 * 16; p++) {
                BYTE r = framePixels[p * 4 + 0];
                BYTE g = framePixels[p * 4 + 1];
                BYTE b = framePixels[p * 4 + 2];
                if (g > 200 && r < 32 && b < 32) {
                    greenPixels++;
                    greenByTile[(p % 576) / 64]++;
                }
                else if (r > 200 && g < 32 && b < 32) {
                    redPixels++;
                    if ((p % 576) >= 8 * 64) rightRedPixels++;
                }
                else if (r < 16 && g < 16 && b < 16) backgroundPixels++;
            }
            int gpuPixelPass = readError == GL_NO_ERROR && greenPixels >= 8 &&
                redPixels >= 8 && backgroundPixels >= 8 &&
                rightRedPixels >= 8;
            for (p = 0; p < 9; p++)
                if (greenByTile[p] < 8) gpuPixelPass = 0;
            DumpMesaBridgeTrace();
            if (!mesaGpuFramebufferVerified || mesaGpuBridgeBatchCount == 0)
                gpuPixelPass = 0;
            LogMsg("[MESA_GPU_PIXEL_RESULT] %s green_pixels=%u red_pixels=%u background_pixels=%u error=0x%X\n",
                   gpuPixelPass ? "PASS" : "FAIL", greenPixels, redPixels,
                   backgroundPixels, readError);
            if (!gpuPixelPass) failures++;
            LogMsg("[MESA_GPU_TILE_RESULT] %s tiles=%u green=%u,%u,%u,%u,%u,%u,%u,%u,%u right_red=%u\n",
                   gpuPixelPass ? "PASS" : "FAIL", 9,
                   greenByTile[0], greenByTile[1], greenByTile[2],
                   greenByTile[3], greenByTile[4], greenByTile[5],
                   greenByTile[6], greenByTile[7], greenByTile[8],
                   rightRedPixels);
        }
        {
            typedef unsigned int (APIENTRY *PFN_GL_CREATE_SHADER)(unsigned int);
            typedef void (APIENTRY *PFN_GL_SHADER_SOURCE)(unsigned int, int,
                const char *const *, const int *);
            typedef void (APIENTRY *PFN_GL_COMPILE_SHADER)(unsigned int);
            typedef void (APIENTRY *PFN_GL_GET_SHADER_IV)(unsigned int,
                unsigned int, int *);
            typedef void (APIENTRY *PFN_GL_GET_SHADER_LOG)(unsigned int,
                int, int *, char *);
            typedef unsigned int (APIENTRY *PFN_GL_CREATE_PROGRAM)(void);
            typedef void (APIENTRY *PFN_GL_ATTACH_SHADER)(unsigned int,
                unsigned int);
            typedef void (APIENTRY *PFN_GL_LINK_PROGRAM)(unsigned int);
            typedef void (APIENTRY *PFN_GL_GET_PROGRAM_IV)(unsigned int,
                unsigned int, int *);
            typedef void (APIENTRY *PFN_GL_GET_PROGRAM_LOG)(unsigned int,
                int, int *, char *);
            typedef void (APIENTRY *PFN_GL_USE_PROGRAM)(unsigned int);
            typedef void (APIENTRY *PFN_GL_DELETE_SHADER)(unsigned int);
            typedef void (APIENTRY *PFN_GL_DELETE_PROGRAM)(unsigned int);
            PFN_GL_CREATE_SHADER createShader = (PFN_GL_CREATE_SHADER)
                (directGetProcAddress ? directGetProcAddress("glCreateShader") :
                 wglGetProcAddress("glCreateShader"));
            PFN_GL_SHADER_SOURCE shaderSource = (PFN_GL_SHADER_SOURCE)
                (directGetProcAddress ? directGetProcAddress("glShaderSource") :
                 wglGetProcAddress("glShaderSource"));
            PFN_GL_COMPILE_SHADER compileShader = (PFN_GL_COMPILE_SHADER)
                (directGetProcAddress ? directGetProcAddress("glCompileShader") :
                 wglGetProcAddress("glCompileShader"));
            PFN_GL_GET_SHADER_IV getShaderiv = (PFN_GL_GET_SHADER_IV)
                (directGetProcAddress ? directGetProcAddress("glGetShaderiv") :
                 wglGetProcAddress("glGetShaderiv"));
            PFN_GL_GET_SHADER_LOG getShaderLog = (PFN_GL_GET_SHADER_LOG)
                (directGetProcAddress ? directGetProcAddress("glGetShaderInfoLog") :
                 wglGetProcAddress("glGetShaderInfoLog"));
            PFN_GL_CREATE_PROGRAM createProgram = (PFN_GL_CREATE_PROGRAM)
                (directGetProcAddress ? directGetProcAddress("glCreateProgram") :
                 wglGetProcAddress("glCreateProgram"));
            PFN_GL_ATTACH_SHADER attachShader = (PFN_GL_ATTACH_SHADER)
                (directGetProcAddress ? directGetProcAddress("glAttachShader") :
                 wglGetProcAddress("glAttachShader"));
            PFN_GL_LINK_PROGRAM linkProgram = (PFN_GL_LINK_PROGRAM)
                (directGetProcAddress ? directGetProcAddress("glLinkProgram") :
                 wglGetProcAddress("glLinkProgram"));
            PFN_GL_GET_PROGRAM_IV getProgramiv = (PFN_GL_GET_PROGRAM_IV)
                (directGetProcAddress ? directGetProcAddress("glGetProgramiv") :
                 wglGetProcAddress("glGetProgramiv"));
            PFN_GL_GET_PROGRAM_LOG getProgramLog = (PFN_GL_GET_PROGRAM_LOG)
                (directGetProcAddress ? directGetProcAddress("glGetProgramInfoLog") :
                 wglGetProcAddress("glGetProgramInfoLog"));
            PFN_GL_USE_PROGRAM useProgram = (PFN_GL_USE_PROGRAM)
                (directGetProcAddress ? directGetProcAddress("glUseProgram") :
                 wglGetProcAddress("glUseProgram"));
            PFN_GL_DELETE_SHADER deleteShader = (PFN_GL_DELETE_SHADER)
                (directGetProcAddress ? directGetProcAddress("glDeleteShader") :
                 wglGetProcAddress("glDeleteShader"));
            PFN_GL_DELETE_PROGRAM deleteProgram = (PFN_GL_DELETE_PROGRAM)
                (directGetProcAddress ? directGetProcAddress("glDeleteProgram") :
                 wglGetProcAddress("glDeleteProgram"));
            static const char vertexSource[] =
                "#version 110\n"
                "void main() { gl_Position = gl_Vertex; }\n";
            static const char fragmentSource[] =
                "#version 110\n"
                "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
            unsigned int vertexShader = 0, fragmentShader = 0, program = 0;
            unsigned int fixedVertexProgram = 0;
            int vertexCompiled = 0, fragmentCompiled = 0, linked = 0;
            int fixedVertexLinked = 0;
            BYTE *pixels = NULL;
            BYTE baselinePixel[4] = {0, 0, 0, 0};
            BYTE postDrawPixel[4] = {0, 0, 0, 0};
            unsigned greenPixels = 0, pixel;
            int greenMinX = w, greenMinY = h, greenMaxX = -1, greenMaxY = -1;
            GLenum shaderError = GL_NO_ERROR;
            GLenum fixedVertexError = GL_NO_ERROR;
            BYTE fixedVertexBaseline[4] = {0, 0, 0, 0};
            BYTE fixedVertexCenter[4] = {0, 0, 0, 0};
            unsigned fixedVertexGreen = 0;
            unsigned rawTargetGreen = 0, fixedRawTargetGreen = 0;
            uint32_t rawTargetBlue = 0, fixedRawTargetBlue = 0;
            uint64_t rawTargetBufferId = 0, fixedRawTargetBufferId = 0;
            uint64_t rawTargetVa = 0, fixedRawTargetVa = 0;
            uint32_t rawTargetBytes = 0, fixedRawTargetBytes = 0;
            int rawTargetAvailable = 0, fixedRawTargetAvailable = 0;
            int baselinePass;
            int shaderApiReady = createShader && shaderSource && compileShader &&
                getShaderiv && getShaderLog && createProgram && attachShader &&
                linkProgram && getProgramiv && getProgramLog && useProgram &&
                deleteShader && deleteProgram;
            LogMsg("[MESA_GLSL_TRIANGLE] shader_api=%s\n",
                   shaderApiReady ? "ready" : "missing");
            if (shaderApiReady) {
                const char *source = vertexSource;
                char shaderLog[512];
                vertexShader = createShader(0x8B31U);
                shaderSource(vertexShader, 1, &source, NULL);
                compileShader(vertexShader);
                getShaderiv(vertexShader, 0x8B81U, &vertexCompiled);
                if (!vertexCompiled) {
                    int logBytes = 0;
                    getShaderLog(vertexShader, sizeof(shaderLog) - 1,
                                 &logBytes, shaderLog);
                    shaderLog[sizeof(shaderLog) - 1] = '\0';
                    LogMsg("[MESA_GLSL_TRIANGLE] vertex compile failed: %s\n",
                           shaderLog);
                }
                source = fragmentSource;
                fragmentShader = createShader(0x8B30U);
                shaderSource(fragmentShader, 1, &source, NULL);
                compileShader(fragmentShader);
                getShaderiv(fragmentShader, 0x8B81U, &fragmentCompiled);
                if (!fragmentCompiled) {
                    int logBytes = 0;
                    getShaderLog(fragmentShader, sizeof(shaderLog) - 1,
                                 &logBytes, shaderLog);
                    shaderLog[sizeof(shaderLog) - 1] = '\0';
                    LogMsg("[MESA_GLSL_TRIANGLE] fragment compile failed: %s\n",
                           shaderLog);
                }
                if (vertexCompiled && fragmentCompiled) {
                    program = createProgram();
                    attachShader(program, vertexShader);
                    attachShader(program, fragmentShader);
                    linkProgram(program);
                    getProgramiv(program, 0x8B82U, &linked);
                    if (!linked) {
                        int logBytes = 0;
                        getProgramLog(program, sizeof(shaderLog) - 1,
                                      &logBytes, shaderLog);
                        shaderLog[sizeof(shaderLog) - 1] = '\0';
                        LogMsg("[MESA_GLSL_TRIANGLE] program link failed: %s\n",
                               shaderLog);
                    }
                }
                if (linked) {
                    size_t pixelBytes = (size_t)w * (size_t)h * 4U;
                    if (w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
                        pixelBytes / 4U / (size_t)w == (size_t)h)
                        pixels = (BYTE *)malloc(pixelBytes);
                    glViewport(0, 0, 64, 64);
                    glMatrixMode(GL_PROJECTION);
                    glLoadIdentity();
                    glMatrixMode(GL_MODELVIEW);
                    glLoadIdentity();
                    glDisable(GL_DEPTH_TEST);
                    glDisable(GL_CULL_FACE);
                    glDisable(GL_BLEND);
                    glDisable(GL_SCISSOR_TEST);
                    glReadBuffer(GL_BACK);
                    glClearColor(0.0f, 0.0f, 0.75f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    useProgram(program);
                    glFinish();
                    shaderError = glGetError();
                    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                 baselinePixel);
                    if (shaderError == GL_NO_ERROR)
                        shaderError = glGetError();
                    glBegin(GL_TRIANGLES);
                    glVertex3f(-0.8f, -0.8f, 0.0f);
                    glVertex3f( 0.8f, -0.8f, 0.0f);
                    glVertex3f( 0.0f,  0.8f, 0.0f);
                    glEnd();
                    glFinish();
                    if (shaderError == GL_NO_ERROR)
                        shaderError = glGetError();
                    rawTargetGreen = ReadRawMesaTargetGreenPixels(
                        2U, &rawTargetBlue, &rawTargetVa, &rawTargetBufferId,
                        &rawTargetBytes,
                        &rawTargetAvailable);
                    LogMsg("[MESA_GLSL_RAW_TARGET] va=%I64u buffer=%I64u bytes=%u green_pixels=%u blue_clear_pixels=%u source=%s\n",
                           rawTargetVa, rawTargetBufferId, rawTargetBytes,
                           rawTargetGreen, rawTargetBlue,
                           rawTargetAvailable ? "miniport-ce-download" :
                                                "unavailable");
                    glReadBuffer(GL_BACK);
                    if (pixels)
                        glReadPixels(0, 0, w, h, GL_RGBA,
                                     GL_UNSIGNED_BYTE, pixels);
                    else if (shaderError == GL_NO_ERROR)
                        shaderError = 0x0505U; /* GL_OUT_OF_MEMORY */
                    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                 postDrawPixel);
                    if (shaderError == GL_NO_ERROR)
                        shaderError = glGetError();
                    if (shaderError == GL_NO_ERROR)
                        shaderError = glGetError();
                    for (pixel = 0; pixels && pixel < (unsigned)w * (unsigned)h;
                         pixel++)
                        if (pixels[pixel * 4U + 1U] > 200U &&
                            pixels[pixel * 4U] < 32U &&
                            pixels[pixel * 4U + 2U] < 32U) {
                            unsigned x = pixel % (unsigned)w;
                            unsigned y = pixel / (unsigned)w;
                            greenPixels++;
                            if ((int)x < greenMinX) greenMinX = (int)x;
                            if ((int)y < greenMinY) greenMinY = (int)y;
                            if ((int)x > greenMaxX) greenMaxX = (int)x;
                            if ((int)y > greenMaxY) greenMaxY = (int)y;
                        }
                    useProgram(0);

                    /* Reuse the constant-green fragment shader with Mesa's
                     * fixed-function vertex stage to isolate the GLSL VS. */
                    fixedVertexProgram = createProgram();
                    attachShader(fixedVertexProgram, fragmentShader);
                    linkProgram(fixedVertexProgram);
                    getProgramiv(fixedVertexProgram, 0x8B82U,
                                 &fixedVertexLinked);
                    if (fixedVertexLinked && pixels) {
                        glViewport(0, 0, 64, 64);
                        glMatrixMode(GL_PROJECTION);
                        glLoadIdentity();
                        glMatrixMode(GL_MODELVIEW);
                        glLoadIdentity();
                        glDisable(GL_DEPTH_TEST);
                        glDisable(GL_CULL_FACE);
                        glDisable(GL_BLEND);
                        glDisable(GL_SCISSOR_TEST);
                        glReadBuffer(GL_BACK);
                        glClearColor(0.0f, 0.0f, 0.75f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        useProgram(fixedVertexProgram);
                        glFinish();
                        fixedVertexError = glGetError();
                        glReadPixels(32, 32, 1, 1, GL_RGBA,
                                     GL_UNSIGNED_BYTE, fixedVertexBaseline);
                        if (fixedVertexError == GL_NO_ERROR)
                            fixedVertexError = glGetError();
                        glBegin(GL_TRIANGLES);
                        glVertex3f(-0.8f, -0.8f, 0.0f);
                        glVertex3f( 0.8f, -0.8f, 0.0f);
                        glVertex3f( 0.0f,  0.8f, 0.0f);
                        glEnd();
                        glFinish();
                        if (fixedVertexError == GL_NO_ERROR)
                            fixedVertexError = glGetError();
                        fixedRawTargetGreen = ReadRawMesaTargetGreenPixels(
                            3U, &fixedRawTargetBlue, &fixedRawTargetVa,
                            &fixedRawTargetBufferId,
                            &fixedRawTargetBytes,
                            &fixedRawTargetAvailable);
                        LogMsg("[MESA_GLSL_FFVS_RAW_TARGET] va=%I64u buffer=%I64u bytes=%u green_pixels=%u blue_clear_pixels=%u source=%s\n",
                               fixedRawTargetVa, fixedRawTargetBufferId,
                               fixedRawTargetBytes,
                               fixedRawTargetGreen,
                               fixedRawTargetBlue,
                               fixedRawTargetAvailable ?
                                   "miniport-ce-download" : "unavailable");
                        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                                     pixels);
                        glReadPixels(32, 32, 1, 1, GL_RGBA,
                                     GL_UNSIGNED_BYTE, fixedVertexCenter);
                        if (fixedVertexError == GL_NO_ERROR)
                            fixedVertexError = glGetError();
                        for (pixel = 0; pixel < (unsigned)w * (unsigned)h;
                             pixel++)
                            if (pixels[pixel * 4U + 1U] > 200U &&
                                pixels[pixel * 4U] < 32U &&
                                pixels[pixel * 4U + 2U] < 32U)
                                fixedVertexGreen++;
                        LogMsg("[MESA_GLSL_FFVS_RESULT] linked=%d green_pixels=%u readback=%dx%d baseline=%u,%u,%u,%u post_center=%u,%u,%u,%u error=0x%X\n",
                               fixedVertexLinked, fixedVertexGreen, w, h,
                               fixedVertexBaseline[0], fixedVertexBaseline[1],
                               fixedVertexBaseline[2], fixedVertexBaseline[3],
                               fixedVertexCenter[0], fixedVertexCenter[1],
                               fixedVertexCenter[2], fixedVertexCenter[3],
                               fixedVertexError);
                        if (rawTargetGreen >= 200U ||
                            fixedRawTargetGreen >= 200U)
                            LogMsg("[MESA_GLSL_RAW_TARGET] hardware readback found triangle pixels missed by glReadPixels\n");
                        if (!fixedVertexLinked ||
                            fixedVertexBaseline[2] < 150U ||
                            fixedVertexGreen < 200U ||
                            fixedVertexError != GL_NO_ERROR)
                            failures++;
                    } else {
                        LogMsg("[MESA_GLSL_FFVS_RESULT] linked=%d readback_buffer=%s error=0x%X\n",
                               fixedVertexLinked, pixels ? "ready" : "missing",
                               fixedVertexError);
                        failures++;
                    }
                    useProgram(0);
                }
                if (vertexShader) deleteShader(vertexShader);
                if (fragmentShader) deleteShader(fragmentShader);
                if (program) deleteProgram(program);
                if (fixedVertexProgram) deleteProgram(fixedVertexProgram);
            }
            if (pixels) free(pixels);
            int glslPass = shaderApiReady && vertexCompiled &&
                fragmentCompiled && linked && shaderError == GL_NO_ERROR &&
                greenPixels >= 200U;
            baselinePass = baselinePixel[0] < 32U &&
                           baselinePixel[1] < 32U &&
                           baselinePixel[2] > 150U;
            glslPass = glslPass && baselinePass;
            LogMsg("[MESA_GLSL_TRIANGLE_RESULT] %s compiled=%d/%d linked=%d baseline_pass=%d green_pixels=%u bbox=%d,%d-%d,%d readback=%dx%d baseline=%u,%u,%u,%u post_center=%u,%u,%u,%u error=0x%X\n",
                   glslPass ? "PASS" : "FAIL", vertexCompiled,
                   fragmentCompiled, linked, baselinePass, greenPixels,
                   greenMinX, greenMinY, greenMaxX, greenMaxY, w, h,
                   baselinePixel[0], baselinePixel[1], baselinePixel[2],
                   baselinePixel[3], postDrawPixel[0], postDrawPixel[1],
                   postDrawPixel[2], postDrawPixel[3],
                   shaderError);
            if (!glslPass) failures++;
            glViewport(0, 0, w, h);
        }
        DumpMesaBridgeTrace();
        {
            BYTE fullTargetCenter[4]={0,0,0,0},fullTargetCorner[4]={0,0,0,0};
            BYTE fullTargetBackCenter[4]={0,0,0,0};
            BYTE mesaClearBackCenter[4]={0,0,0,0};
            GLenum fullTargetError;
            unsigned batchesBefore=mesaGpuBridgeBatchCount;
            glViewport(0,0,w,h);
            glDisable(GL_DEPTH_TEST);
            glReadBuffer(GL_BACK);
            glClearColor(0.125f,0.25f,0.75f,1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glFinish();
            fullTargetError=glGetError();
            if(fullTargetError==GL_NO_ERROR) {
                glReadPixels(w/2,h/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             mesaClearBackCenter);
                fullTargetError=glGetError();
            }
            LogMsg("[MESA_GPU_CLEAR_BACK_READBACK] pixel=%u,%u,%u error=0x%X\n",
                   mesaClearBackCenter[0],mesaClearBackCenter[1],
                   mesaClearBackCenter[2],fullTargetError);
            if(fullTargetError==GL_NO_ERROR&&
               mesaClearBackCenter[0]>=24&&mesaClearBackCenter[0]<=40&&
               mesaClearBackCenter[1]>=56&&mesaClearBackCenter[1]<=72&&
               mesaClearBackCenter[2]>=184&&mesaClearBackCenter[2]<=200) {
                LogMsg("[MESA_GPU_CLEAR_BACK_RESULT] PASS\n");
            } else {
                LogMsg("[MESA_GPU_CLEAR_BACK_RESULT] FAIL\n");
                failures++;
            }
            glClearColor(0.0f,0.0f,0.0f,1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS);
            glColor4f(0.0f,1.0f,0.0f,1.0f);
            glVertex3f(-0.9f,-0.9f,0.0f);
            glVertex3f( 0.9f,-0.9f,0.0f);
            glVertex3f( 0.9f, 0.9f,0.0f);
            glVertex3f(-0.9f, 0.9f,0.0f);
            glEnd();
            glFinish();
            fullTargetError=glGetError();
            if(fullTargetError==GL_NO_ERROR) {
                glReadBuffer(GL_BACK);
                glReadPixels(w/2,h/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             fullTargetBackCenter);
                fullTargetError=glGetError();
            }
            if(fullTargetError==GL_NO_ERROR&&(directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc))) {
                glReadBuffer(GL_FRONT);
                glReadPixels(w/2,h/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             fullTargetCenter);
                glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             fullTargetCorner);
                fullTargetError=glGetError();
            } else if(fullTargetError==GL_NO_ERROR) {
                fullTargetError=(GLenum)GetLastError();
            }
            LogMsg("[MESA_GPU_FULL_TARGET_BACK] center=%u,%u,%u error=0x%X\n",
                   fullTargetBackCenter[0],fullTargetBackCenter[1],
                   fullTargetBackCenter[2],fullTargetError);
            DumpMesaBridgeTrace();
            if(fullTargetError==GL_NO_ERROR&&
               mesaGpuBridgeBatchCount>batchesBefore&&
               fullTargetCenter[1]>200&&fullTargetCenter[0]<32&&
               fullTargetCorner[0]<16&&fullTargetCorner[1]<16&&
               fullTargetCorner[2]<16) {
                LogMsg("[MESA_GPU_FULL_TARGET_RESULT] PASS size=%dx%d center=%u,%u,%u corner=%u,%u,%u\n",
                    w,h,fullTargetCenter[0],fullTargetCenter[1],
                    fullTargetCenter[2],fullTargetCorner[0],
                    fullTargetCorner[1],fullTargetCorner[2]);
            } else {
                LogMsg("[MESA_GPU_FULL_TARGET_RESULT] FAIL size=%dx%d center=%u,%u,%u corner=%u,%u,%u error=0x%X batches=%u->%u\n",
                    w,h,fullTargetCenter[0],fullTargetCenter[1],
                    fullTargetCenter[2],fullTargetCorner[0],
                    fullTargetCorner[1],fullTargetCorner[2],fullTargetError,
                    batchesBefore,mesaGpuBridgeBatchCount);
                failures++;
            }
        }
        {
            BYTE depthCenter[4]={0,0,0,0},depthCorner[4]={0,0,0,0};
            BYTE depthBackCorner[4]={0,0,0,0},depthBackMirror[4]={0,0,0,0};
            BYTE depthBackCenter[4]={0,0,0,0};
            GLenum depthError;
            unsigned batchesBefore=mesaGpuBridgeBatchCount;
            glViewport(0,0,w,h);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glClearColor(0.0f,0.0f,0.0f,1.0f);
            glClearDepth(1.0);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            glBegin(GL_TRIANGLES);
            glColor4f(0.0f,1.0f,0.0f,1.0f);
            glVertex3f(-0.8f,-0.8f,0.5f);
            glVertex3f( 0.8f,-0.8f,0.5f);
            glVertex3f( 0.8f, 0.8f,0.5f);
            glVertex3f(-0.8f,-0.8f,0.5f);
            glVertex3f( 0.8f, 0.8f,0.5f);
            glVertex3f(-0.8f, 0.8f,0.5f);
            glColor4f(1.0f,0.0f,0.0f,1.0f);
            glVertex3f(-0.3f,-0.3f,-0.5f);
            glVertex3f( 0.3f,-0.3f,-0.5f);
            glVertex3f( 0.3f, 0.3f,-0.5f);
            glVertex3f(-0.3f,-0.3f,-0.5f);
            glVertex3f( 0.3f, 0.3f,-0.5f);
            glVertex3f(-0.3f, 0.3f,-0.5f);
            glEnd();
            glFinish();
            depthError=glGetError();
            if(depthError==GL_NO_ERROR) {
                glReadBuffer(GL_BACK);
                glReadPixels(w/6,h/6,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             depthBackCorner);
                glReadPixels(w/6,5*h/6,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             depthBackMirror);
                glReadPixels(w/2,h/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             depthBackCenter);
                depthError=glGetError();
            }
            if(depthError==GL_NO_ERROR&&(directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc))) {
                glReadBuffer(GL_FRONT);
                glReadPixels(w/2,h/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,
                             depthCenter);
                glReadPixels(w/6,h/6,1,1,GL_RGBA,GL_UNSIGNED_BYTE,depthCorner);
                depthError=glGetError();
            } else if(depthError==GL_NO_ERROR) {
                depthError=(GLenum)GetLastError();
            }
            LogMsg("[MESA_GPU_FULL_TARGET_DEPTH_BACK] far=%u,%u,%u mirror=%u,%u,%u center=%u,%u,%u error=0x%X\n",
                   depthBackCorner[0],depthBackCorner[1],depthBackCorner[2],
                   depthBackMirror[0],depthBackMirror[1],depthBackMirror[2],
                   depthBackCenter[0],depthBackCenter[1],depthBackCenter[2],
                   depthError);
            DumpMesaBridgeTrace();
            if(depthError==GL_NO_ERROR&&
               mesaGpuBridgeBatchCount>batchesBefore&&
               depthCenter[0]>200&&depthCenter[1]<32&&
               depthCorner[1]>200&&depthCorner[0]<32) {
                LogMsg("[MESA_GPU_FULL_TARGET_DEPTH_RESULT] PASS size=%dx%d center=%u,%u,%u corner=%u,%u,%u\n",
                    w,h,depthCenter[0],depthCenter[1],depthCenter[2],
                    depthCorner[0],depthCorner[1],depthCorner[2]);
            } else {
                LogMsg("[MESA_GPU_FULL_TARGET_DEPTH_RESULT] FAIL size=%dx%d center=%u,%u,%u corner=%u,%u,%u error=0x%X batches=%u->%u\n",
                    w,h,depthCenter[0],depthCenter[1],depthCenter[2],
                    depthCorner[0],depthCorner[1],depthCorner[2],depthError,
                    batchesBefore,mesaGpuBridgeBatchCount);
                failures++;
            }
            glDisable(GL_DEPTH_TEST);
        }
        glViewport(0, 0, w, h);
    }
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* Simple frustum setup */
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    glFrustum(-aspect, aspect, -1.0f, 1.0f, 1.0f, 100.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);

    if (mesaGpuBridgeProbe) {
        BYTE depthPixel[4] = { 0, 0, 0, 0 };
        GLenum depthError;
        unsigned depthBatchesBefore = mesaGpuBridgeBatchCount;
        glViewport(0, 0, 64, 16);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
        glVertex3f(-0.9f, -0.9f, 0.5f);
        glVertex3f( 0.9f, -0.9f, 0.5f);
        glVertex3f( 0.0f,  0.9f, 0.5f);
        glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
        glVertex3f(-0.9f, -0.9f, -0.5f);
        glVertex3f( 0.9f, -0.9f, -0.5f);
        glVertex3f( 0.0f,  0.9f, -0.5f);
        glColor4f(0.0f, 0.0f, 1.0f, 1.0f);
        glVertex3f(-0.9f, -0.9f, 0.75f);
        glVertex3f( 0.9f, -0.9f, 0.75f);
        glVertex3f( 0.0f,  0.9f, 0.75f);
        glEnd();
        glFinish();
        depthError = glGetError();
        if (depthError == GL_NO_ERROR && (directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc))) {
            glReadBuffer(GL_FRONT);
            glReadPixels(32, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, depthPixel);
            depthError = glGetError();
        } else if (depthError == GL_NO_ERROR) {
            depthError = (GLenum)GetLastError();
        }
        DumpMesaBridgeTrace();
        if (depthError == GL_NO_ERROR && mesaGpuBridgeBatchCount > depthBatchesBefore &&
            depthPixel[1] > 200 &&
            depthPixel[0] < 32 && depthPixel[2] < 32) {
            LogMsg("[MESA_GPU_DEPTH_RESULT] PASS gpu_batches=%u front_green=%u,%u,%u error=0x%X\n",
                   mesaGpuBridgeBatchCount, depthPixel[0], depthPixel[1],
                   depthPixel[2], depthError);
        } else {
            LogMsg("[MESA_GPU_DEPTH_RESULT] FAIL gpu_batches=%u front_pixel=%u,%u,%u error=0x%X\n",
                   mesaGpuBridgeBatchCount, depthPixel[0], depthPixel[1],
                   depthPixel[2], depthError);
            failures++;
        }
        glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-aspect, aspect, -1.0f, 1.0f, 1.0f, 100.0f);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -5.0f);

        /* Keep the cube's GPU evidence independent of the depth probe above. */
        DeleteFileA("C:\\test_accel\\mesa_bridge.log");
        BYTE *cubePixels = NULL, *backPixels = NULL;
        SIZE_T cubeBytes = (SIZE_T)w * (SIZE_T)h * 4;
        unsigned coloredPixels = 0, backColored = 0, noDepthColored = 0,
                 facePixels[6] = {0,0,0,0,0,0},
                 minX = (unsigned)w, minY = (unsigned)h, maxX = 0, maxY = 0,
                 p, visibleFaces = 0;
        GLenum error;
        unsigned batchesBefore = mesaGpuBridgeBatchCount;
        glViewport(0, 0, w, h);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        DrawCube(17.0f);
        glFinish();
        cubePixels = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cubeBytes);
        backPixels = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cubeBytes);
        if (backPixels) {
            GLenum backError;
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, backPixels);
            backError = glGetError();
            if (backError == GL_NO_ERROR) {
                for (p = 0; p < (unsigned)(w * h); p++) {
                    if (backPixels[p * 4] > 200 || backPixels[p * 4 + 1] > 200 ||
                        backPixels[p * 4 + 2] > 200)
                        backColored++;
                }
            }
            LogMsg("[MESA_GPU_CUBE_BACK_READBACK] colored_pixels=%u error=0x%X\n",
                   backColored, backError);
        } else {
            LogMsg("[MESA_GPU_CUBE_BACK_READBACK] allocation failed\n");
        }
        if (!cubePixels) {
            LogMsg("[MESA_GPU_CUBE_RESULT] FAIL (full-frame readback allocation failed)\n");
            failures++;
        } else if (!(directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc))) {
            LogMsg("[MESA_GPU_CUBE_RESULT] FAIL (SwapBuffers error=%lu)\n",
                   GetLastError());
            failures++;
        } else {
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, cubePixels);
            error = glGetError();
            /* Changing an environment variable cannot replace the renderer
             * for a context that already exists.  Treat readback pixels as
             * the evidence; a second draw in this context is not a softpipe
             * reference. */
            for (p = 0; p < (unsigned)(w * h); p++) {
                BYTE r = cubePixels[p * 4 + 0];
                BYTE g = cubePixels[p * 4 + 1];
                BYTE b = cubePixels[p * 4 + 2];
                unsigned x = p % (unsigned)w, y = p / (unsigned)w;
                if (r > 200 || g > 200 || b > 200) {
                    coloredPixels++;
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                    if (r > 200 && g < 32 && b < 32) facePixels[0]++;
                    else if (g > 200 && r < 32 && b < 32) facePixels[1]++;
                    else if (b > 200 && r < 32 && g < 32) facePixels[2]++;
                    else if (r > 200 && g > 200 && b < 32) facePixels[3]++;
                    else if (g > 200 && b > 200 && r < 32) facePixels[4]++;
                    else if (r > 200 && b > 200 && g < 32) facePixels[5]++;
                }
            }
            for (p = 0; p < 6; p++)
                if (facePixels[p] >= 32) visibleFaces++;
            DumpMesaBridgeTrace();
            if (error == GL_NO_ERROR &&
                coloredPixels >= 2000 &&
                visibleFaces >= 2 && maxX - minX >= 40 && maxY - minY >= 40 &&
                mesaGpuBridgeBatchCount > batchesBefore) {
                LogMsg("[MESA_GPU_CUBE_RESULT] PASS colored_pixels=%u bbox=%u,%u-%u,%u faces=%u rgb_counts=%u,%u,%u,%u,%u,%u comparison=none new_gpu_batches=%u\n",
                       coloredPixels, minX, minY, maxX, maxY, visibleFaces,
                       facePixels[0], facePixels[1], facePixels[2], facePixels[3],
                       facePixels[4], facePixels[5],
                       mesaGpuBridgeBatchCount - batchesBefore);
            } else {
                LogMsg("[MESA_GPU_CUBE_RESULT] FAIL colored_pixels=%u bbox=%u,%u-%u,%u faces=%u rgb_counts=%u,%u,%u,%u,%u,%u gpu_batches=%u error=0x%X\n",
                       coloredPixels, minX, minY, maxX, maxY, visibleFaces,
                       facePixels[0], facePixels[1], facePixels[2], facePixels[3],
                       facePixels[4], facePixels[5],
                       mesaGpuBridgeBatchCount - batchesBefore, error);
                failures++;
            }
        }
        if (cubePixels) {
            GLenum noDepthError = GL_NO_ERROR;
            unsigned noDepthBatches = mesaGpuBridgeBatchCount;
            glDisable(GL_DEPTH_TEST);
            glClear(GL_COLOR_BUFFER_BIT);
            DrawCube(17.0f);
            glFinish();
            noDepthError = glGetError();
            if (noDepthError == GL_NO_ERROR && !(directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc)))
                noDepthError = (GLenum)GetLastError();
            if (noDepthError == GL_NO_ERROR) {
                glReadBuffer(GL_FRONT);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                             cubePixels);
                noDepthError = glGetError();
                if (noDepthError == GL_NO_ERROR) {
                    for (p = 0; p < (unsigned)(w * h); p++)
                        if (cubePixels[p * 4] > 200 ||
                            cubePixels[p * 4 + 1] > 200 ||
                            cubePixels[p * 4 + 2] > 200)
                            noDepthColored++;
                }
            }
            DumpMesaBridgeTrace();
            {
                unsigned noDepthBatchesAfterGpu = mesaGpuBridgeBatchCount;
                int noDepthHardwareRendered =
                    noDepthBatchesAfterGpu > noDepthBatches;
                LogMsg("[MESA_GPU_CUBE_NODEPTH] %s colored_pixels=%u error=0x%X new_gpu_batches=%u hardware_rendered=%u\n",
                   noDepthError == GL_NO_ERROR &&
                   noDepthHardwareRendered &&
                   noDepthColored >= 2000 ? "PASS" : "FAIL",
                   noDepthColored, noDepthError,
                   noDepthBatchesAfterGpu - noDepthBatches,
                   noDepthHardwareRendered);
                if (noDepthError != GL_NO_ERROR ||
                    noDepthColored < 2000 || !noDepthHardwareRendered)
                    failures++;
            }
            glEnable(GL_DEPTH_TEST);
        }
        if (cubePixels) HeapFree(GetProcessHeap(), 0, cubePixels);
        if (backPixels) HeapFree(GetProcessHeap(), 0, backPixels);
        glViewport(0, 0, w, h);
    }

#if defined(NVX_MESA_DEPTH_MODE_OVERRIDE) && NVX_MESA_DEPTH_MODE_OVERRIDE == 2
    /* Run after the depth-tested and no-depth cube paths so the isolated
       selected-Zeta probe also checks the state transition that precedes the
       animated WGL workload. */
    TestSelectedZetaFullSurface(hdc);
#endif
    glEnable(GL_DEPTH_TEST);

    const int GL_FRAMES = 100;
    LogMsg("-> Rendering %d 3D animated cube frames with SwapBuffers...\n", GL_FRAMES);

    int framesRendered = 0;
    double tStart = GetTimeSeconds();
    double intervalStart = tStart;
    unsigned intervalBatches = mesaGpuBridgeBatchCount;
    for (int frame = 0; frame < GL_FRAMES; frame++) {
        if (frame == 0) LogMsg("[OPENGL_FRAME0] clear begin\n");
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (frame == 0) LogMsg("[OPENGL_FRAME0] clear complete; cube draw begin\n");

        DrawCube((float)frame * 3.6f);
        if (frame == 0) LogMsg("[OPENGL_FRAME0] cube draw complete; swap begin\n");

        BOOL swapResult = directDdiCurrent ? directSwapBuffers(hdc) : SwapBuffers(hdc);
        if (frame == 0)
            LogMsg("[OPENGL_FRAME0] swap returned=%u error=%lu\n",
                   (unsigned)swapResult, (unsigned long)GetLastError());
        if (!swapResult) {
            LogMsg("-> [FAIL] SwapBuffers failed: %lu\n", GetLastError());
            failures++;
            break;
        }
        framesRendered++;
        if ((framesRendered % 10) == 0 || framesRendered == GL_FRAMES) {
            DumpMesaBridgeTrace();
            double intervalEnd = GetTimeSeconds();
            LogMsg("[OPENGL_FRAME_PROGRESS] frames=%d/%d interval_s=%.3f interval_fps=%.1f gpu_batches=%u\n",
                   framesRendered, GL_FRAMES, intervalEnd - intervalStart,
                   intervalEnd > intervalStart ?
                       10.0 / (intervalEnd - intervalStart) : 0.0,
                   mesaGpuBridgeBatchCount - intervalBatches);
            intervalStart = intervalEnd;
            intervalBatches = mesaGpuBridgeBatchCount;
        }
    }
    double tEnd = GetTimeSeconds();

    double dt = tEnd - tStart;
    double fps = dt > 0 ? (double)framesRendered / dt : 0;
    LogMsg("-> Rendered %d frames in %.4f s (%.1f FPS)\n", framesRendered, dt, fps);
    /* Capture the final bridge callback result from the normal application
       workload, not only from the bounded diagnostic probes above. */
    DumpMesaBridgeTrace();

    {
        BYTE pixel[4] = {0};
        GLenum error;
        glReadBuffer(GL_FRONT);
        glReadPixels(w/2, h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        error = glGetError();
        LogMsg("-> OpenGL center pixel: %u,%u,%u GL error=0x%X\n",
            pixel[0], pixel[1], pixel[2], error);
        if (error != GL_NO_ERROR) failures++;
    }
    DumpMesaRuntimeTraces();
    if (directDdiCurrent) {
        /* DrvSetContext(NULL, 0) is the Mesa ICD's supported unbind path. */
        directSetContext(NULL, 0, NULL);
        if (directIcdModule && directDdiHandle && directDeleteContext)
            directDeleteContext(directDdiHandle);
        if (directIcdModule) FreeLibrary(directIcdModule);
    } else {
        wglMakeCurrent(NULL, NULL);
        if (hglrc) wglDeleteContext(hglrc);
    }
    if (mesaGpuBridgeProbe) {
        typedef ULONG_PTR (APIENTRY *PFN_ICD_CREATE_CONTEXT)(HDC);
        typedef BOOL (APIENTRY *PFN_ICD_DELETE_CONTEXT)(ULONG_PTR);
        HMODULE icd = LoadLibraryA("nvxogl64.dll");
        PFN_ICD_CREATE_CONTEXT create = icd ?
            (PFN_ICD_CREATE_CONTEXT)GetProcAddress(icd, "DrvCreateContext") : NULL;
        PFN_ICD_DELETE_CONTEXT destroy = icd ?
            (PFN_ICD_DELETE_CONTEXT)GetProcAddress(icd, "DrvDeleteContext") : NULL;
        ULONG_PTR direct = create ? create(hdc) : 0;
        LogMsg("[MESA_ICD_DIRECT_PROBE] module=%s DrvCreateContext=%p result=%p\n",
               icd ? "loaded" : "missing", (void *)create,
               (void *)(UINT_PTR)direct);
        DumpMesaIcdTrace();
        if (direct && destroy)
            LogMsg("[MESA_ICD_DIRECT_PROBE] DrvDeleteContext=%u\n",
                   (unsigned)destroy(direct));
        if (icd)
            FreeLibrary(icd);
    }
    LogMsg("-> OpenGL rendering test completed; renderer classification reported above.\n");
}

#undef glBegin
#undef glEnd
#undef glVertex3f
#undef glColor3f
#undef glColor4f
#undef glPushMatrix
#undef glPopMatrix
#undef glRotatef
#undef glGetString
#undef glViewport
#undef glMatrixMode
#undef glLoadIdentity
#undef glDisable
#undef glEnable
#undef glClearColor
#undef glClear
#undef glGetError
#undef glReadBuffer
#undef glReadPixels
#undef glFinish
#undef glClearDepth
#undef glDepthFunc
#undef glDepthMask
#undef glFrustum
#undef glTranslatef

/* Exercise Mesa's libgl-gdi frontend without replacing XP's System32
 * opengl32.dll. This is a diagnostic for the ICD DDI route, not a hardware
 * acceleration result: the configured Mesa backend is softpipe. */
static void TestMesaWglFrontend(HINSTANCE instance)
{
    typedef int (WINAPI *PFN_CHOOSE_PIXEL_FORMAT)(HDC, const PIXELFORMATDESCRIPTOR *);
    typedef int (WINAPI *PFN_DESCRIBE_PIXEL_FORMAT)(HDC, int, UINT, PIXELFORMATDESCRIPTOR *);
    typedef BOOL (WINAPI *PFN_SET_PIXEL_FORMAT)(HDC, int, const PIXELFORMATDESCRIPTOR *);
    typedef HGLRC (WINAPI *PFN_CREATE_CONTEXT)(HDC);
    typedef BOOL (WINAPI *PFN_MAKE_CURRENT)(HDC, HGLRC);
    typedef BOOL (WINAPI *PFN_DELETE_CONTEXT)(HGLRC);
    typedef BOOL (WINAPI *PFN_SWAP_BUFFERS)(HDC);
    typedef void (APIENTRY *PFN_CLEAR_COLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
    typedef void (APIENTRY *PFN_CLEAR)(GLbitfield);
    typedef void (APIENTRY *PFN_FINISH)(void);
    typedef void (APIENTRY *PFN_READ_PIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
    typedef const GLubyte *(APIENTRY *PFN_GET_STRING)(GLenum);
    HWND window = NULL;
    HDC dc = NULL;
    HMODULE mesa = NULL;
    HGLRC context = NULL;
    int passed = 0;
    PIXELFORMATDESCRIPTOR pfd;
    BYTE pixel[4] = {0, 0, 0, 0};
    PFN_CHOOSE_PIXEL_FORMAT choose;
    PFN_DESCRIBE_PIXEL_FORMAT describe;
    PFN_SET_PIXEL_FORMAT set;
    PFN_CREATE_CONTEXT create;
    PFN_MAKE_CURRENT makeCurrent;
    PFN_DELETE_CONTEXT deleteContext;
    PFN_SWAP_BUFFERS swap;
    PFN_CLEAR_COLOR clearColor;
    PFN_CLEAR clear;
    PFN_FINISH finish;
    PFN_READ_PIXELS readPixels;
    PFN_GET_STRING getString;

    LogMsg("\n[MESA_WGL_SHIM] Starting direct Mesa WGL frontend diagnostic\n");
    mesa = LoadLibraryA("C:\\test_accel\\nvxmesa.dll");
    if (!mesa) {
        LogMsg("[MESA_WGL_SHIM] DLL load failed err=%lu\n", GetLastError());
        return;
    }
#define MESA_PROC(var, type, symbol) var = (type)GetProcAddress(mesa, symbol)
    MESA_PROC(choose, PFN_CHOOSE_PIXEL_FORMAT, "wglChoosePixelFormat");
    MESA_PROC(describe, PFN_DESCRIBE_PIXEL_FORMAT, "wglDescribePixelFormat");
    MESA_PROC(set, PFN_SET_PIXEL_FORMAT, "wglSetPixelFormat");
    MESA_PROC(create, PFN_CREATE_CONTEXT, "wglCreateContext");
    MESA_PROC(makeCurrent, PFN_MAKE_CURRENT, "wglMakeCurrent");
    MESA_PROC(deleteContext, PFN_DELETE_CONTEXT, "wglDeleteContext");
    MESA_PROC(swap, PFN_SWAP_BUFFERS, "wglSwapBuffers");
    MESA_PROC(clearColor, PFN_CLEAR_COLOR, "glClearColor");
    MESA_PROC(clear, PFN_CLEAR, "glClear");
    MESA_PROC(finish, PFN_FINISH, "glFinish");
    MESA_PROC(readPixels, PFN_READ_PIXELS, "glReadPixels");
    MESA_PROC(getString, PFN_GET_STRING, "glGetString");
#undef MESA_PROC
    if (!choose || !describe || !set || !create || !makeCurrent ||
        !deleteContext || !swap || !clearColor || !clear || !finish ||
        !readPixels || !getString) {
        LogMsg("[MESA_WGL_SHIM] required WGL/GL export missing\n");
        goto done;
    }
    window = CreateWindowA("STATIC", "Mesa WGL diagnostic", WS_POPUP,
                           0, 0, 64, 64, NULL, NULL, instance, NULL);
    if (!window) {
        LogMsg("[MESA_WGL_SHIM] window creation failed err=%lu\n", GetLastError());
        goto done;
    }
    dc = GetDC(window);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int format = choose(dc, &pfd);
    LogMsg("[MESA_WGL_SHIM] pixel format count=%d chosen=%d\n",
           describe(dc, 0, 0, NULL), format);
    if (!dc || format <= 0 || !set(dc, format, &pfd)) {
        LogMsg("[MESA_WGL_SHIM] pixel format setup failed err=%lu\n", GetLastError());
        goto done;
    }
    context = create(dc);
    LogMsg("[MESA_WGL_SHIM] create context=%p err=%lu\n",
           (void *)context, GetLastError());
    if (!context || !makeCurrent(dc, context)) {
        LogMsg("[MESA_WGL_SHIM] make current failed err=%lu\n", GetLastError());
        goto done;
    }
    LogMsg("[MESA_WGL_SHIM] vendor=%s renderer=%s version=%s\n",
           getString(GL_VENDOR), getString(GL_RENDERER), getString(GL_VERSION));
    clearColor(0.125f, 0.25f, 0.75f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    finish();
    readPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    BOOL swapped = swap(dc);
    LogMsg("[MESA_WGL_SHIM] pixel=%u,%u,%u,%u swap=%u\n",
           pixel[0], pixel[1], pixel[2], pixel[3], (unsigned)swapped);
    passed = pixel[0] > 16 && pixel[1] > 40 && pixel[2] > 150 && swapped;
    LogMsg("[MESA_WGL_SHIM] %s (software backend diagnostic only)\n",
           passed ? "PASS" : "FAIL");
done:
    if (context) {
        makeCurrent(NULL, NULL);
        deleteContext(context);
    }
    if (dc && window) ReleaseDC(window, dc);
    if (window) DestroyWindow(window);
    if (mesa) FreeLibrary(mesa);
}

static int WaitNvx3dFence(HDC hdc,uint64_t fenceValue)
{
    NVX_3D_WAIT_FENCE wait={0},waited={0};
    wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
    wait.FenceValue=fenceValue;wait.TimeoutMs=10000;
    return ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),
                     (LPCSTR)&wait,sizeof(waited),(LPSTR)&waited)!=0&&
           waited.Status==NVX_3D_STATUS_SUCCESS&&
           waited.FenceValue==fenceValue;
}

#if defined(NVX_MESA_DEPTH_MODE_OVERRIDE) && NVX_MESA_DEPTH_MODE_OVERRIDE == 2
static void TestSelectedZetaFullSurface(HDC hdc)
{
    const uint32_t width=632U,height=453U;
    const uint32_t repeats=600U;
    static const float probeTriangle[24]={
        -0.6f,-0.6f,0.5f,1.0f, 0.5f,1.0f,0.0f,1.0f,
         0.6f,-0.6f,0.5f,1.0f, 0.5f,1.0f,0.0f,1.0f,
         0.0f, 0.6f,0.5f,1.0f, 0.5f,1.0f,0.0f,1.0f
    };
    /* Captured from the first WGL batch that stalled at GPGet 0x73 in
       test_logs/v3.0.0.1-20261110-230727.log. */
    static const uint32_t stalledBatchBits[48]={
        0x3e32f9c0U,0xbe79b254U,0x3f4f1902U,0x3f800000U,
        0x3f4f1902U,0x3f800000U,0x00000000U,0x3f800000U,
        0x3d2dee80U,0xbd72a8d0U,0x3f5ba4d0U,0x3f800000U,
        0x3f5ba4d0U,0x3f800000U,0x00000000U,0x3f800000U,
        0x3d93ac10U,0x3e798b20U,0x3f561240U,0x3f800000U,
        0x3f561240U,0x3f800000U,0x00000000U,0x3f800000U,
        0x3e32f9c0U,0xbe79b254U,0x3f4f1902U,0x3f800000U,
        0x3f4f1902U,0x3f800000U,0x00000000U,0x3f800000U,
        0x3d93ac10U,0x3e798b20U,0x3f561240U,0x3f800000U,
        0x3f561240U,0x3f800000U,0x00000000U,0x3f800000U,
        0x3e77ebe0U,0x3e0ec840U,0x3f44e2a6U,0x3f800000U,
        0x3f44e2a6U,0x3f800000U,0x00000000U,0x3f800000U
    };
    static const uint32_t probeColors[6]={
        0xff00ff00U,0xffff0000U,0xff0000ffU,
        0xffffff00U,0xff00ffffU,0xffff00ffU
    };
    NVX_3D_COLOR_SURFACE_LAYOUT colorLayout;
    NVX_3D_DEPTH_SURFACE_LAYOUT depthLayout;
    NVX_3D_ALLOC_BUFFER colorAlloc={0},color={0};
    NVX_3D_ALLOC_BUFFER depthAlloc={0},depth={0};
    NVX_3D_ALLOC_BUFFER vertexAlloc={0},vertex={0};
    NVX_3D_SUBMIT submit={0},submitted={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    struct { NVX_3D_UPLOAD_BUFFER header; float data[48]; } upload;
    float probeVertices[48];
    const char *stage="color allocation";
    uint32_t draw;
    LogMsg("[NVX_SELECTED_ZETA_PROBE] begin width=%lu height=%lu\n",
           (unsigned long)width,(unsigned long)height);
    if(!Nvx3dComputeColorSurfaceLayout(width,height,&colorLayout)||
       !Nvx3dComputeDepthSurfaceLayout(width,height,&depthLayout)) {
        LogMsg("[NVX_SELECTED_ZETA_PROBE] FAIL surface layout\n");
        failures++;
        return;
    }
    colorAlloc.Size=sizeof(colorAlloc);
    colorAlloc.Version=NVX_3D_ABI_VERSION;
    colorAlloc.Usage=NVX_3D_USAGE_COLOR_TARGET;
    colorAlloc.Bytes=colorLayout.Bytes;
    colorAlloc.Width=width;colorAlloc.Height=height;
    colorAlloc.PitchBytes=colorLayout.PitchBytes;
    colorAlloc.TileMode=colorLayout.TileMode;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(colorAlloc),
                  (LPCSTR)&colorAlloc,sizeof(color),(LPSTR)&color))
        goto fail;
    stage="depth allocation";
    depthAlloc.Size=sizeof(depthAlloc);
    depthAlloc.Version=NVX_3D_ABI_VERSION;
    depthAlloc.Usage=NVX_3D_USAGE_DEPTH_TARGET|NVX_3D_USAGE_DEPTH_ZF32;
    depthAlloc.Bytes=NVX_3D_DEPTH_TARGET_OFFSET+depthLayout.Bytes;
    depthAlloc.Width=width;depthAlloc.Height=height;
    depthAlloc.PitchBytes=depthLayout.PitchBytes;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(depthAlloc),
                  (LPCSTR)&depthAlloc,sizeof(depth),(LPSTR)&depth))
        goto fail;
    stage="vertex allocation";
    vertexAlloc.Size=sizeof(vertexAlloc);
    vertexAlloc.Version=NVX_3D_ABI_VERSION;
    vertexAlloc.Usage=1U;vertexAlloc.Bytes=4096U;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(vertexAlloc),
                  (LPCSTR)&vertexAlloc,sizeof(vertex),(LPSTR)&vertex)
       ) goto fail;
    LogMsg("[NVX_SELECTED_ZETA_PROBE] ids color=%llu depth=%llu vertex=%llu va=%llx/%llx/%llx\n",
           (unsigned long long)color.BufferId,
           (unsigned long long)depth.BufferId,
           (unsigned long long)vertex.BufferId,
           (unsigned long long)color.GpuVa,
           (unsigned long long)depth.GpuVa,
           (unsigned long long)vertex.GpuVa);
    /* Mode 2 neither tests nor writes depth, so leave both targets
       untouched and make the selected-Zeta draw the first GR operation. */
    memset(&upload,0,sizeof(upload));
    upload.header.Size=sizeof(upload.header);
    upload.header.Version=NVX_3D_ABI_VERSION;
    upload.header.BufferId=vertex.BufferId;
    upload.header.Bytes=sizeof(upload.data);
    stage="selected-Zeta draw";
    memset(&submit,0,sizeof(submit));
    submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
    submit.Operation=NVX_3D_SUBMIT_DRAW_COLOR_TARGET_DEPTH_ATTACH;
    submit.Width=width;submit.Height=height;submit.BufferId=color.BufferId;
    submit.Color=0xff00ff00U;
    submit.Reserved[0]=6U;
    submit.Reserved[1]=(uint32_t)vertex.BufferId;
    submit.Reserved[2]=(uint32_t)(vertex.BufferId>>32);
    submit.Reserved[3]=colorLayout.TileMode;
    submit.Reserved[4]=(uint32_t)depth.BufferId;
    submit.Reserved[5]=(uint32_t)(depth.BufferId>>32);
    for(draw=0;draw<repeats;draw++) {
        float xShift=((float)(draw%9U)-4.0f)*0.025f;
        float z=0.35f+(float)(draw%5U)*0.05f;
        uint32_t vertexIndex;
        if(draw==0U) {
            memcpy(upload.data,stalledBatchBits,sizeof(stalledBatchBits));
            submit.Color=0xff0000ffU;
        } else {
            memcpy(upload.data,probeTriangle,sizeof(probeTriangle));
            for(vertexIndex=0;vertexIndex<3U;vertexIndex++) {
                upload.data[vertexIndex*8U+0U]+=xShift;
                upload.data[vertexIndex*8U+2U]=z;
                upload.data[vertexIndex*8U+4U]=z;
            }
            memcpy(probeVertices,upload.data,sizeof(probeTriangle));
            memcpy(probeVertices+24U,upload.data,sizeof(probeTriangle));
            memcpy(upload.data,probeVertices,sizeof(probeVertices));
            submit.Color=probeColors[draw%6U];
        }
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,sizeof(upload),
                      (LPCSTR)&upload,0,NULL)) {
            LogMsg("[NVX_SELECTED_ZETA_PROBE] FAIL vertex upload draw=%u winerr=%lu\n",
                   (unsigned)draw,(unsigned long)GetLastError());
            failures++;
            return;
        }
        memset(&submitted,0,sizeof(submitted));
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(submit),(LPCSTR)&submit,
                      sizeof(submitted),(LPSTR)&submitted)||!submitted.FenceValue) {
            LogMsg("[NVX_SELECTED_ZETA_PROBE] FAIL full-size submit draw=%u winerr=%lu\n",
                   (unsigned)draw,(unsigned long)GetLastError());
            failures++;
            return;
        }
        if(!WaitNvx3dFence(hdc,submitted.FenceValue)) {
            LogMsg("[NVX_SELECTED_ZETA_PROBE] FAIL draw=%u fence=%llu\n",
                   (unsigned)draw,(unsigned long long)submitted.FenceValue);
            failures++;
            return;
        }
        if((draw+1U)%100U==0U)
            LogMsg("[NVX_SELECTED_ZETA_PROBE] progress=%u/%u fence=%llu\n",
                   (unsigned)(draw+1U),(unsigned)repeats,
                   (unsigned long long)submitted.FenceValue);
    }
    LogMsg("[NVX_SELECTED_ZETA_PROBE] PASS draws=%u pitch=%lu tile=%lu fence=%llu\n",
           (unsigned)repeats,
           (unsigned long)depthLayout.PitchBytes,
           (unsigned long)colorLayout.TileMode,
           (unsigned long long)submitted.FenceValue);
    freeRequest.Size=sizeof(freeRequest);freeRequest.Version=NVX_3D_ABI_VERSION;
    freeRequest.BufferId=vertex.BufferId;
    ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),(LPCSTR)&freeRequest,0,NULL);
    freeRequest.BufferId=depth.BufferId;
    ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),(LPCSTR)&freeRequest,0,NULL);
    freeRequest.BufferId=color.BufferId;
    ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),(LPCSTR)&freeRequest,0,NULL);
    return;

fail:
    LogMsg("[NVX_SELECTED_ZETA_PROBE] FAIL stage=%s winerr=%lu\n",
           stage,(unsigned long)GetLastError());
    failures++;
    /* Retain allocations on failure: the queue may still reference them
       until channel teardown. */
}
#endif

static int TestNvc0ClearEscape(HDC hdc)
{
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    NVX_3D_ALLOC_BUFFER alloc={0},allocated={0};
    NVX_3D_NVC0_CLEAR clear={0},submitted={0};
    uint8_t batch[sizeof(NVX_3D_BATCH_HEADER)+
                  sizeof(NVX_3D_BUFFER_REFERENCE)+
                  NVX_3D_NVC0_CLEAR_DWORDS*sizeof(uint32_t)+
                  2*sizeof(NVX_3D_RELOCATION)];
    NVX_3D_BATCH_HEADER batchHeader={0};
    NVX_3D_BUFFER_REFERENCE batchReference={0};
    NVX_3D_RELOCATION batchRelocations[2]={{0}};
    NVX_3D_WAIT_FENCE wait={0},waited={0};
    NVX_3D_READBACK readback={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    uint32_t pixels[NVX_3D_READBACK_PIXELS];
#if defined(NVX_TEST_NOUVEAU_PUSHBUF)
    NVX_WINSYS_DEVICE winsys={0};
    NVX_NOUVEAU_BO pushBo={0};
    NVX_NOUVEAU_PUSHBUF push={0};
#endif
    static const uint32_t clearCases[][5]={
        {0x3f000000U,0x3e800000U,0x3f800000U,0x3f800000U,0xff8040ffU},
        {0x3f800000U,0x00000000U,0x00000000U,0x3f800000U,0xffff0000U},
        {0x00000000U,0x3f800000U,0x00000000U,0x3f800000U,0xff00ff00U},
        {0x00000000U,0x00000000U,0x3f800000U,0x3f800000U,0xff0000ffU}
    };
    uint64_t previousFence=0;
    unsigned i;
    int ok=1,allocatedOk=0;

    LogMsg("-> Testing one validated 28-dword NVC0 full-target clear packet\n");
    if(!Nvx3dComputeColorSurfaceLayout(64,16,&layout)) return 0;
    alloc.Size=sizeof(alloc);alloc.Version=NVX_3D_ABI_VERSION;
    alloc.Usage=NVX_3D_USAGE_COLOR_TARGET;alloc.Bytes=layout.Bytes;
    alloc.Width=layout.Width;alloc.Height=layout.Height;
    alloc.PitchBytes=layout.PitchBytes;alloc.TileMode=layout.TileMode;
    alloc.ClientToken=((uint64_t)GetCurrentProcessId()<<32)|GetTickCount();
    if(!alloc.ClientToken) alloc.ClientToken=1;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(alloc),(LPCSTR)&alloc,
                  sizeof(allocated),(LPSTR)&allocated)) {
        LogMsg("-> [FAIL] NVC0 clear target allocation failed\n");
        return 0;
    }
    allocatedOk=1;
    LogMsg("[NVX_3D_STAGE] resource PASS buffer=%llu bytes=%lu\n",
           (unsigned long long)allocated.BufferId,
           (unsigned long)allocated.Bytes);
    clear.Size=sizeof(clear);clear.Version=NVX_3D_ABI_VERSION;
    clear.DwordCount=NVX_3D_NVC0_CLEAR_DWORDS;
    clear.BufferId=allocated.BufferId;
    clear.ClientToken=allocated.ClientToken;
    clear.Dwords[0]=Nvx3dNvc0SeqHeader(0x0d80U,4U);
    clear.Dwords[1]=0x3f000000U;clear.Dwords[2]=0x3e800000U;
    clear.Dwords[3]=0x3f800000U;clear.Dwords[4]=0x3f800000U;
    clear.Dwords[5]=Nvx3dNvc0SeqHeader(0x0ff4U,2U);
    clear.Dwords[6]=layout.Width<<16;clear.Dwords[7]=layout.Height<<16;
    clear.Dwords[8]=Nvx3dNvc0SeqHeader(0x0d6cU,2U);
    clear.Dwords[9]=layout.Width<<16;clear.Dwords[10]=layout.Height<<16;
    clear.Dwords[11]=Nvx3dNvc0SeqHeader(0x10f8U,1U);clear.Dwords[12]=0x10U;
    clear.Dwords[13]=Nvx3dNvc0SeqHeader(0x121cU,1U);clear.Dwords[14]=1U;
    clear.Dwords[15]=Nvx3dNvc0SeqHeader(0x0800U,9U);
    clear.Dwords[16]=(uint32_t)(allocated.GpuVa>>32);
    clear.Dwords[17]=(uint32_t)allocated.GpuVa;
    clear.Dwords[18]=layout.Width;clear.Dwords[19]=layout.Height;
    clear.Dwords[20]=0xcfU;clear.Dwords[21]=layout.TileMode;
    clear.Dwords[22]=1U;clear.Dwords[23]=allocated.Bytes/4U;
    clear.Dwords[24]=0U;
    clear.Dwords[25]=Nvx3dNvc0ImmediateHeader(0x15d0U,0U);
    clear.Dwords[26]=Nvx3dNvc0NonIncHeader(0x19d0U,1U);
    clear.Dwords[27]=0x3cU;
    batchHeader.Size=sizeof(batchHeader);
    batchHeader.Version=NVX_3D_ABI_VERSION;
    batchHeader.DwordCount=NVX_3D_NVC0_CLEAR_DWORDS;
    batchHeader.BufferReferenceCount=1;
    batchHeader.RelocationCount=2;
    batchHeader.ClientToken=allocated.ClientToken;
    batchReference.BufferId=allocated.BufferId;
    batchReference.Access=NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE;
    batchRelocations[0].CommandWord=16;
    batchRelocations[0].Access=NVX_3D_RELOC_READ|NVX_3D_RELOC_WRITE;
    batchRelocations[0].BufferId=allocated.BufferId;
    batchRelocations[0].BufferOffset=0;
    batchRelocations[0].Bytes=allocated.Bytes;
    batchRelocations[1]=batchRelocations[0];
    batchRelocations[1].CommandWord=17;
    memcpy(batch,&batchHeader,sizeof(batchHeader));
    memcpy(batch+sizeof(batchHeader),&batchReference,sizeof(batchReference));
    memcpy(batch+sizeof(batchHeader)+sizeof(batchReference),
           clear.Dwords,sizeof(clear.Dwords));
    memcpy(batch+sizeof(batchHeader)+sizeof(batchReference)+sizeof(clear.Dwords),
           batchRelocations,sizeof(batchRelocations));
#if defined(NVX_TEST_NOUVEAU_PUSHBUF)
    if(!NvxWinsysInitHdc(&winsys,hdc)) {
        LogMsg("-> [FAIL] Nouveau pushbuffer adapter could not query HDC caps\n");
        ok=0;
    } else {
        pushBo.device=&winsys;
        pushBo.allocation=allocated;
        pushBo.references=1;
        if(!NvxNouveauPushbufInit(&push,&winsys,allocated.ClientToken)||
           !NvxNouveauPushbufAppend(&push,clear.Dwords,
                                    NVX_3D_NVC0_CLEAR_DWORDS)||
           !NvxNouveauPushbufRelocate(&push,&pushBo,16,0,allocated.Bytes,
                                      NVX_NOUVEAU_BO_READ|
                                      NVX_NOUVEAU_BO_WRITE)||
           !NvxNouveauPushbufRelocate(&push,&pushBo,17,0,allocated.Bytes,
                                      NVX_NOUVEAU_BO_READ|
                                      NVX_NOUVEAU_BO_WRITE)||
           !NvxNouveauPushbufValidate(&push)) {
            LogMsg("-> [FAIL] Nouveau pushbuffer adapter/miniport preflight failed\n");
            ok=0;
        } else {
            LogMsg("-> Nouveau pushbuffer adapter serialized two owned BO relocations and passed miniport preflight\n");
        }
        NvxNouveauPushbufDestroy(&push);
    }
#else
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(batch),
                  (LPCSTR)&batch,0,NULL)) {
        LogMsg("-> [FAIL] miniport rejected structurally valid NVC0 batch metadata\n");
        ok=0;
    } else {
        LogMsg("-> NVC0 batch framing and BO relocations passed miniport validation\n");
    }
#endif
    if(ok)
        LogMsg("[NVX_3D_STAGE] batch-preflight PASS dwords=%u relocations=2\n",
               (unsigned)batchHeader.DwordCount);
    /* Fast negative cases exercise the guest parser's bounds and BO ownership
       checks without submitting any modified command stream to the GPU. */
    {
        NVX_3D_BATCH_HEADER invalidHeader=batchHeader;
        uint8_t invalidBatch[sizeof(batch)];
        NVX_3D_NVC0_CLEAR invalidClear;
        memcpy(invalidBatch,batch,sizeof(batch));
        invalidHeader.Version++;
        memcpy(invalidBatch,&invalidHeader,sizeof(invalidHeader));
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted unsupported batch ABI version\n");
            ok=0;
        } else {
            LogMsg("-> Rejected unsupported batch ABI version\n");
        }
        memcpy(invalidBatch,batch,sizeof(batch));
        {
            uint32_t invalidMethodHeader=Nvx3dNvc0SeqHeader(0x7ffcU,4U);
            memcpy(invalidBatch+sizeof(batchHeader),&invalidMethodHeader,
                   sizeof(invalidMethodHeader));
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted incrementing method wrap\n");
            ok=0;
        } else {
            LogMsg("-> Rejected incrementing packet that wraps its method field\n");
        }
        memcpy(invalidBatch,batch,sizeof(batch));
        invalidHeader=batchHeader;
        invalidHeader.ClientToken^=1;
        memcpy(invalidBatch,&invalidHeader,sizeof(invalidHeader));
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted batch with another client's token\n");
            ok=0;
        } else {
            LogMsg("-> Rejected batch with another client's ownership token\n");
        }
        memcpy(invalidBatch,batch,sizeof(batch));
        {
            NVX_3D_RELOCATION invalidRelocation=batchRelocations[1];
            invalidRelocation.BufferOffset=allocated.Bytes;
            memcpy(invalidBatch+sizeof(batchHeader)+sizeof(clear.Dwords)+
                   sizeof(batchRelocations[0]),&invalidRelocation,
                   sizeof(invalidRelocation));
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted out-of-range BO relocation\n");
            ok=0;
        } else {
            LogMsg("-> Rejected out-of-range BO relocation\n");
        }

        /* Exercise the remaining relocation invariants at the guest boundary.
           These are parser-only requests: none reaches the graphics queue. */
        memcpy(invalidBatch,batch,sizeof(batch));
        {
            NVX_3D_RELOCATION invalidRelocation=batchRelocations[0];
            invalidRelocation.Reserved[0]=1;
            memcpy(invalidBatch+sizeof(batchHeader)+sizeof(clear.Dwords),
                   &invalidRelocation,sizeof(invalidRelocation));
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted nonzero relocation reserved field\n");
            ok=0;
        } else {
            LogMsg("-> Rejected nonzero relocation reserved field\n");
        }
        memcpy(invalidBatch,batch,sizeof(batch));
        {
            NVX_3D_RELOCATION invalidRelocation=batchRelocations[1];
            invalidRelocation.CommandWord=batchRelocations[0].CommandWord;
            memcpy(invalidBatch+sizeof(batchHeader)+sizeof(clear.Dwords)+
                   sizeof(batchRelocations[0]),&invalidRelocation,
                   sizeof(invalidRelocation));
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted duplicate relocation word\n");
            ok=0;
        } else {
            LogMsg("-> Rejected duplicate relocation word\n");
        }
        memcpy(invalidBatch,batch,sizeof(batch));
        {
            NVX_3D_RELOCATION invalidRelocation=batchRelocations[0];
            invalidRelocation.Access|=0x80U;
            memcpy(invalidBatch+sizeof(batchHeader)+sizeof(clear.Dwords),
                   &invalidRelocation,sizeof(invalidRelocation));
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_VALIDATE_BATCH,sizeof(invalidBatch),
                     (LPCSTR)invalidBatch,0,NULL)) {
            LogMsg("-> [FAIL] miniport accepted unknown relocation access bits\n");
            ok=0;
        } else {
            LogMsg("-> Rejected unknown relocation access bits\n");
        }

        /* Clear packets have their own strict method and surface checks.
           These requests must be rejected before the GPU queue is touched. */
        invalidClear=clear;
        invalidClear.Dwords[0]=0;
        if(ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_CLEAR,sizeof(invalidClear),
                     (LPCSTR)&invalidClear,sizeof(submitted),
                     (LPSTR)&submitted)) {
            LogMsg("-> [FAIL] miniport accepted a malformed NVC0 method header\n");
            ok=0;
        } else {
            LogMsg("-> Rejected malformed NVC0 method header\n");
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR,sizeof(invalidClear),
                     (LPCSTR)&invalidClear,sizeof(submitted),
                     (LPSTR)&submitted)) {
            LogMsg("-> [FAIL] native path accepted a malformed NVC0 method header\n");
            ok=0;
        } else {
            LogMsg("-> Native path rejected malformed NVC0 method header\n");
        }
        invalidClear=clear;
        invalidClear.Dwords[1]=0xbf800000U; /* negative color component */
        if(ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_CLEAR,sizeof(invalidClear),
                     (LPCSTR)&invalidClear,sizeof(submitted),
                     (LPSTR)&submitted)) {
            LogMsg("-> [FAIL] miniport accepted a negative NVC0 UNORM component\n");
            ok=0;
        } else {
            LogMsg("-> Rejected out-of-range NVC0 color component\n");
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR,sizeof(invalidClear),
                     (LPCSTR)&invalidClear,sizeof(submitted),
                     (LPSTR)&submitted)) {
            LogMsg("-> [FAIL] native path accepted an out-of-range UNORM component\n");
            ok=0;
        } else {
            LogMsg("-> Native path rejected out-of-range UNORM component\n");
        }
        invalidClear=clear;
        invalidClear.Dwords[17]^=0x10000U; /* wrong target address */
        if(ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_CLEAR,sizeof(invalidClear),
                     (LPCSTR)&invalidClear,sizeof(submitted),
                     (LPSTR)&submitted)) {
            LogMsg("-> [FAIL] miniport accepted an NVC0 target address outside the owned BO\n");
            ok=0;
        } else {
            LogMsg("-> Rejected NVC0 target address outside the owned BO\n");
        }
        if(ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR,sizeof(invalidClear),
                     (LPCSTR)&invalidClear,sizeof(submitted),
                     (LPSTR)&submitted)) {
            LogMsg("-> [FAIL] native path accepted a target address outside the owned BO\n");
            ok=0;
        } else {
            LogMsg("-> Native path rejected target address outside the owned BO\n");
        }
    }
    {
        NVX_3D_NVC0_CLEAR nativeSubmitted={0};
        /* Direct C797 UNORM conversion truncates the 0.5 red channel to 127;
           the trusted semantic clear path below rounds it to 128. */
        uint32_t expectedNative=0xff7f40ffU;
        unsigned nativePixel;
        wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
        wait.TimeoutMs=10000;
        readback.Size=sizeof(readback);readback.Version=NVX_3D_ABI_VERSION;
        readback.BufferId=allocated.BufferId;
        readback.Width=layout.Width;readback.Height=layout.Height;
#if defined(NVX_TEST_NOUVEAU_PUSHBUF)
        {
            uint64_t adapterFence=0;
            LogMsg("-> Testing Nouveau adapter submission of the validated NVC0 clear stream\n");
            if(!NvxNouveauPushbufInit(&push,&winsys,allocated.ClientToken)||
               !NvxNouveauPushbufAppend(&push,clear.Dwords,
                                        NVX_3D_NVC0_CLEAR_DWORDS)||
               !NvxNouveauPushbufRelocate(&push,&pushBo,16,0,allocated.Bytes,
                                          NVX_NOUVEAU_BO_READ|
                                          NVX_NOUVEAU_BO_WRITE)||
               !NvxNouveauPushbufRelocate(&push,&pushBo,17,0,allocated.Bytes,
                                          NVX_NOUVEAU_BO_READ|
                                          NVX_NOUVEAU_BO_WRITE)||
               !NvxNouveauPushbufSubmitNvc0Clear(&push,&pushBo,
                                                  &adapterFence)) {
                LogMsg("-> [FAIL] Nouveau adapter native clear submission failed\n");
                ok=0;
            } else {
                nativeSubmitted=clear;
                nativeSubmitted.FenceValue=adapterFence;
                LogMsg("-> Nouveau adapter submitted bounded clear; fence=%llu\n",
                       (unsigned long long)adapterFence);
            }
            NvxNouveauPushbufDestroy(&push);
        }
#else
        LogMsg("-> Testing direct submission of the validated NVC0 clear stream\n");
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR,sizeof(clear),
                      (LPCSTR)&clear,sizeof(nativeSubmitted),
                      (LPSTR)&nativeSubmitted)) {
            LogMsg("-> [FAIL] miniport/GPU rejected the bounded native NVC0 clear stream\n");
            ok=0;
        }
#endif
        if(ok) {
            wait.FenceValue=nativeSubmitted.FenceValue;
            memset(&waited,0,sizeof(waited));
            if(!nativeSubmitted.FenceValue||
               !ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                          sizeof(waited),(LPSTR)&waited)||
               waited.FenceValue!=nativeSubmitted.FenceValue||
               waited.Status!=NVX_3D_STATUS_SUCCESS||
               !ExtEscape(hdc,NVX_ESCAPE_3D_READBACK,sizeof(readback),
                          (LPCSTR)&readback,sizeof(pixels),(LPSTR)pixels)) {
                LogMsg("-> [FAIL] native NVC0 clear fence/readback did not complete\n");
                ok=0;
            } else {
                for(nativePixel=0;nativePixel<NVX_3D_READBACK_PIXELS;
                    nativePixel++) {
                    if(pixels[nativePixel]!=expectedNative) {
                        LogMsg("-> [FAIL] native NVC0 clear pixel %u was 0x%08lX, expected 0x%08lX\n",
                               nativePixel,(unsigned long)pixels[nativePixel],
                               (unsigned long)expectedNative);
                        ok=0;
                        break;
                    }
                }
                if(ok)
                    LogMsg("-> Native NVC0 packet executed by GPU; 1024 pixels match 0x%08lX, fence=%llu\n",
                           (unsigned long)expectedNative,
                           (unsigned long long)nativeSubmitted.FenceValue);
                if(ok)
                    LogMsg("[NVX_3D_STAGE] native-submit-fence-readback PASS pixels=%u fence=%llu\n",
                           (unsigned)NVX_3D_READBACK_PIXELS,
                           (unsigned long long)nativeSubmitted.FenceValue);
            }
        }
        LogMsg("[NVX_NVC0_NATIVE_CLEAR_RESULT] %s\n",ok?"PASS":"FAIL");
    }
    /* Repeat the real C797 clear with distinct channel patterns. This catches
       stale target contents, channel swizzles, and fences that do not advance
       while keeping every submitted packet on the proven clear-only path. */
    wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
    wait.TimeoutMs=10000;
    readback.Size=sizeof(readback);readback.Version=NVX_3D_ABI_VERSION;
    readback.BufferId=allocated.BufferId;
    readback.Width=layout.Width;readback.Height=layout.Height;
    for(unsigned pass=0;pass<sizeof(clearCases)/sizeof(clearCases[0]);pass++) {
        uint32_t expected=clearCases[pass][4];
        clear.Dwords[1]=clearCases[pass][0];
        clear.Dwords[2]=clearCases[pass][1];
        clear.Dwords[3]=clearCases[pass][2];
        clear.Dwords[4]=clearCases[pass][3];
        memset(&submitted,0,sizeof(submitted));
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_CLEAR,sizeof(clear),
                      (LPCSTR)&clear,sizeof(submitted),(LPSTR)&submitted)) {
            LogMsg("-> [FAIL] NVC0 clear case %u was rejected\n",pass);
            ok=0;
            break;
        }
        if(!submitted.FenceValue||submitted.FenceValue<=previousFence) {
            LogMsg("-> [FAIL] NVC0 clear case %u fence did not advance (%llu after %llu)\n",
                   pass,(unsigned long long)submitted.FenceValue,
                   (unsigned long long)previousFence);
            ok=0;
            break;
        }
        previousFence=submitted.FenceValue;
        wait.FenceValue=submitted.FenceValue;
        memset(&waited,0,sizeof(waited));
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                      sizeof(waited),(LPSTR)&waited) ||
           waited.FenceValue!=submitted.FenceValue ||
           waited.Status!=NVX_3D_STATUS_SUCCESS) {
            LogMsg("-> [FAIL] NVC0 clear case %u fence wait failed\n",pass);
            ok=0;
            break;
        }
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_READBACK,sizeof(readback),
                      (LPCSTR)&readback,sizeof(pixels),(LPSTR)pixels)) {
            LogMsg("-> [FAIL] NVC0 clear case %u readback failed\n",pass);
            ok=0;
            break;
        }
        for(i=0;i<NVX_3D_READBACK_PIXELS;i++) {
            if(pixels[i]!=expected) {
                LogMsg("-> [FAIL] NVC0 clear case %u pixel %u was 0x%08lX, expected 0x%08lX\n",
                       pass,i,(unsigned long)pixels[i],(unsigned long)expected);
                ok=0;
                break;
            }
        }
        if(!ok) break;
        LogMsg("-> NVC0 clear case %u: 1024 pixels match 0x%08lX; fence=%llu\n",
               pass,(unsigned long)expected,
               (unsigned long long)submitted.FenceValue);
    }
    if(ok)
        LogMsg("[NVX_3D_STAGE] semantic-clear-color-matrix PASS cases=%u\n",
               (unsigned)(sizeof(clearCases)/sizeof(clearCases[0])));
    if(allocatedOk) {
        freeRequest.Size=sizeof(freeRequest);
        freeRequest.Version=NVX_3D_ABI_VERSION;
        freeRequest.BufferId=allocated.BufferId;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),
                      (LPCSTR)&freeRequest,0,NULL)) {
            LogMsg("-> [FAIL] NVC0 clear target release failed\n");
            ok=0;
        } else if(ok)
            LogMsg("[NVX_3D_STAGE] resource-release PASS\n");
    }
#if defined(NVX_TEST_NOUVEAU_PUSHBUF)
    if(winsys.ClientToken && !NvxWinsysCloseClient(&winsys)) {
        LogMsg("-> [WARN] Nouveau adapter session close failed\n");
        ok=0;
    }
#endif
    LogMsg("[NVX_NVC0_CLEAR_RESULT] %s\n",ok?"PASS":"FAIL");
    return ok;
}

/* Re-run the exact whitelisted NVC0 clear against a full WGL-sized target.
   This isolates the block-linear extent/address calculation from Mesa's draw
   state and checks both the origin and a later aligned tile by readback. */
static int TestNativeNvc0FullSizeClear(HDC hdc)
{
    static const uint32_t clearCases[][5]={
        {0x3f800000U,0U,0x3f800000U,0x3f800000U,0xffff00ffU},
        {0U,0U,0U,0x3f800000U,0xff000000U},
        {0x3f800000U,0x3f800000U,0x3f800000U,0x3f800000U,0xffffffffU},
        {0U,0x3f800000U,0x3f800000U,0x3f800000U,0xff00ffffU},
    };
    const uint32_t width=632U,height=453U;
    NVX_3D_COLOR_SURFACE_LAYOUT layout;
    NVX_3D_ALLOC_BUFFER alloc={0},allocated={0};
    NVX_3D_NVC0_CLEAR clear={0},submitted={0};
    NVX_3D_WAIT_FENCE wait={0},waited={0};
    NVX_3D_DOWNLOAD_BUFFER download={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    const uint32_t sampleX[3]={0,width/2U,width-1U};
    const uint32_t sampleY[3]={0,height/2U,height-1U};
    unsigned pass;
    int allocatedOk=0,ok=0;

    if(!Nvx3dComputeColorSurfaceLayout(width,height,&layout)||
       layout.Bytes>NVX_3D_MAX_BUFFER_BYTES) {
        LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] FAIL layout\n");
        return 0;
    }
    alloc.Size=sizeof(alloc);alloc.Version=NVX_3D_ABI_VERSION;
    alloc.Usage=NVX_3D_USAGE_COLOR_TARGET;alloc.Bytes=layout.Bytes;
    alloc.Width=width;alloc.Height=height;alloc.PitchBytes=layout.PitchBytes;
    alloc.TileMode=layout.TileMode;
    alloc.ClientToken=((uint64_t)GetCurrentProcessId()<<32)|GetTickCount();
    if(!alloc.ClientToken) alloc.ClientToken=1;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(alloc),(LPCSTR)&alloc,
                  sizeof(allocated),(LPSTR)&allocated)) {
        LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] FAIL allocation\n");
        return 0;
    }
    allocatedOk=1;

    clear.Size=sizeof(clear);clear.Version=NVX_3D_ABI_VERSION;
    clear.DwordCount=NVX_3D_NVC0_CLEAR_DWORDS;
    clear.BufferId=allocated.BufferId;
    clear.ClientToken=allocated.ClientToken;
    clear.Dwords[0]=Nvx3dNvc0SeqHeader(0x0d80U,4U);
    clear.Dwords[1]=0x3f800000U;clear.Dwords[2]=0U;
    clear.Dwords[3]=0x3f800000U;clear.Dwords[4]=0x3f800000U;
    clear.Dwords[5]=Nvx3dNvc0SeqHeader(0x0ff4U,2U);
    clear.Dwords[6]=width<<16;clear.Dwords[7]=height<<16;
    clear.Dwords[8]=Nvx3dNvc0SeqHeader(0x0d6cU,2U);
    clear.Dwords[9]=width<<16;clear.Dwords[10]=height<<16;
    clear.Dwords[11]=Nvx3dNvc0SeqHeader(0x10f8U,1U);clear.Dwords[12]=0x10U;
    clear.Dwords[13]=Nvx3dNvc0SeqHeader(0x121cU,1U);clear.Dwords[14]=1U;
    clear.Dwords[15]=Nvx3dNvc0SeqHeader(0x0800U,9U);
    clear.Dwords[16]=(uint32_t)(allocated.GpuVa>>32);
    clear.Dwords[17]=(uint32_t)allocated.GpuVa;
    clear.Dwords[18]=width;clear.Dwords[19]=height;
    clear.Dwords[20]=0xcfU;clear.Dwords[21]=layout.TileMode;
    clear.Dwords[22]=1U;clear.Dwords[23]=allocated.Bytes/4U;
    clear.Dwords[24]=0U;
    clear.Dwords[25]=Nvx3dNvc0ImmediateHeader(0x15d0U,0U);
    clear.Dwords[26]=Nvx3dNvc0NonIncHeader(0x19d0U,1U);
    clear.Dwords[27]=0x3cU;
    wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
    wait.TimeoutMs=10000;
    download.Size=sizeof(download);download.Version=NVX_3D_ABI_VERSION;
    download.BufferId=allocated.BufferId;download.Bytes=sizeof(uint32_t);

    LogMsg("-> Testing native NVC0 clear on %ux%u block-linear target (%lu bytes), raw BO samples\n",
           (unsigned)width,(unsigned)height,(unsigned long)layout.Bytes);
    wait.FenceValue=0;
    for(pass=0;pass<sizeof(clearCases)/sizeof(clearCases[0]);pass++) {
        unsigned point;
        uint32_t expected=clearCases[pass][4];
        uint32_t raw[3];
        clear.Dwords[1]=clearCases[pass][0];
        clear.Dwords[2]=clearCases[pass][1];
        clear.Dwords[3]=clearCases[pass][2];
        clear.Dwords[4]=clearCases[pass][3];
        memset(&submitted,0,sizeof(submitted));
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR,sizeof(clear),
                      (LPCSTR)&clear,sizeof(submitted),(LPSTR)&submitted)||
           !submitted.FenceValue||submitted.FenceValue<=wait.FenceValue) {
            LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] FAIL submit case=%u fence=%llu after=%llu\n",
                   pass,(unsigned long long)submitted.FenceValue,
                   (unsigned long long)wait.FenceValue);
            goto done;
        }
        wait.FenceValue=submitted.FenceValue;
        memset(&waited,0,sizeof(waited));
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                      sizeof(waited),(LPSTR)&waited)||
           waited.FenceValue!=submitted.FenceValue||
           waited.Status!=NVX_3D_STATUS_SUCCESS) {
            LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] FAIL fence case=%u value=%llu\n",
                   pass,(unsigned long long)submitted.FenceValue);
            goto done;
        }
        for(point=0;point<3;point++) {
            uint32_t offset=Nvx3dTuringColorPixelOffset(sampleX[point],
                sampleY[point],width,layout.TileMode);
            download.Offset=offset;
            if(offset>allocated.Bytes-sizeof(uint32_t)||
               !ExtEscape(hdc,NVX_ESCAPE_3D_DOWNLOAD,sizeof(download),
                          (LPCSTR)&download,sizeof(uint32_t),(LPSTR)&raw[point])) {
                LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] FAIL raw-download case=%u at %u,%u offset=%lu\n",
                       pass,(unsigned)sampleX[point],(unsigned)sampleY[point],
                       (unsigned long)offset);
                goto done;
            }
            if(raw[point]!=expected) {
                LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] FAIL raw-pixel case=%u at %u,%u offset=%lu got=%08lX expected=%08lX\n",
                       pass,(unsigned)sampleX[point],(unsigned)sampleY[point],
                       (unsigned long)offset,(unsigned long)raw[point],
                       (unsigned long)expected);
                goto done;
            }
        }
        LogMsg("-> Full-size native clear case %u: origin/center/far-corner match 0x%08lX; fence=%llu\n",
               pass,(unsigned long)expected,(unsigned long long)submitted.FenceValue);
    }
    ok=1;
    LogMsg("[NVX_3D_STAGE] native-fullsize-extent PASS size=%ux%u cases=%u samples_per_case=3 fence=%llu\n",
           (unsigned)width,(unsigned)height,
           (unsigned)(sizeof(clearCases)/sizeof(clearCases[0])),
           (unsigned long long)submitted.FenceValue);

done:
    if(allocatedOk) {
        freeRequest.Size=sizeof(freeRequest);
        freeRequest.Version=NVX_3D_ABI_VERSION;
        freeRequest.BufferId=allocated.BufferId;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),
                      (LPCSTR)&freeRequest,0,NULL))
            ok=0;
    }
    LogMsg("[NVX_3D_FULLSIZE_NATIVE_CLEAR_RESULT] %s\n",ok?"PASS":"FAIL");
    return ok;
}

static int TestNvxBufferRoundTrip(HDC hdc,uint32_t usage,const char *name)
{
    /* The larger GART case crosses pages; retain the established VRAM case. */
    const unsigned words=(usage==NVX_3D_USAGE_GART_BUFFER)?8192U:256U;
    const unsigned offset=(usage==NVX_3D_USAGE_GART_BUFFER)?4096U:256U;
    struct {
        NVX_3D_UPLOAD_BUFFER header;
        uint32_t data[8192];
    } upload;
    NVX_3D_ALLOC_BUFFER alloc={0},allocated={0};
    NVX_3D_DOWNLOAD_BUFFER download={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    uint32_t downloaded[8192];
    unsigned i,done,totalBytes,chunkBytes;
    int ok=0,allocatedOk=0;

    LogMsg("-> Testing NVX %s BO upload/download staging round trip\n",name);
    alloc.Size=sizeof(alloc);alloc.Version=NVX_3D_ABI_VERSION;
    alloc.Usage=usage;
    alloc.Bytes=(usage==NVX_3D_USAGE_GART_BUFFER) ?
        offset+words*(uint32_t)sizeof(upload.data[0]) : 4096U;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(alloc),(LPCSTR)&alloc,
                  sizeof(allocated),(LPSTR)&allocated))
        goto done;
    allocatedOk=1;

    memset(&upload,0,sizeof(upload));
    upload.header.Size=sizeof(upload.header);
    upload.header.Version=NVX_3D_ABI_VERSION;
    upload.header.BufferId=allocated.BufferId;
    upload.header.Offset=offset;
    upload.header.Bytes=words*(uint32_t)sizeof(upload.data[0]);
    for(i=0;i<words;i++)
        upload.data[i]=0x6d3a5000U^(i*0x10203U);
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,
                  sizeof(upload.header)+upload.header.Bytes,
                  (LPCSTR)&upload,0,NULL))
        goto done;

    download.Size=sizeof(download);
    download.Version=NVX_3D_ABI_VERSION;
    download.BufferId=allocated.BufferId;
    totalBytes=words*sizeof(downloaded[0]);
    memset(downloaded,0,totalBytes);
    for(done=0;done<totalBytes;) {
        chunkBytes=totalBytes-done;
        if(chunkBytes>NVX_3D_MAX_DOWNLOAD_BYTES)
            chunkBytes=NVX_3D_MAX_DOWNLOAD_BYTES;
        download.Bytes=chunkBytes;
        download.Offset=offset+done;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_DOWNLOAD,sizeof(download),
                      (LPCSTR)&download,download.Bytes,
                      (LPSTR)((UCHAR *)downloaded+done)))
            goto done;
        done+=chunkBytes;
    }
    ok=memcmp(upload.data,downloaded,words*sizeof(downloaded[0]))==0;

done:
    if(allocatedOk) {
        freeRequest.Size=sizeof(freeRequest);
        freeRequest.Version=NVX_3D_ABI_VERSION;
        freeRequest.BufferId=allocated.BufferId;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),
                      (LPCSTR)&freeRequest,0,NULL))
            ok=0;
    }
    if(ok)
        LogMsg("[NVX_3D_STAGE] %s-upload-download-roundtrip PASS bytes=%u offset=%u\n",
               name,words*(unsigned)sizeof(uint32_t),offset);
    LogMsg("[NVX_%s_BUFFER_ROUNDTRIP_RESULT] %s words=%u bytes=%u\n",
           name,ok?"PASS":"FAIL",words,
           words*(unsigned)sizeof(uint32_t));
    return ok;
}

static int TestNvxGartMaximumUpload(HDC hdc)
{
    struct {
        NVX_3D_UPLOAD_BUFFER header;
        UCHAR data[NVX_3D_MAX_UPLOAD_BYTES];
    } upload;
    NVX_3D_ALLOC_BUFFER alloc={0},allocated={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    int ok=0,allocatedOk=0;

    LogMsg("-> Testing maximum-size GART upload into a fresh 512 KiB BO\n");
    alloc.Size=sizeof(alloc);alloc.Version=NVX_3D_ABI_VERSION;
    alloc.Usage=NVX_3D_USAGE_GART_BUFFER;alloc.Bytes=0x80000U;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(alloc),(LPCSTR)&alloc,
                  sizeof(allocated),(LPSTR)&allocated))
        goto done;
    allocatedOk=1;
    memset(&upload,0,sizeof(upload));
    upload.header.Size=sizeof(upload.header);
    upload.header.Version=NVX_3D_ABI_VERSION;
    upload.header.BufferId=allocated.BufferId;
    upload.header.Bytes=sizeof(upload.data);
    memset(upload.data,0x5a,sizeof(upload.data));
    LogMsg("[NVX_GART_MAX_UPLOAD] begin va=%I64X bytes=%lu\n",
           (unsigned __int64)allocated.GpuVa,(unsigned long)upload.header.Bytes);
    ok=ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,
                 sizeof(upload.header)+upload.header.Bytes,
                 (LPCSTR)&upload,0,NULL)!=0;

done:
    if(allocatedOk) {
        freeRequest.Size=sizeof(freeRequest);
        freeRequest.Version=NVX_3D_ABI_VERSION;
        freeRequest.BufferId=allocated.BufferId;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),
                      (LPCSTR)&freeRequest,0,NULL))
            ok=0;
    }
    LogMsg("[NVX_GART_MAX_UPLOAD_RESULT] %s\n",ok?"PASS":"FAIL");
    return ok;
}

static void TestNvx3dSubmission(HDC hdc)
{
    NVX_3D_QUERY_CAPS caps={0};
    NVX_3D_QUERY_CAPS verifiedCaps={0};
    NVX_3D_ALLOC_BUFFER alloc={0},allocated={0};
    NVX_3D_ALLOC_BUFFER vertexAlloc={0},vertexAllocated={0};
    NVX_3D_ALLOC_BUFFER surfaceAlloc={0},surfaceAllocated={0};
    NVX_3D_ALLOC_BUFFER surfaceVertexAlloc={0},surfaceVertexAllocated={0};
    struct { NVX_3D_UPLOAD_BUFFER header; UCHAR data[256]; } upload;
    struct { NVX_3D_UPLOAD_BUFFER header; float data[18]; } vertexUpload;
    struct { NVX_3D_UPLOAD_BUFFER header; float data[36]; } batchUpload;
    struct { NVX_3D_UPLOAD_BUFFER header; float data[36]; } surfaceUpload;
    NVX_3D_SUBMIT submit={0},submitted={0};
    NVX_3D_SUBMIT surfaceSubmit={0},surfaceSubmitted={0};
    NVX_3D_SUBMIT surfaceDraw={0},surfaceDrawn={0};
    NVX_3D_READBACK readbackRequest={0};
    uint32_t surfacePixels[NVX_3D_READBACK_PIXELS];
    NVX_3D_WAIT_FENCE wait={0},waited={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
    NVX_3D_FREE_BUFFER surfaceFree={0};
    int ok=1,clearSubmitted=0,vertexReady=0;
    LogMsg("\n==================================================\n");
    LogMsg(" [TEST 3D] NVX bounded GR allocation/submit/fence ABI\n");
    LogMsg("==================================================\n");
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_QUERY_CAPS,0,NULL,sizeof(caps),
                  (LPSTR)&caps)) {
        LogMsg("-> [INFO] 3D caps escape unavailable\n");
#if defined(NVX_REQUIRE_3D_ABI) && NVX_REQUIRE_3D_ABI
        LogMsg("[NVX_3D_RESULT] FAIL\n");failures++;
#else
        LogMsg("[NVX_3D_RESULT] UNAVAILABLE\n");
#endif
        return;
    }
    {
        uint32_t slot=(caps.PciLocation>>NVX_3D_PCI_LOCATION_SLOT_SHIFT)&0xffU;
        LogMsg("-> NVX 3D features=0x%08lX class=0x%08lX ABI=%lu PCI=%04lX BDF=%02lX:%02lX.%lu chipset=0x%03lX VRAM-aperture=%llu GART=%llu\n",
        (unsigned long)caps.Features,(unsigned long)caps.GraphicsClass,
        (unsigned long)caps.Version,(unsigned long)caps.DeviceId,
        (unsigned long)(caps.PciLocation&NVX_3D_PCI_LOCATION_BUS_MASK),
        (unsigned long)(slot&0x1fU),(unsigned long)((slot>>5)&0x7U),
        (unsigned long)caps.Chipset,
        (unsigned long long)caps.VramApertureBytes,
        (unsigned long long)caps.GartBytes);
    }
    if (mesaGpuBridgeProbe) {
        int bridged = (caps.Features & NVX_3D_FEATURE_CLIENT_TRIANGLE) != 0 &&
                      mesaGpuFramebufferVerified;
        LogMsg("[MESA_TO_GPU_RESULT] %s (Mesa pixels %s copied from the fenced GPU target)\n",
               bridged ? "PASS" : "FAIL",
               bridged ? "were" : "were not");
        if (!bridged) failures++;
    }
#if defined(NVX_REQUIRE_3D_ABI) && NVX_REQUIRE_3D_ABI
    if(!(caps.Features&NVX_3D_FEATURE_TEST_TRIANGLE)) {
        LogMsg("-> [INFO] triangle capability is not advertised; running fixed diagnostic operation without promotion\n");
    }
#endif
    if(!(caps.Features&NVX_3D_FEATURE_USER_ALLOC)||
       !(caps.Features&NVX_3D_FEATURE_UPLOAD)||
       !(caps.PciLocation&NVX_3D_PCI_LOCATION_VALID)||
       caps.Version!=NVX_3D_ABI_VERSION) {
        LogMsg("-> [INFO] safe 3D allocation/submit capabilities unavailable\n");
#if defined(NVX_REQUIRE_3D_ABI) && NVX_REQUIRE_3D_ABI
        LogMsg("[NVX_3D_RESULT] FAIL\n");failures++;
#else
        LogMsg("[NVX_3D_RESULT] UNAVAILABLE\n");
#endif
        return;
    }
    if(!TestNvc0ClearEscape(hdc)) ok=0;
    alloc.Size=sizeof(alloc);alloc.Version=NVX_3D_ABI_VERSION;
    alloc.Usage=1;alloc.Bytes=65536;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(alloc),(LPCSTR)&alloc,
                  sizeof(allocated),(LPSTR)&allocated)) {
        LogMsg("-> [FAIL] 3D buffer allocation escape failed\n");
        failures++;return;
    }
    LogMsg("-> Allocated buffer id=%llu gpuVa=0x%llX bytes=%lu\n",
        (unsigned long long)allocated.BufferId,
        (unsigned long long)allocated.GpuVa,(unsigned long)allocated.Bytes);
    memset(&upload,0,sizeof(upload));
    upload.header.Size=sizeof(upload.header);
    upload.header.Version=NVX_3D_ABI_VERSION;
    upload.header.BufferId=allocated.BufferId;
    upload.header.Offset=0x1000;
    upload.header.Bytes=sizeof(upload.data);
    for(unsigned i=0;i<sizeof(upload.data);i++)
        upload.data[i]=(UCHAR)(i*37U+11U);
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,sizeof(upload),(LPCSTR)&upload,0,NULL)) {
        LogMsg("-> [FAIL] 3D client-to-VRAM upload failed\n");
        ok=0;
    } else {
        LogMsg("-> Uploaded %u bytes to allocation offset 0x%X; CE readback verification is logged by the miniport\n",
            (unsigned)sizeof(upload.data),(unsigned)upload.header.Offset);
    }
    vertexAlloc.Size=sizeof(vertexAlloc);vertexAlloc.Version=NVX_3D_ABI_VERSION;
    vertexAlloc.Usage=1;vertexAlloc.Bytes=4096;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(vertexAlloc),
                  (LPCSTR)&vertexAlloc,sizeof(vertexAllocated),
                  (LPSTR)&vertexAllocated)) {
        LogMsg("-> [FAIL] vertex-buffer allocation failed\n");ok=0;
    } else {
        memset(&vertexUpload,0,sizeof(vertexUpload));
        vertexUpload.header.Size=sizeof(vertexUpload.header);
        vertexUpload.header.Version=NVX_3D_ABI_VERSION;
        vertexUpload.header.BufferId=vertexAllocated.BufferId;
        vertexUpload.header.Bytes=sizeof(vertexUpload.data);
        {
            static const float triangle[18]={
                0.125f,0.125f,0.0f,0.0f,0.0f,1.0f,
                0.125f,0.875f,0.0f,1.0f,0.0f,1.0f,
                0.875f,0.125f,1.0f,0.0f,0.0f,1.0f
            };
            memcpy(vertexUpload.data,triangle,sizeof(triangle));
        }
        vertexReady=ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,sizeof(vertexUpload),
                              (LPCSTR)&vertexUpload,0,NULL)!=0;
        if(vertexReady)
            LogMsg("-> Uploaded 3 client vertices (%u bytes) to GPU memory\n",
                   (unsigned)sizeof(vertexUpload.data));
        else {LogMsg("-> [FAIL] client vertex upload failed\n");ok=0;}
    }
    submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
    submit.Operation=NVX_3D_SUBMIT_CLEAR_SURFACE;
    submit.Width=64;submit.Height=16;submit.BufferId=allocated.BufferId;
    submit.Color=0xffffffffU;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(submit),(LPCSTR)&submit,
                  sizeof(submitted),(LPSTR)&submitted)) {
        LogMsg("-> [FAIL] bounded C797 clear submission failed\n");ok=0;
    } else {
        LogMsg("-> Submitted white clear; fence=%llu matching_pixels=%lu\n",
            (unsigned long long)submitted.FenceValue,
            (unsigned long)submitted.Reserved[0]);
        if(submitted.Reserved[0]!=1024) {
            LogMsg("-> [FAIL] white clear readback count was not 1024\n");ok=0;
        }
        LogMsg("-> White clear fence=%llu\n",
            (unsigned long long)submitted.FenceValue);
        wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
        wait.FenceValue=submitted.FenceValue;wait.TimeoutMs=10000;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                      sizeof(waited),(LPSTR)&waited)) {
            LogMsg("-> [FAIL] 3D completion fence wait failed\n");ok=0;
        } else {LogMsg("-> 3D completion fence signaled\n");clearSubmitted=1;}
    }
    if(clearSubmitted && (caps.Features&NVX_3D_FEATURE_TEST_TRIANGLE)) {
        memset(&submit,0,sizeof(submit));
        memset(&submitted,0,sizeof(submitted));
        submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
        submit.Operation=NVX_3D_SUBMIT_TEST_TRIANGLE;
        submit.Width=64;submit.Height=16;submit.BufferId=allocated.BufferId;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(submit),(LPCSTR)&submit,
                      sizeof(submitted),(LPSTR)&submitted)) {
            LogMsg("-> [FAIL] bounded C797 triangle submission/readback failed (winerr=%lu)\n",
                (unsigned long)GetLastError());
            ok=0;
        } else {
            LogMsg("-> Submitted fixed triangle; fence=%llu green_pixels=%lu white_pixels=%lu\n",
                (unsigned long long)submitted.FenceValue,
                (unsigned long)submitted.Reserved[0],
                (unsigned long)submitted.Reserved[1]);
            if(submitted.Reserved[0]<24||submitted.Reserved[1]==0||
               submitted.Reserved[0]+submitted.Reserved[1]!=1024) {
                LogMsg("-> [FAIL] hardware triangle readback counts invalid\n");
                ok=0;
            }
            memset(&wait,0,sizeof(wait));memset(&waited,0,sizeof(waited));
            wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
            wait.FenceValue=submitted.FenceValue;wait.TimeoutMs=10000;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                          sizeof(waited),(LPSTR)&waited)) {
                LogMsg("-> [FAIL] triangle fence wait failed\n");ok=0;
            } else LogMsg("-> triangle completion fence signaled\n");
        }
    } else if(clearSubmitted) {
        LogMsg("-> Skipping fixed triangle operation; the miniport did not advertise NVX_3D_FEATURE_TEST_TRIANGLE\n");
    }
    if(vertexReady && (caps.Features&NVX_3D_FEATURE_CLIENT_TRIANGLE)) {
        NVX_3D_ALLOC_BUFFER depthAlloc={0},depthAllocated={0};
        NVX_3D_SUBMIT depthSubmit={0},depthSubmitted={0};
        NVX_3D_FREE_BUFFER depthFree={0};
        depthAlloc.Size=sizeof(depthAlloc);
        depthAlloc.Version=NVX_3D_ABI_VERSION;
        depthAlloc.Usage=NVX_3D_USAGE_DEPTH_TARGET|NVX_3D_USAGE_DEPTH_ZF32;
        depthAlloc.Bytes=NVX_3D_DEPTH_TARGET_TOTAL_BYTES;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(depthAlloc),
                     (LPCSTR)&depthAlloc,sizeof(depthAllocated),
                     (LPSTR)&depthAllocated)) {
            LogMsg("-> [FAIL] ZF32 depth-target allocation failed\n");ok=0;
        } else {
            LogMsg("-> Allocated ZF32 depth diagnostic buffer id=%llu\n",
                   (unsigned long long)depthAllocated.BufferId);
            depthSubmit.Size=sizeof(depthSubmit);
            depthSubmit.Version=NVX_3D_ABI_VERSION;
            depthSubmit.Operation=NVX_3D_SUBMIT_CLEAR_SURFACE;
            depthSubmit.Width=64;depthSubmit.Height=16;
            depthSubmit.BufferId=depthAllocated.BufferId;
            depthSubmit.Color=0xffffffffU;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(depthSubmit),
                    (LPCSTR)&depthSubmit,sizeof(depthSubmitted),
                    (LPSTR)&depthSubmitted)) {
                LogMsg("-> [FAIL] ZF32 test color-target clear failed (winerr=%lu)\n",
                       (unsigned long)GetLastError());
                ok=0;
            }
            memset(&depthSubmit,0,sizeof(depthSubmit));
            memset(&depthSubmitted,0,sizeof(depthSubmitted));
            depthSubmit.Size=sizeof(depthSubmit);
            depthSubmit.Version=NVX_3D_ABI_VERSION;
            depthSubmit.Operation=NVX_3D_SUBMIT_CLEAR_DEPTH;
            depthSubmit.Width=64;depthSubmit.Height=16;
            depthSubmit.BufferId=depthAllocated.BufferId;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(depthSubmit),
                    (LPCSTR)&depthSubmit,sizeof(depthSubmitted),
                    (LPSTR)&depthSubmitted)) {
                LogMsg("-> [FAIL] ZF32 depth clear failed (winerr=%lu)\n",
                       (unsigned long)GetLastError());
                ok=0;
            }
            {
                struct { NVX_3D_UPLOAD_BUFFER header; float data[24]; } depthVertexUpload;
                static const float depthTriangle[24]={
                    0.125f,0.125f,0.5f,1.0f, 0.5f,1.0f,0.0f,1.0f,
                    0.125f,0.875f,0.5f,1.0f, 0.5f,1.0f,0.0f,1.0f,
                    0.875f,0.125f,0.5f,1.0f, 0.5f,1.0f,0.0f,1.0f
                };
                memset(&depthVertexUpload,0,sizeof(depthVertexUpload));
                depthVertexUpload.header.Size=sizeof(depthVertexUpload.header);
                depthVertexUpload.header.Version=NVX_3D_ABI_VERSION;
                depthVertexUpload.header.BufferId=vertexAllocated.BufferId;
                depthVertexUpload.header.Bytes=sizeof(depthVertexUpload.data);
                memcpy(depthVertexUpload.data,depthTriangle,sizeof(depthTriangle));
                if(!ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,sizeof(depthVertexUpload),
                              (LPCSTR)&depthVertexUpload,0,NULL)) {
                    LogMsg("-> [FAIL] depth vertex buffer upload failed\n");
                    ok=0;
                }
            }
            memset(&depthSubmit,0,sizeof(depthSubmit));
            memset(&depthSubmitted,0,sizeof(depthSubmitted));
            depthSubmit.Size=sizeof(depthSubmit);
            depthSubmit.Version=NVX_3D_ABI_VERSION;
            depthSubmit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLE_DEPTH;
            depthSubmit.Width=64;depthSubmit.Height=16;
            depthSubmit.BufferId=depthAllocated.BufferId;
            depthSubmit.Color=0xff00ff00U;
            depthSubmit.Reserved[0]=(uint32_t)vertexAllocated.BufferId;
            depthSubmit.Reserved[1]=(uint32_t)(vertexAllocated.BufferId>>32);
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(depthSubmit),
                    (LPCSTR)&depthSubmit,sizeof(depthSubmitted),
                    (LPSTR)&depthSubmitted)) {
                LogMsg("-> [FAIL] ZF32 depth-tested known triangle failed (winerr=%lu)\n",
                       (unsigned long)GetLastError());
                ok=0;
            } else {
                LogMsg("-> ZF32 depth-tested known triangle fence=%llu draw_pixels=%lu background=%lu\n",
                       (unsigned long long)depthSubmitted.FenceValue,
                       (unsigned long)depthSubmitted.Reserved[0],
                       (unsigned long)depthSubmitted.Reserved[1]);
            }
            depthFree.Size=sizeof(depthFree);
            depthFree.Version=NVX_3D_ABI_VERSION;
            depthFree.BufferId=depthAllocated.BufferId;
            ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(depthFree),
                      (LPCSTR)&depthFree,0,NULL);
        }
    }
    if(clearSubmitted&&vertexReady&&
       (caps.Features&NVX_3D_FEATURE_CLIENT_TRIANGLE)) {
        memset(&submit,0,sizeof(submit));memset(&submitted,0,sizeof(submitted));
        submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
        submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLE;
        submit.Width=64;submit.Height=16;submit.BufferId=allocated.BufferId;
        submit.Color=0xff00ff00U;
        submit.Reserved[0]=(uint32_t)vertexAllocated.BufferId;
        submit.Reserved[1]=(uint32_t)(vertexAllocated.BufferId>>32);
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(submit),(LPCSTR)&submit,
                      sizeof(submitted),(LPSTR)&submitted)) {
            LogMsg("-> [FAIL] client-vertex GR draw/readback failed (winerr=%lu)\n",
                   (unsigned long)GetLastError());ok=0;
        } else {
            LogMsg("-> Client vertices rasterized by C797; fence=%llu draw_pixels=%lu white_pixels=%lu\n",
                (unsigned long long)submitted.FenceValue,
                (unsigned long)submitted.Reserved[0],
                (unsigned long)submitted.Reserved[1]);
            if(submitted.Reserved[0]<24||submitted.Reserved[1]==0||
               submitted.Reserved[0]+submitted.Reserved[1]!=1024) {
                LogMsg("-> [FAIL] client-vertex draw readback partition invalid\n");ok=0;
            }
            memset(&wait,0,sizeof(wait));memset(&waited,0,sizeof(waited));
            wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
            wait.FenceValue=submitted.FenceValue;wait.TimeoutMs=10000;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                          sizeof(waited),(LPSTR)&waited)) {
                LogMsg("-> [FAIL] client-vertex draw fence wait failed\n");ok=0;
            }
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_QUERY_CAPS,0,NULL,
                          sizeof(verifiedCaps),(LPSTR)&verifiedCaps)||
               !(verifiedCaps.Features&NVX_3D_FEATURE_CLIENT_TRIANGLE)) {
                LogMsg("-> [FAIL] miniport did not advertise verified client-triangle support\n");
                ok=0;
            } else {
                LogMsg("-> Miniport advertises verified client-triangle submission\n");
            }
        }
    }
    if(clearSubmitted&&vertexReady&&
       (caps.Features&NVX_3D_FEATURE_CLIENT_TRIANGLE)) {
        uint32_t pixels[NVX_3D_READBACK_PIXELS];
        unsigned greenPixels=0;
        static const float triangles[36]={
            0.05f,0.05f,0.0f,0.0f,0.0f,1.0f,
            0.45f,0.05f,0.0f,0.0f,0.0f,1.0f,
            0.25f,0.45f,0.0f,0.0f,0.0f,1.0f,
            0.55f,0.55f,0.0f,0.0f,0.0f,1.0f,
            0.95f,0.55f,0.0f,0.0f,0.0f,1.0f,
            0.75f,0.95f,0.0f,0.0f,0.0f,1.0f
        };
        memset(&batchUpload,0,sizeof(batchUpload));
        batchUpload.header.Size=sizeof(batchUpload.header);
        batchUpload.header.Version=NVX_3D_ABI_VERSION;
        batchUpload.header.BufferId=vertexAllocated.BufferId;
        batchUpload.header.Bytes=sizeof(batchUpload.data);
        memcpy(batchUpload.data,triangles,sizeof(triangles));
        memset(&submit,0,sizeof(submit));memset(&submitted,0,sizeof(submitted));
        submit.Size=sizeof(submit);submit.Version=NVX_3D_ABI_VERSION;
        submit.Operation=NVX_3D_SUBMIT_DRAW_TRIANGLES;
        submit.Width=64;submit.Height=6;submit.BufferId=allocated.BufferId;
        submit.Color=0xff00ff00U;
        submit.Reserved[0]=(uint32_t)vertexAllocated.BufferId;
        submit.Reserved[1]=(uint32_t)(vertexAllocated.BufferId>>32);
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,sizeof(batchUpload),
                      (LPCSTR)&batchUpload,0,NULL) ||
           !ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(submit),(LPCSTR)&submit,
                      sizeof(submitted),(LPSTR)&submitted)) {
            LogMsg("-> [FAIL] two-triangle batched C797 submission failed\n");
            ok=0;
        } else {
            memset(&wait,0,sizeof(wait));memset(&waited,0,sizeof(waited));
            wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
            wait.FenceValue=submitted.FenceValue;wait.TimeoutMs=10000;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),(LPCSTR)&wait,
                          sizeof(waited),(LPSTR)&waited)) {
                LogMsg("-> [FAIL] batched triangle fence wait failed\n");
                ok=0;
            } else {
                readbackRequest.Size=sizeof(readbackRequest);
                readbackRequest.Version=NVX_3D_ABI_VERSION;
                readbackRequest.BufferId=allocated.BufferId;
                readbackRequest.Width=64;readbackRequest.Height=16;
                if(!ExtEscape(hdc,NVX_ESCAPE_3D_READBACK,
                    sizeof(readbackRequest),(LPCSTR)&readbackRequest,
                    sizeof(pixels),(LPSTR)pixels)) {
                    LogMsg("-> [FAIL] batched triangle readback failed\n");
                    ok=0;
                } else {
                    for(unsigned p=0;p<NVX_3D_READBACK_PIXELS;p++)
                        if((pixels[p]&0x00ffffffU)==0x0000ff00U)
                            greenPixels++;
                    LogMsg("[NVX_3D_BATCH_RESULT] %s vertices=6 green_pixels=%u fence=%llu\n",
                           greenPixels>=24?"PASS":"FAIL",greenPixels,
                           (unsigned long long)submitted.FenceValue);
                    if(greenPixels<24) ok=0;
                }
            }
        }
    }
    {
        NVX_3D_COLOR_SURFACE_LAYOUT layout={0};
        if(!Nvx3dComputeColorSurfaceLayout(632,453,&layout)) {
            LogMsg("[NVX_3D_SURFACE_RESULT] FAIL layout computation\n");
            ok=0;
        } else {
            surfaceAlloc.Size=sizeof(surfaceAlloc);
            surfaceAlloc.Version=NVX_3D_ABI_VERSION;
            surfaceAlloc.Usage=NVX_3D_USAGE_COLOR_TARGET;
            surfaceAlloc.Bytes=layout.Bytes;
            surfaceAlloc.Width=layout.Width;
            surfaceAlloc.Height=layout.Height;
            surfaceAlloc.PitchBytes=layout.PitchBytes;
            surfaceAlloc.TileMode=layout.TileMode;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,sizeof(surfaceAlloc),
                          (LPCSTR)&surfaceAlloc,sizeof(surfaceAllocated),
                          (LPSTR)&surfaceAllocated)) {
                LogMsg("[NVX_3D_SURFACE_RESULT] FAIL color-target allocation\n");
                ok=0;
            } else {
                surfaceSubmit.Size=sizeof(surfaceSubmit);
                surfaceSubmit.Version=NVX_3D_ABI_VERSION;
                surfaceSubmit.Operation=NVX_3D_SUBMIT_CLEAR_COLOR_TARGET;
                surfaceSubmit.Width=layout.Width;
                surfaceSubmit.Height=layout.Height;
                surfaceSubmit.BufferId=surfaceAllocated.BufferId;
                surfaceSubmit.Color=0xff204080U;
                if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(surfaceSubmit),
                              (LPCSTR)&surfaceSubmit,sizeof(surfaceSubmitted),
                              (LPSTR)&surfaceSubmitted)) {
                    LogMsg("[NVX_3D_SURFACE_RESULT] FAIL C797 full-surface clear\n");
                    ok=0;
                } else {
                    memset(&wait,0,sizeof(wait));memset(&waited,0,sizeof(waited));
                    wait.Size=sizeof(wait);wait.Version=NVX_3D_ABI_VERSION;
                    wait.FenceValue=surfaceSubmitted.FenceValue;
                    wait.TimeoutMs=10000;
                    if(!ExtEscape(hdc,NVX_ESCAPE_3D_WAIT,sizeof(wait),
                                  (LPCSTR)&wait,sizeof(waited),(LPSTR)&waited)) {
                        LogMsg("[NVX_3D_SURFACE_RESULT] FAIL clear fence wait\n");
                        ok=0;
                    } else {
                        memset(&readbackRequest,0,sizeof(readbackRequest));
                        readbackRequest.Size=sizeof(readbackRequest);
                        readbackRequest.Version=NVX_3D_ABI_VERSION;
                        readbackRequest.BufferId=surfaceAllocated.BufferId;
                        readbackRequest.Width=64;
                        readbackRequest.Height=16;
                        if(!ExtEscape(hdc,NVX_ESCAPE_3D_READBACK,
                                sizeof(readbackRequest),
                                (LPCSTR)&readbackRequest,sizeof(surfacePixels),
                                (LPSTR)surfacePixels) ||
                           (surfacePixels[0]&0x00ffffffU)!=0x00204080U) {
                            LogMsg("[NVX_3D_SURFACE_RESULT] FAIL clear readback origin=%08lX\n",
                                (unsigned long)surfacePixels[0]);
                            ok=0;
                        } else {
                            readbackRequest.Reserved[0]=576;
                            readbackRequest.Reserved[1]=448;
                            if(!ExtEscape(hdc,NVX_ESCAPE_3D_READBACK,
                                    sizeof(readbackRequest),
                                    (LPCSTR)&readbackRequest,
                                    sizeof(surfacePixels),(LPSTR)surfacePixels) ||
                               (surfacePixels[4*64+55]&0x00ffffffU)!=0x00204080U) {
                                LogMsg("[NVX_3D_SURFACE_RESULT] FAIL clear edge readback=%08lX\n",
                                    (unsigned long)surfacePixels[4*64+55]);
                                ok=0;
                            } else {
                        LogMsg("[NVX_3D_SURFACE_RESULT] PASS %lux%lu pitch=%lu tileMode=0x%X bytes=%lu samples=%lu fence=%llu\n",
                            (unsigned long)layout.Width,
                            (unsigned long)layout.Height,
                            (unsigned long)layout.PitchBytes,
                            (unsigned)layout.TileMode,
                            (unsigned long)layout.Bytes,
                            2UL,
                            (unsigned long long)surfaceSubmitted.FenceValue);
                            }
                        }
                    }
                }
                if(!(caps.Features&NVX_3D_FEATURE_CLIENT_TRIANGLE)) {
                    LogMsg("[NVX_3D_SURFACE_DRAW_RESULT] SKIP client-triangle capability not advertised\n");
                } else {
                surfaceVertexAlloc.Size=sizeof(surfaceVertexAlloc);
                surfaceVertexAlloc.Version=NVX_3D_ABI_VERSION;
                surfaceVertexAlloc.Usage=1;
                surfaceVertexAlloc.Bytes=4096;
                if(!ExtEscape(hdc,NVX_ESCAPE_3D_ALLOC,
                              sizeof(surfaceVertexAlloc),
                              (LPCSTR)&surfaceVertexAlloc,
                              sizeof(surfaceVertexAllocated),
                              (LPSTR)&surfaceVertexAllocated)) {
                    LogMsg("[NVX_3D_SURFACE_DRAW_RESULT] FAIL vertex allocation\n");
                    ok=0;
                } else {
                    static const float quad[36]={
                        -1,-1,0.5f,0,0,1, 1,-1,0.5f,1,0,1,
                         1, 1,0.5f,1,1,1, -1,-1,0.5f,0,0,1,
                         1, 1,0.5f,1,1,1, -1, 1,0.5f,0,1,1
                    };
                    memset(&surfaceUpload,0,sizeof(surfaceUpload));
                    surfaceUpload.header.Size=sizeof(surfaceUpload.header);
                    surfaceUpload.header.Version=NVX_3D_ABI_VERSION;
                    surfaceUpload.header.BufferId=surfaceVertexAllocated.BufferId;
                    surfaceUpload.header.Bytes=sizeof(quad);
                    memcpy(surfaceUpload.data,quad,sizeof(quad));
                    if(!ExtEscape(hdc,NVX_ESCAPE_3D_UPLOAD,
                            sizeof(surfaceUpload.header)+sizeof(quad),
                            (LPCSTR)&surfaceUpload,0,NULL)) {
                        LogMsg("[NVX_3D_SURFACE_DRAW_RESULT] FAIL vertex upload\n");
                        ok=0;
                    } else {
                        surfaceDraw.Size=sizeof(surfaceDraw);
                        surfaceDraw.Version=NVX_3D_ABI_VERSION;
                        surfaceDraw.Operation=NVX_3D_SUBMIT_VERIFY_COLOR_TARGET_DRAW;
                        surfaceDraw.Width=layout.Width;
                        surfaceDraw.Height=layout.Height;
                        surfaceDraw.Color=0xff20d040U;
                        surfaceDraw.BufferId=surfaceAllocated.BufferId;
                        surfaceDraw.Reserved[0]=6;
                        surfaceDraw.Reserved[1]=(uint32_t)surfaceVertexAllocated.BufferId;
                        surfaceDraw.Reserved[2]=(uint32_t)(surfaceVertexAllocated.BufferId>>32);
                        surfaceDraw.Reserved[3]=layout.TileMode;
                        if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,
                                sizeof(surfaceDraw),(LPCSTR)&surfaceDraw,
                                sizeof(surfaceDrawn),(LPSTR)&surfaceDrawn) ||
                           !surfaceDrawn.FenceValue ||
                           !WaitNvx3dFence(hdc,surfaceDrawn.FenceValue)) {
                            LogMsg("[NVX_3D_SURFACE_DRAW_RESULT] FAIL full-target C797 draw/fence\n");
                            ok=0;
                        } else {
                            LogMsg("[NVX_3D_SURFACE_DRAW_RESULT] PASS %lux%lu vertices=6 pixel_samples=5 fence=%llu\n",
                                (unsigned long)layout.Width,
                                (unsigned long)layout.Height,
                                (unsigned long long)surfaceDrawn.FenceValue);
                            memset(&readbackRequest,0,sizeof(readbackRequest));
                            readbackRequest.Size=sizeof(readbackRequest);
                            readbackRequest.Version=NVX_3D_ABI_VERSION;
                            readbackRequest.BufferId=surfaceAllocated.BufferId;
                            readbackRequest.Width=64;
                            readbackRequest.Height=16;
                            readbackRequest.Reserved[0]=576;
                            readbackRequest.Reserved[1]=448;
                            if(!ExtEscape(hdc,NVX_ESCAPE_3D_READBACK,
                                    sizeof(readbackRequest),
                                    (LPCSTR)&readbackRequest,sizeof(surfacePixels),
                                    (LPSTR)surfacePixels) ||
                               (surfacePixels[0]&0x00ffffffU)!=0x0020d040U ||
                               (surfacePixels[4*64+55]&0x00ffffffU)!=0x0020d040U) {
                                LogMsg("[NVX_3D_SURFACE_TILE_READBACK] FAIL bottom-right tiled readback first=%08lX last=%08lX\n",
                                    (unsigned long)surfacePixels[0],
                                    (unsigned long)surfacePixels[4*64+55]);
                                ok=0;
                            } else {
                                LogMsg("[NVX_3D_SURFACE_TILE_READBACK] PASS tile=576,448 edge_pixels=2 bottom_right=(631,452)\n");
                            }
                        }
                    }
                    {
                        NVX_3D_FREE_BUFFER vertexFree={0};
                        vertexFree.Size=sizeof(vertexFree);
                        vertexFree.Version=NVX_3D_ABI_VERSION;
                        vertexFree.BufferId=surfaceVertexAllocated.BufferId;
                        if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(vertexFree),
                                      (LPCSTR)&vertexFree,0,NULL)) {
                            LogMsg("[NVX_3D_SURFACE_DRAW_RESULT] FAIL vertex release\n");
                            ok=0;
                        }
                    }
                }
                }
                surfaceFree.Size=sizeof(surfaceFree);
                surfaceFree.Version=NVX_3D_ABI_VERSION;
                surfaceFree.BufferId=surfaceAllocated.BufferId;
                if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(surfaceFree),
                              (LPCSTR)&surfaceFree,0,NULL)) {
                    LogMsg("[NVX_3D_SURFACE_RESULT] FAIL color-target release\n");
                    ok=0;
                }
            }
        }
    }
    freeRequest.Size=sizeof(freeRequest);freeRequest.Version=NVX_3D_ABI_VERSION;
    freeRequest.BufferId=allocated.BufferId;
    if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),
                  (LPCSTR)&freeRequest,0,NULL)) {
        LogMsg("-> [FAIL] 3D buffer release failed\n");ok=0;
    }
    if(vertexAllocated.BufferId) {
        freeRequest.BufferId=vertexAllocated.BufferId;
        if(!ExtEscape(hdc,NVX_ESCAPE_3D_FREE,sizeof(freeRequest),
                      (LPCSTR)&freeRequest,0,NULL)) {
            LogMsg("-> [FAIL] vertex-buffer release failed\n");ok=0;
        }
    }
    if(ok) LogMsg("[NVX_3D_RESULT] PASS\n");
    else {LogMsg("[NVX_3D_RESULT] FAIL\n");failures++;}
}

/* ---------------- Direct3D 9 / DirectX Tests ---------------- */
static void TestDirect3D9(HWND hwnd)
{
    LogMsg("\n==================================================\n");
    LogMsg(" [TEST 3/3] Direct3D 9 Device Availability\n");
    LogMsg("==================================================\n");

    /* Dynamically load d3d9.dll */
    HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) {
        LogMsg("-> [FAIL] d3d9.dll not found (err=%lu)\n", GetLastError()); failures++;
        return;
    }
    LogMsg("-> d3d9.dll loaded successfully at %p\n", (void *)hD3D9);

    typedef IDirect3D9* (WINAPI *LPDIRECT3DCREATE9)(UINT);
    LPDIRECT3DCREATE9 pDirect3DCreate9 = (LPDIRECT3DCREATE9)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pDirect3DCreate9) {
        LogMsg("-> [FAIL] Direct3DCreate9 export not found\n"); failures++;
        FreeLibrary(hD3D9);
        return;
    }

    IDirect3D9 *pD3D = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        LogMsg("-> [FAIL] Direct3DCreate9 returned NULL\n"); failures++;
        FreeLibrary(hD3D9);
        return;
    }
    LogMsg("-> IDirect3D9 interface created!\n");

    D3DPRESENT_PARAMETERS d3dpp;
    memset(&d3dpp, 0, sizeof(d3dpp));
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = hwnd;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D16;

    D3DADAPTER_IDENTIFIER9 adapter;
    if (SUCCEEDED(pD3D->lpVtbl->GetAdapterIdentifier(pD3D, 0, 0, &adapter)))
        LogMsg("-> D3D adapter: %s driver=%s\n", adapter.Description, adapter.Driver);
    D3DDEVTYPE deviceType = D3DDEVTYPE_HAL;
    IDirect3DDevice9 *pDevice = NULL;
    HRESULT hr = pD3D->lpVtbl->CreateDevice(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                           hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                           &d3dpp, &pDevice);
    if (FAILED(hr)) {
        LogMsg("-> HAL device creation with depth stencil returned 0x%08lX, retrying without depth stencil...\n", (unsigned long)hr);
        d3dpp.EnableAutoDepthStencil = FALSE;
        hr = pD3D->lpVtbl->CreateDevice(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                       hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                       &d3dpp, &pDevice);
        if (FAILED(hr)) {
            LogMsg("-> HAL without depth returned 0x%08lX, attempting REF...\n", (unsigned long)hr);
            deviceType = D3DDEVTYPE_REF;
            hr = pD3D->lpVtbl->CreateDevice(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF,
                                           hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                           &d3dpp, &pDevice);
        }
    }

    if (SUCCEEDED(hr) && pDevice) {
        LogMsg("-> IDirect3DDevice9 created successfully! Type=%s\n", deviceType == D3DDEVTYPE_HAL ? "HAL" : "REF (software)");
        if (deviceType != D3DDEVTYPE_HAL) software3d++;

        const int D3D_FRAMES = 60;
        LogMsg("-> Rendering %d Direct3D 9 frames with Present...\n", D3D_FRAMES);

        DWORD clearFlags = d3dpp.EnableAutoDepthStencil ? (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) : D3DCLEAR_TARGET;
        double t0 = GetTimeSeconds();
        for (int i = 0; i < D3D_FRAMES; i++) {
            D3DCOLOR clr = D3DCOLOR_XRGB((i * 4) % 256, 40, (255 - i * 4) % 256);
            struct { float x,y,z,rhw; DWORD color; } triangle[3] = {
                {60,60,0.5f,1.0f,0xffff0000},
                {300,60,0.5f,1.0f,0xff00ff00},
                {180,300,0.5f,1.0f,0xff0000ff}
            };
            hr = pDevice->lpVtbl->Clear(pDevice, 0, NULL, clearFlags, clr, 1.0f, 0);
            if (SUCCEEDED(hr)) hr = pDevice->lpVtbl->BeginScene(pDevice);
            if (SUCCEEDED(hr)) {
                hr = pDevice->lpVtbl->SetFVF(pDevice, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                if (SUCCEEDED(hr)) hr = pDevice->lpVtbl->DrawPrimitiveUP(pDevice,
                    D3DPT_TRIANGLELIST, 1, triangle, sizeof(triangle[0]));
                {
                    HRESULT end = pDevice->lpVtbl->EndScene(pDevice);
                    if (SUCCEEDED(hr)) hr = end;
                }
            }
            if (SUCCEEDED(hr)) hr = pDevice->lpVtbl->Present(pDevice, NULL, NULL, NULL, NULL);
            if (FAILED(hr)) {
                LogMsg("-> [FAIL] D3D rendering/presentation returned 0x%08lX\n", (unsigned long)hr);
                failures++; break;
            }
        }
        double t1 = GetTimeSeconds();

        double dt = t1 - t0;
        LogMsg("-> Direct3D 9 rendered %d frames in %.4f s (%.1f FPS)\n", D3D_FRAMES, dt, (double)D3D_FRAMES / dt);
        pDevice->lpVtbl->Release(pDevice);
        LogMsg("-> Direct3D rendering test ended; device type and errors reported above.\n");
    } else {
        if ((unsigned long)hr == 0x8876086AUL) {
            d3dUnavailable = 1;
            LogMsg("-> [EXPECTED LIMITATION] Direct3D 9 CreateDevice returned D3DERR_NOTAVAILABLE (0x%08lX).\n",
                   (unsigned long)hr);
            LogMsg("-> This display driver has no Direct3D HAL; the failure does not diagnose OpenGL or its GPU copy engine.\n");
        } else {
            LogMsg("-> [FAIL] Direct3D 9 device creation result: 0x%08lX\n", (unsigned long)hr);
            failures++;
        }
    }

    pD3D->lpVtbl->Release(pD3D);
    FreeLibrary(hD3D9);
}

/* ---------------- Window Procedure & Main ---------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static BOOL CALLBACK FindSecondary(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data)
{
    MONITORINFO info;
    (void)dc; (void)rect;
    memset(&info,0,sizeof(info)); info.cbSize=sizeof(info);
    if (GetMonitorInfo(monitor,&info) && !(info.dwFlags & MONITORINFOF_PRIMARY)) {
        *(RECT *)data=info.rcMonitor;
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    InitLogging();
    LogMsg("[CLIENT_PROCESS] pid=%lu\n", (unsigned long)GetCurrentProcessId());

    LogMsg("\n==================================================================\n");
    LogMsg(" NVX XP Graphics Verification Suite (2D correctness and 3D renderer audit)\n");
    LogMsg("==================================================================\n");

    int only2d = 0, secondary = 0, rapid3d = 0, openglOnly = 0;
    RECT target = {0,0,0,0};
    int originX = 100, originY = 100;
    for (int arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--2d-only")) only2d = 1;
        if (!strcmp(argv[arg], "--3d-rapid")) rapid3d = 1;
        if (!strcmp(argv[arg], "--secondary")) secondary = 1;
        if (!strcmp(argv[arg], "--mesa-gpu-bridge")) mesaGpuBridgeProbe = 1;
        if (!strcmp(argv[arg], "--opengl-only")) openglOnly = 1;
    }
    if (GetFileAttributesA("C:\\test_accel\\rapid3d.flag") !=
        INVALID_FILE_ATTRIBUTES)
        rapid3d = 1;
    if (GetFileAttributesA("C:\\test_accel\\opengl_only.flag") !=
        INVALID_FILE_ATTRIBUTES)
        openglOnly = 1;
    SetEnvironmentVariableA("NVX_MESA_GALLIUM_BRIDGE",
                            mesaGpuBridgeProbe ? "1" : NULL);
#if defined(NVX_MESA_BRIDGE_DISABLE_DEPTH) && NVX_MESA_BRIDGE_DISABLE_DEPTH
    SetEnvironmentVariableA("NVX_MESA_BRIDGE_DISABLE_DEPTH",
                            mesaGpuBridgeProbe ? "1" : NULL);
#else
    SetEnvironmentVariableA("NVX_MESA_BRIDGE_DISABLE_DEPTH", NULL);
#endif
#if defined(NVX_MESA_DEPTH_MODE_OVERRIDE) && NVX_MESA_DEPTH_MODE_OVERRIDE == 2
    SetEnvironmentVariableA("NVX_MESA_DEPTH_MODE", "2");
#elif defined(NVX_MESA_DEPTH_MODE_OVERRIDE) && NVX_MESA_DEPTH_MODE_OVERRIDE == 3
    SetEnvironmentVariableA("NVX_MESA_DEPTH_MODE", "3");
#else
    SetEnvironmentVariableA("NVX_MESA_DEPTH_MODE", NULL);
#endif
#if defined(NVX_MESA_ZF32_DIAGNOSTIC) && NVX_MESA_ZF32_DIAGNOSTIC
    SetEnvironmentVariableA("NVX_MESA_ZF32_DIAGNOSTIC",
                            mesaGpuBridgeProbe ? "1" : NULL);
#else
    SetEnvironmentVariableA("NVX_MESA_ZF32_DIAGNOSTIC", NULL);
#endif
    if (mesaGpuBridgeProbe)
        DeleteFileA("C:\\test_accel\\mesa_bridge.log");
    if (secondary) {
        EnumDisplayMonitors(NULL,NULL,FindSecondary,(LPARAM)&target);
        if (target.right <= target.left) {
            LogMsg("[FAIL] Secondary desktop monitor not attached.\n");
            CloseLogging(); return 1;
        }
        originX=target.left+100; originY=target.top+100;
    }
    LogMsg("-> Target=%s at %d,%d\n", secondary?"secondary":"primary",originX,originY);

    /* Register test window class */
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "NvxGpuAccelTestClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "Antigravity GPU Acceleration Test",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              originX, originY, 640, 480, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        LogMsg("Error creating test window!\n");
        CloseLogging();
        return 1;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, originX, originY, 640, 480, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    HDC hdc = GetDC(hwnd);

    if (rapid3d) {
        LogMsg("[MESA_GPU_CUBE_RESULT] SKIPPED reason=rapid_3d_mode (use the full guest test to run depth and no-depth cube comparisons)\n");
        int result=(TestNvxBufferRoundTrip(hdc,1U,"VRAM")&&
                    TestNvxBufferRoundTrip(hdc,NVX_3D_USAGE_GART_BUFFER,"GART")&&
                    TestNvc0ClearEscape(hdc)&&
                    TestNativeNvc0FullSizeClear(hdc))?0:1;
        NvxTestCloseClient(hdc);
        ReleaseDC(hwnd,hdc);
        DestroyWindow(hwnd);
        LogMsg("[NVX_3D_RAPID_RESULT] %s\n",result?"FAIL":"PASS");
        CloseLogging();
        return result;
    }

    /* Run Tests */
    if (!openglOnly && !only2d &&
        (!TestNvxBufferRoundTrip(hdc,1U,"VRAM")||
         !TestNvxBufferRoundTrip(hdc,NVX_3D_USAGE_GART_BUFFER,"GART")||
         !TestNvxGartMaximumUpload(hdc)))
        failures++;
    if (!openglOnly) {
        Test2D(hwnd, hdc);
        LogMsg("[2D_RESULT] %s\n", failures ? "FAIL" : "PASS");
    } else {
        LogMsg("[2D_RESULT] SKIPPED (OpenGL-only diagnostic)\n");
    }
    if (!only2d) {
#if !defined(NVX_MESA_APPLOCAL_FRONTEND)
        char runMesaFrontendTest[2];
        /* Keep the system ICD test isolated. The direct Mesa frontend loads
           a second WGL stack into this process and is opt-in for diagnostics. */
        if (GetEnvironmentVariableA("NVX_RUN_MESA_FRONTEND_TEST",
                                      runMesaFrontendTest,
                                      sizeof(runMesaFrontendTest)) > 0 &&
              runMesaFrontendTest[0] == '1' &&
            GetFileAttributesA("C:\\test_accel\\nvxmesa.dll") != INVALID_FILE_ATTRIBUTES)
            TestMesaWglFrontend(wc.hInstance);
#endif
        int before = failures;
        TestOpenGL(hwnd, hdc);
        if (mesaGpuBridgeProbe)
            DumpMesaBridgeTrace();
        LogMsg("[OPENGL_RESULT] %s\n", failures != before ? "FAIL" : software3d ? "SOFTWARE" : "RENDERED");
        if (!openglOnly)
            TestNvx3dSubmission(hdc);
        before = failures;
        TestDirect3D9(hwnd);
        LogMsg("[D3D9_RESULT] %s\n", failures != before ? "FAIL" : d3dUnavailable ? "UNAVAILABLE (no Direct3D HAL)" : "RENDERED (see device type)");
    }

    NvxTestCloseClient(hdc);

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);

    LogMsg("\n==================================================================\n");
    LogMsg(" Failures=%d software3d=%d\n", failures, software3d);
    LogMsg(" [TEST_COMPLETE] Status: %s\n", failures ? "FAIL" : software3d ? "SOFTWARE_3D_ONLY" : "PASS");
    LogMsg("==================================================================\n\n");

    /* If not interactive, exit promptly */
    int autoExit = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-exit")) autoExit = 0;
    }

    if (!autoExit) {
        MSG msg;
        while (GetMessageA(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    CloseLogging();
    return failures ? 1 : software3d ? 2 : 0;
}
