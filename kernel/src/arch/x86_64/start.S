.section .text
.global _start
.type _start, @function
_start:
    /* 显式引导栈：不依赖 bootloader 提供的栈。
       组件化启动（KmodBootComponents 拓扑：ACPI->APIC->...->PCI 枚举->FS->
       网络->用户态）比旧的硬编码 init 链调用深度更大，叠加 kprintf/flanterm
       输出路径后必须给 Tier0 一个确定大小的栈，否则会在不同组件处随机
       "异常处理再入 kprintf 自死锁 / 栈静默踩坏"。64 KiB 只用于 bootstrap
       阶段，调度器上线后各线程使用自己的内核栈。 */
    lea     __boot_stack_top(%rip), %rsp
    and     $-16, %rsp

    /* 检查 RDRAND: CPUID.01H:ECX[bit 30] */
    mov     $1, %eax
    xor     %ecx, %ecx
    cpuid
    test    $0x40000000, %ecx       /* ECX bit 30 = RDRAND */
    jz      .Lfallback

    /* RDRAND 重试 10 次 */
    mov     $10, %ecx
.Lrdrand_retry:
    rdrand  %rax
    jc      .Lgot_random
    dec     %ecx
    jnz     .Lrdrand_retry
    jmp     .Lfallback              /* 重试耗尽, 走回退 */

.Lno_rdrand:
.Lfallback:
    /* 回退: TSC + 常量异异 (movabs + 寄存器 xor) */
    rdtsc
    shl     $32, %rdx
    or      %rdx, %rax
    movabs  $0xDEADBEEFCAFEBABE, %rcx
    xor     %rcx, %rax
    /* TSC 回退值可能为 0, 检查 */
    test    %rax, %rax
    jnz     .Lstore
    /* TSC 也给了 0, 用静态值 */
    movabs  $0x0043414E41525900, %rax    /* "\0RANAC\0" */
    jmp     .Lstore

.Lgot_random:
    /* RDRAND 成功, 检查是否为 0 */
    test    %rax, %rax
    jz      .Lfallback              /* RDRAND 返回 0, 走回退 */

.Lstore:
    /* LSB 清零: and $-256 等价于 and $0xFFFFFFFFFFFFFF00
       (-256 的 32 位补码符号扩展后正好是 0xFFFFFFFFFFFFFF00) */
    and     $-256, %rax
    /* 写入 canary (RIP-relative) */
    lea     __stack_chk_guard(%rip), %rdx
    mov     %rax, (%rdx)
    /* 调用 kmain */
    call    kmain
    /* 不应返回 */
    cli
.Lhang:
    hlt
    jmp     .Lhang
.size _start, . - _start

/* Tier0 bootstrap 栈（.bss，64 KiB，16 字节对齐）。Limine 跳转 _start 时
   不保证栈大小；组件化启动链路比旧硬编码 init 链更深（拓扑图的 ACPI/APIC/
   PCI/FS/网络组件 init + kprintf/flanterm 输出路径），需要确定大小的栈。
   调度器上线后各线程使用自己的内核栈，本栈只在 bootstrap 阶段使用。 */
.section .bss
.align 16
.global __boot_stack
__boot_stack:
    .skip 65536
.global __boot_stack_top
__boot_stack_top: