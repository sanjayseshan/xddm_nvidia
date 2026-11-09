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

static HANDLE hCom1 = INVALID_HANDLE_VALUE;
static FILE *fLog = NULL;
static int failures = 0;
static int software3d = 0;
static int d3dUnavailable = 0;

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
    LogMsg("\n==================================================\n");
    LogMsg(" [TEST 2/3] OpenGL Renderer and Presentation Audit\n");
    LogMsg("==================================================\n");

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
    if (!format) {
        LogMsg("-> [FAIL] ChoosePixelFormat failed (err=%lu)\n", GetLastError()); failures++;
        return;
    }

    if (!SetPixelFormat(hdc, format, &pfd)) {
        LogMsg("-> [FAIL] SetPixelFormat failed (err=%lu)\n", GetLastError()); failures++;
        return;
    }
    LogMsg("-> Pixel format %d configured successfully\n", format);

    HGLRC hglrc = wglCreateContext(hdc);
    if (!hglrc) {
        LogMsg("-> [FAIL] wglCreateContext failed (err=%lu)\n", GetLastError()); failures++;
        return;
    }

    if (!wglMakeCurrent(hdc, hglrc)) {
        LogMsg("-> [FAIL] wglMakeCurrent failed (err=%lu)\n", GetLastError()); failures++;
        wglDeleteContext(hglrc);
        return;
    }

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
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* Simple frustum setup */
    float aspect = (h > 0) ? (float)w / (float)h : 1.0f;
    glFrustum(-aspect, aspect, -1.0f, 1.0f, 1.0f, 100.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);

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
    LogMsg("-> OpenGL rendering test completed; renderer classification reported above.\n");
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
    }
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
        int before = failures;
        TestOpenGL(hwnd, hdc);
        LogMsg("[OPENGL_RESULT] %s\n", failures != before ? "FAIL" : software3d ? "SOFTWARE" : "RENDERED");
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
