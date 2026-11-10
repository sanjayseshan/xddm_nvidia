#include <windows.h>

void NvxXpIcdTrace(const char *event);

void
NvxXpIcdTrace(const char *event)
{
   static const char path[] = "C:\\test_accel\\nvxogl_trace.txt";
   HANDLE file;
   DWORD written;
   DWORD length = 0;
   if (!event)
      return;
   while (event[length])
      length++;
   file = CreateFileA(path, FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   WriteFile(file, event, length, &written, NULL);
   WriteFile(file, "\r\n", 2, &written, NULL);
   CloseHandle(file);
}
