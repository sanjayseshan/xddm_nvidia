BITS 32
ORG 0x1086

start_hook:
    pushfd
    pushad

    ; Send COM1 banner
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
    popad
    popfd

    ; Original displaced instruction: sub eax, 1; cmp eax, 0x1B
    sub eax, 1
    cmp eax, 0x1B
    jmp 0x107A

msg_banner:
    db "[NVX.VXD] Ring 0 Mini-VDD Control_Dispatcher active!", 0x0D, 0x0A, 0

