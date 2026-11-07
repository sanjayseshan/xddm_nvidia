; vmm_wrappers.asm - VMM Service wrappers for 32-bit Ring 0 VxD
BITS 32
SECTION .text

global __MapPhysToLinear@12
global __PageAllocate@32
global __PageFree@8
global __CopyPageTable@16
global _Out_Debug_String@4

; void * __attribute__((stdcall)) _MapPhysToLinear(uint32_t physAddr, uint32_t nBytes, uint32_t flags);
__MapPhysToLinear@12:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    push dword [ebp + 16]  ; flags
    push dword [ebp + 12]  ; nBytes
    push dword [ebp + 8]   ; physAddr
    int 0x20
    dd 0x0001006C          ; VMM Service 0x6C (_MapPhysToLinear)
    add esp, 12
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret 12

; void * __attribute__((stdcall)) _PageAllocate(uint32_t nPages, uint32_t pType, uint32_t VM, uint32_t AlignMask, uint32_t minPhys, uint32_t maxPhys, uint32_t *PhysAddr, uint32_t flags);
__PageAllocate@32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    push dword [ebp + 36]  ; flags
    push dword [ebp + 32]  ; PhysAddr
    push dword [ebp + 28]  ; maxPhys
    push dword [ebp + 24]  ; minPhys
    push dword [ebp + 20]  ; AlignMask
    push dword [ebp + 16]  ; VM
    push dword [ebp + 12]  ; pType
    push dword [ebp + 8]   ; nPages
    int 0x20
    dd 0x00010053          ; VMM Service 0x53 (_PageAllocate)
    add esp, 32
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret 32

; int __attribute__((stdcall)) _PageFree(void *hMem, uint32_t flags);
__PageFree@8:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    push dword [ebp + 12]  ; flags
    push dword [ebp + 8]   ; hMem
    int 0x20
    dd 0x00010055          ; VMM Service 0x55 (_PageFree)
    add esp, 8
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret 8

; uint32_t __attribute__((stdcall)) _CopyPageTable(uint32_t LinPgNum, uint32_t nPages, void *Buffer, uint32_t Flags);
__CopyPageTable@16:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    push dword [ebp + 20]  ; Flags
    push dword [ebp + 16]  ; Buffer
    push dword [ebp + 12]  ; nPages
    push dword [ebp + 8]   ; LinPgNum
    int 0x20
    dd 0x00010061          ; VMM Service 0x61 (_CopyPageTable)
    add esp, 16
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret 16



; void __attribute__((stdcall)) Out_Debug_String(const char *str);
_Out_Debug_String@4:
    push ebp
    mov ebp, esp
    push esi
    mov esi, [ebp + 8]
    int 0x20
    dd 0x0001000C          ; VMM Service 0x0C (Out_Debug_String)
    pop esi
    pop ebp
    ret 4

global __R0_OpenFile@8
global __R0_ReadFile@16
global __R0_CloseFile@4

; int __attribute__((stdcall)) _R0_OpenFile(const char *path, uint32_t *phFile);
__R0_OpenFile@8:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi
    mov eax, 0x0000D500     ; R0_OPENCREATFILE
    mov ebx, 0x00000000     ; Read-only
    xor ecx, ecx            ; Normal attributes
    mov edx, 0x00000001     ; Action: open existing
    mov esi, [ebp + 8]      ; Filename path
    int 0x20
    dd 0x00400032          ; IFSMgr_Ring0_FileIO (Service 0x32)
    jc .open_err
    mov edx, [ebp + 12]     ; phFile
    mov [edx], eax          ; Store handle
    xor eax, eax            ; Success (0)
    jmp .open_done
.open_err:
    ; EAX contains error code
    mov edx, [ebp + 12]
    mov dword [edx], 0
.open_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 8

; int __attribute__((stdcall)) _R0_ReadFile(uint32_t hFile, uint32_t count, void *buf, uint32_t fileOffset, uint32_t *pBytesRead);
global __R0_ReadFileAt@20
__R0_ReadFileAt@20:
    push ebp
    mov ebp, esp
    push ebx
    push ecx
    push edx
    push esi
    push edi
    mov eax, 0x0000D600     ; R0_READFILE
    mov ebx, [ebp + 8]      ; File handle
    mov ecx, [ebp + 12]     ; Count
    mov esi, [ebp + 16]     ; Destination buffer
    mov edx, [ebp + 20]     ; File offset
    int 0x20
    dd 0x00400032          ; IFSMgr_Ring0_FileIO (Service 0x32)
    jc .read_err
    ; On success, EAX contains actual number of bytes read
    mov edx, [ebp + 24]     ; pBytesRead
    test edx, edx
    jz .read_ok
    mov [edx], eax          ; Store actual bytes read
.read_ok:
    xor eax, eax            ; Success (0)
    jmp .read_done
.read_err:
    ; On error, EAX contains error code
    mov edx, [ebp + 24]
    test edx, edx
    jz .read_done
    mov dword [edx], 0
.read_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop ebp
    ret 20

; int __attribute__((stdcall)) _R0_CloseFile(uint32_t hFile);
__R0_CloseFile@4:
    push ebp
    mov ebp, esp
    push ebx
    push edi
    push esi
    mov eax, 0x0000D700     ; R0_CLOSEFILE
    mov ebx, [ebp + 8]      ; File handle
    int 0x20
    dd 0x00400032
    jc .close_err
    xor eax, eax
    jmp .close_done
.close_err:
.close_done:
    pop esi
    pop edi
    pop ebx
    pop ebp
    ret 4

