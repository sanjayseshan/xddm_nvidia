; nvx_vxd_stub.asm - 32-bit LE VxD Stub for Windows 98 SE
; Implements Device Description Block (DDB) for NVX Mini-VDD

BITS 32

%define DDK_VERSION         0x0400
%define UNDEFINED_DEVICE_ID 0x0000
%define NVX_DEVICE_ID       0x3842   ; Custom VxD Device ID

; Standard VMM / VxD Service Control Call numbers
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
    dw NVX_DEVICE_ID        ; DDB_Req_Device_Number
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
    cmp eax, Sys_Critical_Init
    je .on_critical_init
    cmp eax, Device_Init
    je .on_device_init
    cmp eax, W32_DeviceIOControl
    je .on_device_io_control
    clc
    ret

.on_critical_init:
    ; Critical init logic
    clc
    ret

.on_device_init:
    ; Hardware / GSP detection and PCI scan
    clc
    ret

.on_device_io_control:
    ; Dispatch Ring 3 -> Ring 0 requests from nvx.drv
    xor eax, eax
    clc
    ret

