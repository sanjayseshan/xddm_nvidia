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

static void DumpMesaBridgeTrace(void)
{
    typedef struct MESA_BRIDGE_STATUS {
        DWORD size;
        DWORD version;
        LONG screenCount;
        LONG callbackCount;
        LONG gpuBatchCount;
        char lastMessage[96];
        LONG depthStageCount;
        char depthStages[3][96];
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
    LogMsg("[MESA_BRIDGE_STATUS] screens=%ld callbacks=%ld gpu_batches=%ld version=%lu last=%s\n",
           status.screenCount, status.callbackCount, status.gpuBatchCount,
           (unsigned long)status.version, status.lastMessage);
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

static void TestOpenGL(HWND hwnd, HDC hdc)
{
    /* The bridge appends per-process results. Remove any prior guest run so
       batch-count deltas cannot be satisfied by stale hardware submissions. */
    BOOL bridgeTraceDeleted = DeleteFileA("C:\\test_accel\\mesa_bridge.log");
    DWORD bridgeTraceDeleteError = bridgeTraceDeleted ? ERROR_SUCCESS : GetLastError();
    LogMsg("[MESA_BRIDGE_TRACE_RESET] delete_error=%lu\n", bridgeTraceDeleteError);
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
    DumpOpenGlTrace(wglTraceStart);
    DumpMesaIcdTrace();
    if (!hglrc) {
        LogMsg("-> wglCreateContext failed (err=%lu)\n", GetLastError());
        LogMsg("-> ICD already loaded by WGL: %s\n",
               GetModuleHandleA("nvxogl64.dll") ? "yes" : "no");
        HMODULE icd = LoadLibraryA("nvxogl64.dll");
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
            typedef ULONG_PTR (APIENTRY *PFN_DRV_CREATE_CONTEXT)(HDC);
            typedef BOOL (APIENTRY *PFN_DRV_DELETE_CONTEXT)(ULONG_PTR);
            PFN_DRV_CREATE_CONTEXT createContext =
                (PFN_DRV_CREATE_CONTEXT)GetProcAddress(icd, "DrvCreateContext");
            PFN_DRV_DELETE_CONTEXT deleteContext =
                (PFN_DRV_DELETE_CONTEXT)GetProcAddress(icd, "DrvDeleteContext");
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
                if (directContext && deleteContext)
                    deleteContext(directContext);
            }
            LogMsg("-> Mesa ICD callback trace after direct ICD probes:\n");
            DumpMesaIcdTrace();
            wglTraceStart = openGlTraceCount;
            hglrc = wglCreateContext(hdc);
            DumpOpenGlTrace(wglTraceStart);
            LogMsg("-> WGL retry after explicit ICD initialization: %s (err=%lu)\n",
                   hglrc ? "success" : "failed", GetLastError());
            FreeLibrary(icd);
        }
        if (!hglrc) {
            LogMsg("-> [FAIL] XP WGL could not create an ICD context\n");
            failures++;
            return;
        }
    }

    if (!wglMakeCurrent(hdc, hglrc)) {
        LogMsg("-> [FAIL] wglMakeCurrent failed (err=%lu)\n", GetLastError()); failures++;
        wglDeleteContext(hglrc);
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
        if (!SwapBuffers(hdc)) {
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
        DumpMesaBridgeTrace();
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
        if (depthError == GL_NO_ERROR && SwapBuffers(hdc)) {
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
        BYTE cubePixels[64 * 16 * 4];
        unsigned coloredPixels = 0, p;
        GLenum error;
        unsigned batchesBefore = mesaGpuBridgeBatchCount;
        glViewport(0, 0, 64, 16);
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        DrawCube(17.0f);
        glFinish();
        if (!SwapBuffers(hdc)) {
            LogMsg("[MESA_GPU_CUBE_RESULT] FAIL (SwapBuffers error=%lu)\n",
                   GetLastError());
            failures++;
        } else {
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, 64, 16, GL_RGBA, GL_UNSIGNED_BYTE, cubePixels);
            error = glGetError();
            for (p = 0; p < 64 * 16; p++)
                if (cubePixels[p * 4 + 0] > 16 || cubePixels[p * 4 + 1] > 16 ||
                    cubePixels[p * 4 + 2] > 16)
                    coloredPixels++;
            DumpMesaBridgeTrace();
            if (error == GL_NO_ERROR && coloredPixels >= 8 &&
                mesaGpuBridgeBatchCount > batchesBefore) {
                LogMsg("[MESA_GPU_CUBE_RESULT] PASS colored_pixels=%u new_gpu_batches=%u\n",
                       coloredPixels, mesaGpuBridgeBatchCount - batchesBefore);
            } else {
                LogMsg("[MESA_GPU_CUBE_RESULT] FAIL colored_pixels=%u gpu_batches=%u error=0x%X\n",
                       coloredPixels, mesaGpuBridgeBatchCount - batchesBefore,
                       error);
                failures++;
            }
        }
        glViewport(0, 0, w, h);
    }

    glEnable(GL_DEPTH_TEST);

    const int GL_FRAMES = 100;
    LogMsg("-> Rendering %d 3D animated cube frames with SwapBuffers...\n", GL_FRAMES);

    int framesRendered = 0;
    double tStart = GetTimeSeconds();
    for (int frame = 0; frame < GL_FRAMES; frame++) {
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        DrawCube((float)frame * 3.6f);

        if (!SwapBuffers(hdc)) {
            LogMsg("-> [FAIL] SwapBuffers failed: %lu\n", GetLastError());
            failures++;
            break;
        }
        framesRendered++;
    }
    double tEnd = GetTimeSeconds();

    double dt = tEnd - tStart;
    double fps = dt > 0 ? (double)framesRendered / dt : 0;
    LogMsg("-> Rendered %d frames in %.4f s (%.1f FPS)\n", framesRendered, dt, fps);

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
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hglrc);
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

static void TestNvx3dSubmission(HDC hdc)
{
    NVX_3D_QUERY_CAPS caps={0};
    NVX_3D_QUERY_CAPS verifiedCaps={0};
    NVX_3D_ALLOC_BUFFER alloc={0},allocated={0};
    NVX_3D_ALLOC_BUFFER vertexAlloc={0},vertexAllocated={0};
    struct { NVX_3D_UPLOAD_BUFFER header; UCHAR data[256]; } upload;
    struct { NVX_3D_UPLOAD_BUFFER header; float data[18]; } vertexUpload;
    struct { NVX_3D_UPLOAD_BUFFER header; float data[36]; } batchUpload;
    NVX_3D_SUBMIT submit={0},submitted={0};
    NVX_3D_READBACK readbackRequest={0};
    NVX_3D_WAIT_FENCE wait={0},waited={0};
    NVX_3D_FREE_BUFFER freeRequest={0};
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
    LogMsg("-> NVX 3D features=0x%08lX class=0x%08lX ABI=%lu\n",
        (unsigned long)caps.Features,(unsigned long)caps.GraphicsClass,
        (unsigned long)caps.Version);
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
       caps.Version!=NVX_3D_ABI_VERSION) {
        LogMsg("-> [INFO] safe 3D allocation/submit capabilities unavailable\n");
#if defined(NVX_REQUIRE_3D_ABI) && NVX_REQUIRE_3D_ABI
        LogMsg("[NVX_3D_RESULT] FAIL\n");failures++;
#else
        LogMsg("[NVX_3D_RESULT] UNAVAILABLE\n");
#endif
        return;
    }
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
    if(clearSubmitted) {
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
                    (LPSTR)&depthSubmitted)) { ok=0; }
            memset(&depthSubmit,0,sizeof(depthSubmit));
            memset(&depthSubmitted,0,sizeof(depthSubmitted));
            depthSubmit.Size=sizeof(depthSubmit);
            depthSubmit.Version=NVX_3D_ABI_VERSION;
            depthSubmit.Operation=NVX_3D_SUBMIT_CLEAR_DEPTH;
            depthSubmit.Width=64;depthSubmit.Height=16;
            depthSubmit.BufferId=depthAllocated.BufferId;
            if(!ExtEscape(hdc,NVX_ESCAPE_3D_SUBMIT,sizeof(depthSubmit),
                    (LPCSTR)&depthSubmit,sizeof(depthSubmitted),
                    (LPSTR)&depthSubmitted)) { ok=0; }
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
                LogMsg("-> [FAIL] ZF32 depth-tested known triangle failed\n");
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
    if(clearSubmitted&&vertexReady) {
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

    LogMsg("\n==================================================================\n");
    LogMsg(" NVX XP Graphics Verification Suite (2D correctness and 3D renderer audit)\n");
    LogMsg("==================================================================\n");

    int only2d = 0, secondary = 0;
    RECT target = {0,0,0,0};
    int originX = 100, originY = 100;
    for (int arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--2d-only")) only2d = 1;
        if (!strcmp(argv[arg], "--secondary")) secondary = 1;
        if (!strcmp(argv[arg], "--mesa-gpu-bridge")) mesaGpuBridgeProbe = 1;
    }
    SetEnvironmentVariableA("NVX_MESA_GALLIUM_BRIDGE",
                            mesaGpuBridgeProbe ? "1" : NULL);
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

    /* Run Tests */
    Test2D(hwnd, hdc);
    LogMsg("[2D_RESULT] %s\n", failures ? "FAIL" : "PASS");
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
        TestNvx3dSubmission(hdc);
        before = failures;
        TestDirect3D9(hwnd);
        LogMsg("[D3D9_RESULT] %s\n", failures != before ? "FAIL" : d3dUnavailable ? "UNAVAILABLE (no Direct3D HAL)" : "RENDERED (see device type)");
    }

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
