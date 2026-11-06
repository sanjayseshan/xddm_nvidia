#include <windows.h>
#include <stdio.h>

typedef BOOL (WINAPI *PFN_EnumDisplayDevicesA)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
typedef LONG (WINAPI *PFN_ChangeDisplaySettingsExA)(LPCSTR, LPDEVMODEA, HWND, DWORD, LPVOID);

static void LogCom1(const char *msg) {
    while (*msg) {
        if (*msg == '\n') {
            __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)'\r'), "Nd"((unsigned short)0x3F8));
        }
        __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)*msg), "Nd"((unsigned short)0x3F8));
        msg++;
    }
}

static void CheckDevice(const char *devName, PFN_ChangeDisplaySettingsExA pChange) {
    char buf[256];
    wsprintfA(buf, "ATTACH_SEC: Checking '%s':\r\n", devName ? devName : "(primary/NULL)");
    LogCom1(buf);

    DEVMODEA dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    /* Test ENUM_CURRENT_SETTINGS */
    if (EnumDisplaySettingsA(devName, (DWORD)-1, &dm)) {
        wsprintfA(buf, "ATTACH_SEC:   Current mode: %ldx%ldx%ld @ %ldHz (pos %ld,%ld)\r\n",
                  dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel,
                  dm.dmDisplayFrequency, dm.dmPosition.x, dm.dmPosition.y);
        LogCom1(buf);
    } else {
        DWORD err = GetLastError();
        wsprintfA(buf, "ATTACH_SEC:   Current mode failed (err=%ld)\r\n", err);
        LogCom1(buf);
    }

    /* Test ENUM_REGISTRY_SETTINGS */
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(devName, (DWORD)-2, &dm)) {
        wsprintfA(buf, "ATTACH_SEC:   Registry mode: %ldx%ldx%ld @ %ldHz (pos %ld,%ld)\r\n",
                  dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel,
                  dm.dmDisplayFrequency, dm.dmPosition.x, dm.dmPosition.y);
        LogCom1(buf);
    } else {
        DWORD err = GetLastError();
        wsprintfA(buf, "ATTACH_SEC:   Registry mode failed (err=%ld)\r\n", err);
        LogCom1(buf);
    }

    /* Test mode 0 */
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    DWORD modeCount = 0;
    for (DWORD m = 0; EnumDisplaySettingsA(devName, m, &dm); m++) {
        modeCount++;
        if (m < 3 || dm.dmPelsWidth >= 1920) {
            wsprintfA(buf, "ATTACH_SEC:   Mode %ld: %ldx%ldx%ld @ %ldHz\r\n",
                      m, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
            LogCom1(buf);
        }
    }
    wsprintfA(buf, "ATTACH_SEC:   Total modes: %ld\r\n", modeCount);
    LogCom1(buf);

    /* If this is secondary, try attaching */
    if (devName && (strstr(devName, "2") || strstr(devName, "Display2"))) {
        DEVMODEA attachMode;
        ZeroMemory(&attachMode, sizeof(attachMode));
        attachMode.dmSize = sizeof(attachMode);
        attachMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_POSITION;
        attachMode.dmPelsWidth = 2560;
        attachMode.dmPelsHeight = 1440;
        attachMode.dmBitsPerPel = 32;
        attachMode.dmPosition.x = 1920;
        attachMode.dmPosition.y = 0;

        wsprintfA(buf, "ATTACH_SEC: Attempting ChangeDisplaySettingsExA('%s', 2560x1440x32 @ 1920,0)...\r\n", devName);
        LogCom1(buf);

        LONG ret = pChange(devName, &attachMode, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        wsprintfA(buf, "ATTACH_SEC: Result = %ld\r\n", ret);
        LogCom1(buf);

        if (ret != 0 && ret != 3) {
            attachMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
            ret = pChange(devName, &attachMode, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
            wsprintfA(buf, "ATTACH_SEC: Result (no pos) = %ld\r\n", ret);
            LogCom1(buf);
        }

        if (ret == 0 || ret == 3) {
            ret = pChange(NULL, NULL, NULL, 0, NULL);
            wsprintfA(buf, "ATTACH_SEC: Desktop refresh Result = %ld\r\n", ret);
            LogCom1(buf);
        }
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nShow;

    LogCom1("ATTACH_SEC: [START] Initializing Windows 98 Dual-Display attach helper...\r\n");

    typedef BOOL (WINAPI *PFN_EnumDisplayDevicesA)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
    typedef LONG (WINAPI *PFN_ChangeDisplaySettingsExA)(LPCSTR, LPDEVMODEA, HWND, DWORD, LPVOID);

    HMODULE hUser = GetModuleHandleA("USER32.DLL");
    if (!hUser) hUser = LoadLibraryA("USER32.DLL");
    PFN_EnumDisplayDevicesA pEnum = (PFN_EnumDisplayDevicesA)GetProcAddress(hUser, "EnumDisplayDevicesA");
    PFN_ChangeDisplaySettingsExA pChange = (PFN_ChangeDisplaySettingsExA)GetProcAddress(hUser, "ChangeDisplaySettingsExA");

    if (pEnum) {
        /* Test with different cb sizes */
        DWORD test_sizes[] = { 168 /* 0xA8 Win98 */, 424 /* 0x1A8 XP */, sizeof(DISPLAY_DEVICEA) };
        for (int s = 0; s < 3; s++) {
            DISPLAY_DEVICEA dd;
            ZeroMemory(&dd, sizeof(dd));
            dd.cb = test_sizes[s];
            char buf[128];
            wsprintfA(buf, "ATTACH_SEC: Testing EnumDisplayDevicesA with cb=%ld...\r\n", dd.cb);
            LogCom1(buf);
            BOOL ok = pEnum(NULL, 0, &dd, 0);
            wsprintfA(buf, "ATTACH_SEC:   Result=%d Name='%s' String='%s' Flags=0x%lx (err=%ld)\r\n",
                      ok, dd.DeviceName, dd.DeviceString, dd.StateFlags, GetLastError());
            LogCom1(buf);
            if (ok) {
                /* Enumerate all */
                for (DWORD i = 1; pEnum(NULL, i, &dd, 0); i++) {
                    wsprintfA(buf, "ATTACH_SEC:   Device %ld: Name='%s' String='%s' Flags=0x%lx\r\n",
                              i, dd.DeviceName, dd.DeviceString, dd.StateFlags);
                    LogCom1(buf);
                }
                break;
            }
        }
    }

    if (pChange) {
        CheckDevice(NULL, pChange);
        CheckDevice("\\\\.\\Display1", pChange);
        CheckDevice("\\\\.\\Display2", pChange);
        CheckDevice("DISPLAY", pChange);
        CheckDevice("DISPLAY2", pChange);
    }

    LogCom1("ATTACH_SEC: [DONE] Exiting attach helper.\r\n");
    return 0;
}
