SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .stub .text.* .gnu.linkonce.t.*)
    }

    /* GUI Console Identify */
    .GUI_C : ALIGN(4)
    {
        KEEP(*(.GUI_C))          /* .GUI_C Section */
        KEEP(*(.GUI_C.*))        /* COMPAT .GUI_C.xxx */
        . = ALIGN(4);
    } = 0x90                     /* FULL NOP INSTRUCT */

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    }
    .data : { *(.data .data.*) }
    .bss  : { *(.bss .bss.*) }
}