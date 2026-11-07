#include <windows.h>
#include <stdio.h>

#define IOCTL_NVX_GET_HEAD0_INFO  0x0001
#define IOCTL_NVX_GET_HEAD1_INFO  0x0002
#define IOCTL_NVX_GPU_COPY        0x0003
#define IOCTL_NVX_INIT_GSP        0x0004

int main(int argc, char **argv) {
    printf("NVX GSP Bootstrap & Acceleration Loader for Windows 98\n");
    HANDLE h = CreateFileA("\\\\.\\NVX", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("ERROR: Could not open \\\\.\\NVX device handle (error %lu)\n", GetLastError());
        return 1;
    }
    printf("Connected to NVX VxD successfully.\n");
    printf("Triggering authentic Ampere GSP bootstrap and copy engine initialization...\n");
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_NVX_INIT_GSP, NULL, 0, NULL, 0, &returned, NULL);
    if (ok) {
        printf("SUCCESS: GSP bootstrap and acceleration command delivered to miniport!\n");
    } else {
        printf("WARNING: DeviceIoControl returned error %lu\n", GetLastError());
    }
    CloseHandle(h);
    return 0;
}

