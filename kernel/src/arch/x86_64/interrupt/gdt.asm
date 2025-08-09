[global gdt_reload_seg]
gdt_reload_seg:
    push 0x08
    lea rax, [rel .gdt_reload_cs]
    push rax
    retfq
.gdt_reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
