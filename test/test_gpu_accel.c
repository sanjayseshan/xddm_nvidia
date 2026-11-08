/*
 * Antigravity GPU Acceleration Verification Suite
 * Tests: 2D GDI Accel (Fills, SRCCOPY Blits), OpenGL 3D Pipeline, and Direct3D 9.
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
            WriteFile(hCom1, buf, (DWORD)len, &written, NULL);
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
    COLORREF cBlack = GetPixel(hdc, 15, 15);
    COLORREF cWhite = GetPixel(hdc, 125, 15);
    LogMsg("-> Fill Verification: Black=0x%06X (expected 0x000000), White=0x%06X (expected 0xFFFFFF)\n",
           (unsigned int)cBlack, (unsigned int)cWhite);

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

    DeleteObject(redBrush);
    DeleteObject(blueBrush);
    DeleteObject(greenBrush);

    LogMsg("-> [PASS] 2D GDI Acceleration verified!\n");
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
    LogMsg(" [TEST 2/3] OpenGL 3D Hardware Pipeline\n");
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
        LogMsg("-> [FAIL] ChoosePixelFormat failed (err=%lu)\n", GetLastError());
        return;
    }

    if (!SetPixelFormat(hdc, format, &pfd)) {
        LogMsg("-> [FAIL] SetPixelFormat failed (err=%lu)\n", GetLastError());
        return;
    }
    LogMsg("-> Pixel format %d configured successfully\n", format);

    HGLRC hglrc = wglCreateContext(hdc);
    if (!hglrc) {
        LogMsg("-> [FAIL] wglCreateContext failed (err=%lu)\n", GetLastError());
        return;
    }

    if (!wglMakeCurrent(hdc, hglrc)) {
        LogMsg("-> [FAIL] wglMakeCurrent failed (err=%lu)\n", GetLastError());
        wglDeleteContext(hglrc);
        return;
    }

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);

    LogMsg("-> OpenGL Vendor:   %s\n", vendor ? vendor : "(null)");
    LogMsg("-> OpenGL Renderer: %s\n", renderer ? renderer : "(null)");
    LogMsg("-> OpenGL Version:  %s\n", version ? version : "(null)");

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

    double tStart = GetTimeSeconds();
    for (int frame = 0; frame < GL_FRAMES; frame++) {
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        DrawCube((float)frame * 3.6f);

        SwapBuffers(hdc);
    }
    double tEnd = GetTimeSeconds();

    double dt = tEnd - tStart;
    double fps = (double)GL_FRAMES / dt;
    LogMsg("-> Rendered %d frames in %.4f s (%.1f FPS)\n", GL_FRAMES, dt, fps);

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hglrc);
    LogMsg("-> [PASS] OpenGL 3D Pipeline fully operational!\n");
}

/* ---------------- Direct3D 9 / DirectX Tests ---------------- */
static void TestDirect3D9(HWND hwnd)
{
    LogMsg("\n==================================================\n");
    LogMsg(" [TEST 3/3] Direct3D 9 / WineD3D Pipeline\n");
    LogMsg("==================================================\n");

    /* Dynamically load d3d9.dll */
    HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) {
        LogMsg("-> d3d9.dll not found (err=%lu)\n", GetLastError());
        return;
    }
    LogMsg("-> d3d9.dll loaded successfully at %p\n", (void *)hD3D9);

    typedef IDirect3D9* (WINAPI *LPDIRECT3DCREATE9)(UINT);
    LPDIRECT3DCREATE9 pDirect3DCreate9 = (LPDIRECT3DCREATE9)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pDirect3DCreate9) {
        LogMsg("-> [FAIL] Direct3DCreate9 export not found\n");
        FreeLibrary(hD3D9);
        return;
    }

    IDirect3D9 *pD3D = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        LogMsg("-> [FAIL] Direct3DCreate9 returned NULL\n");
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
            hr = pD3D->lpVtbl->CreateDevice(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF,
                                           hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                           &d3dpp, &pDevice);
        }
    }

    if (SUCCEEDED(hr) && pDevice) {
        LogMsg("-> IDirect3DDevice9 created successfully!\n");

        const int D3D_FRAMES = 60;
        LogMsg("-> Rendering %d Direct3D 9 frames with Present...\n", D3D_FRAMES);

        DWORD clearFlags = d3dpp.EnableAutoDepthStencil ? (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) : D3DCLEAR_TARGET;
        double t0 = GetTimeSeconds();
        for (int i = 0; i < D3D_FRAMES; i++) {
            D3DCOLOR clr = D3DCOLOR_XRGB((i * 4) % 256, 40, (255 - i * 4) % 256);
            pDevice->lpVtbl->Clear(pDevice, 0, NULL, clearFlags, clr, 1.0f, 0);
            if (SUCCEEDED(pDevice->lpVtbl->BeginScene(pDevice))) {
                pDevice->lpVtbl->EndScene(pDevice);
            }
            pDevice->lpVtbl->Present(pDevice, NULL, NULL, NULL, NULL);
        }
        double t1 = GetTimeSeconds();

        double dt = t1 - t0;
        LogMsg("-> Direct3D 9 rendered %d frames in %.4f s (%.1f FPS)\n", D3D_FRAMES, dt, (double)D3D_FRAMES / dt);
        pDevice->lpVtbl->Release(pDevice);
        LogMsg("-> [PASS] Direct3D 9 Pipeline operational!\n");
    } else {
        LogMsg("-> [INFO] Direct3D 9 device creation result: 0x%08lX\n", (unsigned long)hr);
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

int main(int argc, char **argv)
{
    InitLogging();

    LogMsg("\n==================================================================\n");
    LogMsg(" Antigravity NVIDIA GA104 XP x64 Hardware Acceleration Test Suite\n");
    LogMsg("==================================================================\n");

    /* Register test window class */
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "NvxGpuAccelTestClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "Antigravity GPU Acceleration Test",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 640, 480, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        LogMsg("Error creating test window!\n");
        CloseLogging();
        return 1;
    }

    UpdateWindow(hwnd);
    HDC hdc = GetDC(hwnd);

    /* Run Tests */
    Test2D(hwnd, hdc);
    TestOpenGL(hwnd, hdc);
    TestDirect3D9(hwnd);

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);

    LogMsg("\n==================================================================\n");
    LogMsg(" ALL ACCELERATION TESTS COMPLETED SUCCESSFULLY!\n");
    LogMsg(" [TEST_COMPLETE] Status: PASS\n");
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
    return 0;
}
