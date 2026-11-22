; SPDX-FileCopyrightText: 2026 Yo-yo-ooo
; SPDX-License-Identifier: MIT

; filedemo.asm - 16-bit .COM that exercises the host file bridge end to end.
;
; Every INT 21h file call (3Dh open / 3Fh read / 3Eh close) is parked by the
; trampoline: the 16-bit side records the request in the state page and leaves
; the machine, programs/dos performs the real operation against the VFS, then
; re-enters and iret's back here with AX/CF already filled in.  If this prints
; "READ OK, bytes=16" the whole round trip works.
;
; Build: nasm -f bin -o filedemo.com filedemo.asm

    org 0x100
    jmp start

path  db "C:\SYSTEM\DOS\HELLO.COM", 0
ok2   db "READ OK, bytes=$"
err1  db "OPEN FAILED$"
err2  db "READ FAILED$"
buf   db 0

start:
    mov dx, path
    mov ax, 0x3D00                 ; AH=3Dh open, AL=0 read-only
    int 0x21
    jc  open_fail
    mov bx, ax                     ; BX = handle

    mov ah, 0x3F                   ; AH=3Fh read
    mov cx, 16
    mov dx, buf
    int 0x21
    jc  read_fail

    push ax                        ; keep the byte count across close
    mov ah, 0x3E                   ; AH=3Eh close (BX still the handle)
    int 0x21

    mov dx, ok2
    mov ah, 0x09
    int 0x21
    pop ax
    call putdec
    jmp done

open_fail:
    mov dx, err1
    mov ah, 0x09
    int 0x21
    jmp done
read_fail:
    mov dx, err2
    mov ah, 0x09
    int 0x21
done:
    mov ax, 0x4C00
    int 0x21

; AX -> decimal on the console (INT 21h AH=02, one digit at a time).
putdec:
    push ax
    push bx
    push cx
    push dx
    xor cx, cx
    mov bx, 10
.div:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .div
.out:
    pop ax
    add al, '0'
    mov dl, al
    mov ah, 0x02
    int 0x21
    loop .out
    pop dx
    pop cx
    pop bx
    pop ax
    ret
