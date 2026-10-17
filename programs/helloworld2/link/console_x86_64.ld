OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

PHDRS
{
    phdr_rw      PT_LOAD FLAGS(6); /* RW */
    phdr_text    PT_LOAD FLAGS(5); /* RX */
    phdr_rodata  PT_LOAD FLAGS(4); /* RO */
    phdr_data    PT_LOAD FLAGS(6); /* RW */
    phdr_tls     PT_TLS;           /* TLS 段程序头 */
    phdr_dynamic PT_DYNAMIC;       /* 没有它, 内核(和 readelf -d)看不到 DT_NEEDED */
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
    /* 动态链接所需的元数据**必须并进同一个 .rodata 段**。
       原先我把 .dynsym/.dynstr/.dynamic/.rela.* 各自作为独立输出段并挂到
       phdr_rodata, 结果 ld 生成了**相互重叠**的 PT_LOAD:
         seg2 (RX) 0x401000 filesz=0x3060  → 覆盖到 0x404060
         seg3 (R)  0x402000 filesz=0x3040  → 起点落在 seg2 内部
       重叠本身能加载 (权限收紧已改成第二趟), 但内核的 dl_vaddr_to_off()
       是按"第一个包含该 VA 的 PT_LOAD"反查文件偏移的, 于是 DT_STRTAB
       (0x4032xx) 被算成 seg2 的偏移, 读到的字符串是垃圾 -> dyn_read_needed()
       找不到 DT_NEEDED -> elf_dyn_apply() 提前返回 -> PLT 全是野地址。
       把元数据和 .rodata 放在同一个输出段里, 段就不再重叠, 反查结果唯一。 */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.hash)
        *(.hash)
        *(.rela.init)
        *(.rela.text)
        *(.rela.text.*)
        *(.rela.rodata)
        *(.rela.rodata.*)
        *(.rela.data)
        *(.rela.data.*)
        *(.rela.got)
        *(.rela.dyn)
        *(.rela.plt)
        *(.rela.*)
    } :phdr_rodata

    /* .dynamic 是 DT_NEEDED 的唯一载体, 必须挂到 PT_DYNAMIC 上 */
    .dynamic : {
        *(.dynamic)
    } :phdr_dynamic :phdr_rodata

    /* .plt 必须可执行; 放在 .text 段内, 避免另起一个重叠段 */
    .plt : {
        *(.plt)
        *(.plt.*)
        *(.iplt)
    } :phdr_text

    . = ALIGN(0x1000);
    .data : {
        *(.data)
        *(.data.*)
    } :phdr_data

    /* GOT 必须可写, 且必须落进某个 PT_LOAD */
    .got : {
        *(.got)
        *(.got.plt)
        *(.igot)
        *(.igot.plt)
        *(.igot.*)
    } :phdr_data :phdr_rodata

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