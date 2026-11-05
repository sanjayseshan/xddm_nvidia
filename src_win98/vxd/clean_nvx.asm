BITS 32

%define DDK_VERSION         0x0400
%define UNDEFINED_DEVICE_ID 0x0000

; VMM Service Control messages
%define Sys_Critical_Init   0x00000000
%define Device_Init         0x00000001
%define Init_Complete       0x00000002
%define Sys_VM_Init         0x00000003
%define Sys_VM_Terminate    0x00000004
%define System_Exit         0x00000005
%define Sys_Critical_Exit   0x00000006
%define W32_DeviceIOControl 0x00000023

SECTION .text

global _DDB
global _Control_Dispatcher

_DDB:
    dd 0                    ; DDB_Next
    dw DDK_VERSION          ; DDB_SDK_Version
    dw UNDEFINED_DEVICE_ID  ; DDB_Req_Device_Number
    db 1, 0                 ; DDB_Dev_Major_Version, DDB_Dev_Minor_Version
    dw 0                    ; DDB_Flags
    db "NVX     "           ; DDB_Name (8 chars blank padded)
    dd 0x00010000           ; DDB_Init_Order
    dd _Control_Dispatcher  ; DDB_Control_Proc
    dd 0                    ; DDB_V86_API_Proc
    dd 0                    ; DDB_PM_API_Proc
    dd 0                    ; DDB_V86_API_CSIP
    dd 0                    ; DDB_PM_API_CSIP
    dd 0                    ; DDB_Reference_Data
    dd 0                    ; DDB_Service_Table_Ptr
    dd 0                    ; DDB_Service_Table_Size
    dd 0                    ; DDB_Win32_Service_Table
    dd 0                    ; DDB_Prev
    dd 0                    ; DDB_Size
    dd 0                    ; DDB_Reserved1
    dd 0                    ; DDB_Reserved2
    dd 0                    ; DDB_Reserved3

_Control_Dispatcher:
    pushfd
    pushad

    ; Output to COM1 (0x3F8)
    call print_banner

    popad
    popfd
    clc                     ; Clear Carry Flag = SUCCESS
    ret

print_banner:
    lea esi, [msg_banner]
.next_ch:
    lodsb
    test al, al
    jz .done
    mov dx, 0x3FD
.wait_tx:
    in al, dx
    test al, 0x20
    jz .wait_tx
    mov al, [esi - 1]
    mov dx, 0x3F8
    out dx, al
    jmp .next_ch
.done:
    ret

msg_banner:
    db "[NVX.VXD] Windows 98 SE Mini-VDD initialized successfully in Ring 0!", 0x0D, 0x0A, 0

