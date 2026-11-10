#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
   char path[MAX_PATH];
   HMODULE module;
   const char *exports[] = {
      "DrvCreateContext", "DrvSetContext", "DrvGetProcAddress",
      "DrvSetCallbackProcs", "DrvSwapBuffers"
   };

   if (argc != 2 || strlen(argv[1]) + 3 >= sizeof(path)) {
      fprintf(stderr, "usage: %s /path/to/nvxogl64.dll\n", argv[0]);
      return 2;
   }
   strcpy(path, "Z:");
   strcat(path, argv[1]);
   for (char *p = path; *p; p++)
      if (*p == '/')
         *p = '\\';

   module = LoadLibraryA(path);
   if (!module) {
      fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
      return 1;
   }
   for (unsigned i = 0; i < sizeof(exports) / sizeof(exports[0]); i++) {
      if (!GetProcAddress(module, exports[i])) {
         fprintf(stderr, "missing export: %s\n", exports[i]);
         return 3;
      }
   }
   puts("ICD loaded and required WGL exports resolved");
   FreeLibrary(module);
   return 0;
}
