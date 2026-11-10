/* MinGW winpthread uses the GetTickCount64 import pointer for timed waits. */
#include <windows.h>

typedef ULONGLONG (WINAPI *nvx_tick_count64_fn)(void);

static ULONGLONG WINAPI
nvx_xp_get_tick_count64(void)
{
    /* XP uptime wraps after about 49.7 days, acceptable for relative waits. */
    return (ULONGLONG)GetTickCount();
}

nvx_tick_count64_fn __imp_GetTickCount64 = nvx_xp_get_tick_count64;
