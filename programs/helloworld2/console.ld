OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start) /* 或者你的入口 */

/* 1. 显式定义程序头模板 */
PHDRS
{
    phdr_text  PT_LOAD FLAGS(5); /* R+X = 5 */
    phdr_rodata PT_LOAD FLAGS(4); /* R = 4 */
    phdr_data  PT_LOAD FLAGS(6); /* R+W = 6 */
}

SECTIONS
{
    . = 0x400000;
    
    /* 2. 将段绑定到对应的程序头 */
    .text : {
        *(.text)
        KEEP(*(.GUI_C))
    } :phdr_text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata)
    } :phdr_rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data)
    } :phdr_data

    .bss : {
        *(.bss)
    } :phdr_data
}