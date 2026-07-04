[global gdt_reload_seg]
gdt_reload_seg:
    push 0x08
    lea rax, [rel .gdt_reload_cs]
    push rax
    retfq
.gdt_reload_cs:
    ; 64位下数据段基址和界限被忽略，但显式设为0是最佳实践
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax     ; FS/GS 基址由 MSR 控制，这里只清空选择子
    mov gs, ax
    mov ax, 0x10   ; SS 必须设为有效的数据段选择子
    mov ss, ax
    ret