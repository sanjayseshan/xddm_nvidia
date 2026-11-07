; vxd_entry.asm - Standard Windows 98 32-bit LE Device Descriptor Block (DDB) & Dispatcher
BITS 32

SECTION .text.entry

global _DDB
global _Control_Dispatcher
extern _NvxVxDDeviceInit
extern _NvxVxDInitComplete
extern _NvxVxDDeviceIoControl

; DDB Structure (offset 0 in flat binary)
_DDB:
    dd 0                    ; 00h: DDB_Next
    dw 0x0400               ; 04h: DDB_SDK_Version (Windows 4.0)
    dw 0                    ; 06h: DDB_Req_Device_Number (undefined)
    db 1, 0                 ; 08h: DDB_Dev_Major_Version, DDB_Dev_Minor_Version
    dw 0                    ; 0Ah: DDB_Flags
    db "NVX     "           ; 0Ch: DDB_Name (8 chars blank padded)
    dd 0x28000000           ; 14h: DDB_Init_Order (VDD_Init_Order)
    dd _Control_Dispatcher  ; 18h: DDB_Control_Proc
    dd 0                    ; 1Ch: DDB_V86_API_Proc
    dd 0                    ; 20h: DDB_PM_API_Proc
    dd 0                    ; 24h: DDB_V86_API_CSIP
    dd 0                    ; 28h: DDB_PM_API_CSIP
    dd 0                    ; 2Ch: DDB_Reference_Data
    dd 0                    ; 30h: DDB_Service_Table_Ptr
    dd 0                    ; 34h: DDB_Service_Table_Size
    dd 0                    ; 38h: DDB_Win32_Service_Table
    dd 0x50726576           ; 3Ch: DDB_Prev ('Prev' signature required by Win98 VMM)
    dd 0x00000050           ; 40h: DDB_Size (80 bytes required for Win 4.0 DDB)
    dd 0x52737631           ; 44h: DDB_Reserved1 ('Rsv1')
    dd 0x52737632           ; 48h: DDB_Reserved2 ('Rsv2')
    dd 0x52737633           ; 4Ch: DDB_Reserved3 ('Rsv3')

_Control_Dispatcher:
    cmp eax, 0x00000001     ; Device_Init
    je .on_device_init
    cmp eax, 0x00000002     ; Init_Complete
    je .on_init_complete
    cmp eax, 0x0000003A     ; PNP_NEW_DEVNODE
    je .on_device_init
    cmp eax, 0x00000023     ; W32_DeviceIOControl
    je .on_device_io_control
    clc                     ; Success
    ret

.on_device_init:
    pushad
    call _NvxVxDDeviceInit
    popad
    clc
    ret

.on_init_complete:
    pushad
    call _NvxVxDInitComplete
    popad
    clc
    ret

.on_device_io_control:
    push esi                ; ESI = DIOCParams pointer
    call _NvxVxDDeviceIoControl
    add esp, 4
    clc
    ret

