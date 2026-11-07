; serial_log.asm - Tiny DOS .COM utility to write a string to COM1 (0x3F8)
BITS 16
ORG 0x100

start:
    cld
    mov si, 0x82        ; PSP command tail offset

    ; Skip leading spaces
skip_spaces:
    mov al, [si]
    cmp al, ' '
    jne check_empty
    inc si
    jmp skip_spaces

check_empty:
    cmp byte [si], 0x0D ; Enter / end of string
    jbe print_crlf

send_loop:
    mov al, [si]
    cmp al, 0x0D
    jbe print_crlf
    call send_char
    inc si
    jmp send_loop

print_crlf:
    mov al, 0x0D
    call send_char
    mov al, 0x0A
    call send_char

    ; Exit to DOS
    mov ax, 0x4C00
    int 0x21

send_char:
    push dx
    push ax
    push cx
    mov dx, 0x3FD       ; Line Status Register (COM1)
    mov cx, 0xFFFF
wait_tx:
    in al, dx
    test al, 0x20       ; Transmitter Holding Register Empty?
    jnz do_send
    loop wait_tx
    jmp done_send
do_send:
    pop cx
    pop ax
    mov dx, 0x3F8       ; Data register (COM1)
    out dx, al
    pop dx
    ret
done_send:
    pop cx
    pop ax
    pop dx
    ret

