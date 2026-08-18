OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

PHDRS
{
    phdr_rw     PT_LOAD FLAGS(6); /* RW */
    phdr_text   PT_LOAD FLAGS(5); /* RX */
    phdr_rodata PT_LOAD FLAGS(4); /* RO */
    phdr_data   PT_LOAD FLAGS(6); /* RW */
    phdr_tls    PT_TLS;           /* TLS 段程序头 */
}

SECTIONS
{
    . = 0x400000;
    
    .prepad : {
        FILL(0);
        . = . + 0xFFF;
        BYTE(0);
    } :phdr_rw

    . = ALIGN(0x1000);

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

    /* 新增：TLS 段 */
    .tdata : {
        *(.tdata)
        *(.tdata.*)
    } :phdr_tls :phdr_data

    .tbss : {
        *(.tbss)
        *(.tbss.*)
    } :phdr_tls :phdr_data

    .bss : {
        *(.bss)
    } :phdr_data
}