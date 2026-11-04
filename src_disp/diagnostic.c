/* Small, independent file logger for display-DDI startup diagnostics. */
#include "driver.h"

typedef struct _NVX_DISP_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} NVX_DISP_UNICODE_STRING;

typedef struct _NVX_DISP_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    NVX_DISP_UNICODE_STRING *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} NVX_DISP_OBJECT_ATTRIBUTES;

typedef struct _NVX_DISP_IO_STATUS_BLOCK {
    union { LONG Status; PVOID Pointer; } u;
    ULONG_PTR Information;
} NVX_DISP_IO_STATUS_BLOCK;

extern LONG NTAPI ZwCreateFile(PHANDLE, ACCESS_MASK,
    NVX_DISP_OBJECT_ATTRIBUTES *, NVX_DISP_IO_STATUS_BLOCK *,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
extern LONG NTAPI ZwWriteFile(HANDLE, HANDLE, PVOID, PVOID,
    NVX_DISP_IO_STATUS_BLOCK *, PVOID, ULONG, PLARGE_INTEGER, PULONG);
extern LONG NTAPI ZwClose(HANDLE);

static HANDLE g_NvxDispLog;
static BOOLEAN g_NvxDispLogAttempted;

static VOID NvxDispWrite(const CHAR *buffer, ULONG length)
{
    NVX_DISP_IO_STATUS_BLOCK io;
    LARGE_INTEGER append;
    if (!g_NvxDispLog || !length) return;
    append.QuadPart = -1;
    (void)ZwWriteFile(g_NvxDispLog, NULL, NULL, NULL, &io,
        (PVOID)buffer, length, &append, NULL);
}

static VOID NvxDispOpenLog(VOID)
{
    static WCHAR pathBuffer[] = L"\\??\\C:\\WINDOWS\\nvxdisp.log";
    static const CHAR marker[] = "\r\n===== NVX display DDI run =====\r\n";
    NVX_DISP_UNICODE_STRING path;
    NVX_DISP_OBJECT_ATTRIBUTES attributes;
    NVX_DISP_IO_STATUS_BLOCK io;

    if (g_NvxDispLogAttempted) return;
    g_NvxDispLogAttempted = TRUE;
    path.Buffer = pathBuffer;
    path.Length = (USHORT)(sizeof(pathBuffer) - sizeof(WCHAR));
    path.MaximumLength = sizeof(pathBuffer);
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = NULL;
    attributes.ObjectName = &path;
    attributes.Attributes = 0x40; /* OBJ_CASE_INSENSITIVE */
    attributes.SecurityDescriptor = NULL;
    attributes.SecurityQualityOfService = NULL;
    if (ZwCreateFile(&g_NvxDispLog, 0x00010004, &attributes, &io, NULL,
        0x80, 0x00000007, 0x00000003, 0x00000020, NULL, 0) < 0) {
        g_NvxDispLog = NULL;
        return;
    }
    NvxDispWrite(marker, sizeof(marker) - 1);
}

static VOID NvxDispWriteHex(CHAR *buffer, ULONG value)
{
    LONG i;
    for (i = 7; i >= 0; --i) {
        ULONG nibble = value & 0xFUL;
        buffer[i] = (CHAR)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        value >>= 4;
    }
}

VOID NvxDispLogEvent(ULONG stage, ULONG value0, ULONG value1, ULONG value2)
{
    CHAR line[57];
    if (!g_NvxDispLogAttempted) NvxDispOpenLog();
    if (!g_NvxDispLog) return;
    line[0]='N'; line[1]='V'; line[2]='X'; line[3]='D'; line[4]=' ';
    line[5]='s'; line[6]='t'; line[7]='a'; line[8]='g'; line[9]='e'; line[10]='=';
    NvxDispWriteHex(line+11,stage);
    line[19]=' '; line[20]='v'; line[21]='0'; line[22]='=';
    NvxDispWriteHex(line+23,value0);
    line[31]=' '; line[32]='v'; line[33]='1'; line[34]='=';
    NvxDispWriteHex(line+35,value1);
    line[43]=' '; line[44]='v'; line[45]='2'; line[46]='=';
    NvxDispWriteHex(line+47,value2);
    line[55]='\r'; line[56]='\n';
    NvxDispWrite(line,sizeof(line));
}

VOID NvxDispCloseLog(VOID)
{
    if (g_NvxDispLog) {
        (void)ZwClose(g_NvxDispLog);
        g_NvxDispLog = NULL;
    }
}
