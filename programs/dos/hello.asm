; SPDX-FileCopyrightText: 2026 Yo-yo-ooo
; SPDX-License-Identifier: MIT
; ----------------------------------------------------------------------------
; hello.asm — the smallest honest DOS program: a .COM that only uses the two
; services DOSVM now provides (int 21h AH=09h print, AH=4Ch terminate).
;
;   nasm -f bin -o hello.com hello.asm
;
; Loaded at PSP:0x0100 with DS=ES=CS=0x7100 and SS:SP pointing at the reserved
; low-memory stack, so it runs identically under MS-DOS and under DOSVM.
; ----------------------------------------------------------------------------
        org 0x100
        bits 16

start:
        mov     dx, msg_hello
        mov     ah, 0x09              ; print $-terminated string
        int     0x21

        mov     dx, msg_ver
        mov     ah, 0x09
        int     0x21

        mov     ah, 0x30              ; "get version" -- proves AX survives
        int     0x21
        mov     dx, ax                ; stash AX, print it as two hex digits
        mov     al, dh
        call    puthex
        mov     al, dl
        call    puthex
        mov     dx, msg_crlf
        mov     ah, 0x09
        int     0x21

        mov     ax, 0x4C00            ; terminate, exit code 0
        int     0x21

; AL = byte to print as two hex digits, via int 21h AH=02h (putc).
puthex:
        push    ax
        push    dx
        mov     ah, al
        shr     al, 1
        shr     al, 1
        shr     al, 1
        shr     al, 1
        call    .nib
        mov     al, ah
        call    .nib
        pop     dx
        pop     ax
        ret
.nib:
        and     al, 0x0F
        cmp     al, 10
        jb      .digit
        add     al, 'A' - 10
        jmp     .out
.digit:
        add     al, '0'
.out:
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        ret

msg_hello: db "Hello from a real 16-bit .COM in REAL MODE!", 0x0D, 0x0A, "$"
msg_ver:   db "int 21h AH=30h reports DOS version ", "$"
msg_crlf:  db 0x0D, 0x0A, "$"
