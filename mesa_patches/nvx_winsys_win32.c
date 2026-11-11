#include "nvx_winsys_win32.h"

static int
nvx_winsys_escape_hdc(void *opaque, uint32_t escape, uint32_t input_bytes,
                      const void *input, uint32_t output_bytes, void *output)
{
   HDC hdc = (HDC)opaque;
   return ExtEscape(hdc, (int)escape, (int)input_bytes, (LPCSTR)input,
                    (int)output_bytes, (LPSTR)output) != 0;
}

int
NvxWinsysInitHdc(NVX_WINSYS_DEVICE *device, HDC hdc)
{
   if (!hdc)
      return 0;
   return NvxWinsysInit(device, nvx_winsys_escape_hdc, hdc);
}
