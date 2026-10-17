//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* hw: 启动期自检程序 (boot-time smoke test)。
 *
 * 它是内核在桌面之后启动的第一个用户态进程, 所以这里顺带做一次
 * "系统调用返回值链路"体检: 逐个 syscall 单独接返回值, 打印出来。
 * (曾经发生过 syscall 返回值被 %rax 覆盖而全部变成 0 的事故 —— 桌面因此
 *  拿到全 0 的帧缓冲几何信息并自杀; 见 docs/debug-hang-2026-10-11.md 第 11 节。
 *  在这里留一行体检输出, 以后再出同类问题一眼可见。)
 */
#include <stdio.h>
#include <stdint.h>
#include <syscall.h>

#define SYS_GETPID_NR   8
#define SYS_GETTID_NR   7

/* ============================================================================
 * TLS 探针 (动态链接调试用)
 * ----------------------------------------------------------------------------
 * 注意 (踩过的坑): 这里**不能**写死 `%fs:0x2feb8` 这种偏移 —— 那是 libsky.so
 * 里 tls_data 的偏移, 只对动态链接的进程成立。静态程序 (hw.elf 自己) 的 TLS
 * 块只有它自己的 PT_TLS (32 字节), 写死偏移会直接读飞:
 *   Segfault: unmapped addr=0x7FA826816268, %fs=0x7FA8267E63B0
 *   差值正好 = 0x2feb8  <- 一眼看出是探针自己干的
 * 所以改成通过**真实 TLS 变量**取值, 让编译器和链接器算出正确偏移。
 * ==========================================================================*/
static __thread int probe_tls_var = -1;   /* 初值 -1, 便于判断"读到初值了" */

static void tls_probe(void) {
    uint64_t fs = 0;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(fs));

    /* 编译器会为这两行生成正确的 TLS 访问 (静态程序是 local-exec,
       动态程序是 GOT/TPOFF), 偏移由链接器决定, 不写死。 */
    int v = probe_tls_var;
    probe_tls_var = 0x5A5A;
    int v2 = probe_tls_var;

    char b[224];
    int n = snprintf(b, sizeof(b),
        "[hw] tls probe: fs=%llx tcb_self=%llx var_init=%d var_after=%d\n",
        (unsigned long long)fs, (unsigned long long)fs,
        (int)v, (int)v2);
    if (n > 0) syscall(SYS_DBGSOUT, (long)b, (long)n, 0, 0, 0, 0);
}

int main(void) {
    const char *msg = "Hello, World!";
    syscall(SYS_DBGSOUT, (long)msg, 16, 0, 0, 0, 0);

    tls_probe();

    /* 逐条 syscall, 每条单独接返回值 */
    uint64_t pid = syscall(SYS_GETPID_NR, 0, 0, 0, 0, 0, 0);
    uint64_t tid = syscall(SYS_GETTID_NR, 0, 0, 0, 0, 0, 0);

    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "[hw] syscall return check: pid=%llu tid=%llu (both must be non-zero)\n",
                     (unsigned long long)pid, (unsigned long long)tid);
    if (n > 0) syscall(SYS_DBGSOUT, (long)buf, (long)n, 0, 0, 0, 0);

    syscall(SYS_EXIT, 0, 0, 0, 0, 0, 0); // Exit
}
