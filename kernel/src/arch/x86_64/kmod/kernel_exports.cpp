//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel_exports.cpp —— Tier0 内核导出表（.kout 段）
 * ============================================================================
 * 职责：把内核**固定不可模块化**的少量服务做成模块可导入的符号表，供
 *   elf_loader 在解析 .ko 的 .in/直接外部引用时按 (name) 回填绝对地址。
 * 设计约束（对应 docs/kernel-modules.md 原则）：
 *   - 只导出 Tier0 契约：内存(kernel.mem.v1)、日志(kernel.log.v1)、
 *     epoch(kernel.epoch.v1)、编译器 mem* 兜底(kernel.libc.v1)、
 *     单调毫秒(kernel.time.v1)。不导出任何业务语义接口。
 *   - 导出项定义在 .kout 段（KMOD_SEC_KERNEL_OUT）。PIE/KASLR 构建下该段
 *     由 link script 放进可写 data 段，装载期由动态重定位回填指针，因此
 *     这里用**非 const** 全局（不能用 const 数组放进 .rodata）。
 *   - 宿主测试（-D__KERNEL_TEST_HOST__）不引用 __kout_start/__kout_end，
 *     改用同内容的静态数组；宿主表里必需的 kprintf_/printf_/mem* 由
 *     本文件的 libc 版本提供，保证 elf_loader fixture 可独立链接。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#ifdef __KERNEL_TEST_HOST__
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#else
#include <stdarg.h>
#include <klib/kprintf.h>
#include <mem/heap.h>
#endif

/* kmod_abi.h 目前只为 mem/log/sync/epoch/syscall 五个内核契约提供了宏；
 * libc/time 契约按任务约定使用字面量（如后续头文件补宏，这里直接替换即可）。 */
#define KMOD_CONTRACT_KERNEL_LIBC_V1 "kernel.libc.v1"
#define KMOD_CONTRACT_KERNEL_TIME_V1 "kernel.time.v1"

/* 布局冻结自检：kmod_export_t 必须 9 字段 / 48 字节（模块与内核同头编译，
 * 这里再钉一次，防止有人给结构体加字段而忘了 ABI 指纹升级）。 */
static_assert(sizeof(kmod_export_t) == 48u,
              "kmod_export_t layout must stay 9 fields / 48 bytes");

/* ================================================================== */
/* KmodLog：Tier0 日志出口的变参包装（模块可直接导入）                */
/* ================================================================== */
extern "C" void KmodLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef __KERNEL_TEST_HOST__
    std::vfprintf(stdout, fmt, ap);
#else
    vprintf_(fmt, ap);
#endif
    va_end(ap);
}

/* ================================================================== */
/* KmodNowMs：单调毫秒（kmod_uptime_ms 的稳定包装）                    */
/* ================================================================== */
extern "C" uint64_t KmodNowMs(void)
{
    return kmod_uptime_ms();
}

/* ================================================================== */
/* 内核分支：.kout 段内的真实导出项（地址=内核符号，装载期重定位）     */
/* ================================================================== */
#ifndef __KERNEL_TEST_HOST__

extern "C" {
/* main.cpp 提供的最小 libc（标准参数序）。.ko 由 clang 以 -ffreestanding
 * 编译时仍可能为结构体拷贝/清零生成 memcpy/memset 外部引用，这里作为
 * kernel.libc.v1 兜底导出；memmove/memcmp 用于模块源码的直接调用。 */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
}

/* link script 集成点（Lead）：在 .data 内
 *   .kout : { __kout_start = .; KEEP(*(.kout)) __kout_end = .; }
 * KEEP 必须写：内核链接用 --gc-sections，段内对象只有经这两个标记被引用。 */
extern "C" kmod_export_t __kout_start[];
extern "C" kmod_export_t __kout_end[];

/* 内核里 "kprintf_" 只是宏 kprintf 的展开（符号真名是 printf_），而 .ko 的
 * KMOD_IMPORT_FN 按字符串 "kprintf_" 查导出表 —— 所以这里需要"导出名 !=
 * 函数符号名"的别名导出，header 的 KMOD_KEXPORT 不适用，用等价局部宏。 */
#define KMOD_KEXPORT_AS(export_name_, sym_, contract_, abi_, kind_)            \
    __attribute__((used, section(KMOD_SEC_KERNEL_OUT), aligned(8)))            \
    kmod_export_t __kmod_kexport_##export_name_ = {                           \
        #export_name_, contract_, (void *)&sym_, 0u, (uint32_t)(abi_),         \
        kind_, 0u, 0u, 0u }

KMOD_KEXPORT(kmalloc, KMOD_CONTRACT_KERNEL_MEM, 1u, KMOD_SYM_FUNC);
KMOD_KEXPORT(kfree, KMOD_CONTRACT_KERNEL_MEM, 1u, KMOD_SYM_FUNC);

KMOD_KEXPORT_AS(kprintf_, printf_, KMOD_CONTRACT_KERNEL_LOG, 1u,
                KMOD_SYM_FUNC);
KMOD_KEXPORT(printf_, KMOD_CONTRACT_KERNEL_LOG, 1u, KMOD_SYM_FUNC);
KMOD_KEXPORT(KmodLog, KMOD_CONTRACT_KERNEL_LOG, 1u, KMOD_SYM_FUNC);

KMOD_KEXPORT(KmodEpochEnter, KMOD_CONTRACT_KERNEL_EPOCH, 1u, KMOD_SYM_FUNC);
KMOD_KEXPORT(KmodEpochExit, KMOD_CONTRACT_KERNEL_EPOCH, 1u, KMOD_SYM_FUNC);

KMOD_KEXPORT(memcpy, KMOD_CONTRACT_KERNEL_LIBC_V1, 1u, KMOD_SYM_FUNC);
KMOD_KEXPORT(memset, KMOD_CONTRACT_KERNEL_LIBC_V1, 1u, KMOD_SYM_FUNC);
KMOD_KEXPORT(memmove, KMOD_CONTRACT_KERNEL_LIBC_V1, 1u, KMOD_SYM_FUNC);
KMOD_KEXPORT(memcmp, KMOD_CONTRACT_KERNEL_LIBC_V1, 1u, KMOD_SYM_FUNC);

KMOD_KEXPORT(KmodNowMs, KMOD_CONTRACT_KERNEL_TIME_V1, 1u, KMOD_SYM_FUNC);

#endif /* !__KERNEL_TEST_HOST__ */

/* ================================================================== */
/* 宿主测试分支：同内容的静态表（不引用 __kout_start / 内核 klib）     */
/* ================================================================== */
#ifdef __KERNEL_TEST_HOST__

namespace {

void *host_kmalloc(uint64_t size)
{
    return std::malloc(static_cast<size_t>(size));
}

void host_kfree(void *ptr)
{
    std::free(ptr);
}

int host_printf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int32_t host_epoch_enter(const char *name)
{
    (void)name;
    return KMOD_OK;
}

void host_epoch_exit(const char *name)
{
    (void)name;
}

uint64_t host_now_ms(void)
{
    return kmod_uptime_ms();
}

void *host_memcpy(void *dst, const void *src, size_t n)
{
    std::memcpy(dst, src, n);
    return dst;
}

void *host_memset(void *dst, int c, size_t n)
{
    std::memset(dst, c, n);
    return dst;
}

void *host_memmove(void *dst, const void *src, size_t n)
{
    std::memmove(dst, src, n);
    return dst;
}

int host_memcmp(const void *a, const void *b, size_t n)
{
    return std::memcmp(a, b, n);
}

/* 与内核 .kout 内容一一对应；宿主上函数地址由宿主链接器静态重定位。 */
kmod_export_t g_host_kout[] = {
    KMOD_EXPORT_ENTRY("kmalloc", KMOD_CONTRACT_KERNEL_MEM, host_kmalloc, 0u,
                      1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("kfree", KMOD_CONTRACT_KERNEL_MEM, host_kfree, 0u, 1u,
                      KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("kprintf_", KMOD_CONTRACT_KERNEL_LOG, host_printf_, 0u,
                      1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("printf_", KMOD_CONTRACT_KERNEL_LOG, host_printf_, 0u,
                      1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("KmodLog", KMOD_CONTRACT_KERNEL_LOG, host_printf_, 0u,
                      1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("KmodEpochEnter", KMOD_CONTRACT_KERNEL_EPOCH,
                      host_epoch_enter, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("KmodEpochExit", KMOD_CONTRACT_KERNEL_EPOCH,
                      host_epoch_exit, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("memcpy", KMOD_CONTRACT_KERNEL_LIBC_V1, host_memcpy,
                      0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("memset", KMOD_CONTRACT_KERNEL_LIBC_V1, host_memset,
                      0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("memmove", KMOD_CONTRACT_KERNEL_LIBC_V1, host_memmove,
                      0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("memcmp", KMOD_CONTRACT_KERNEL_LIBC_V1, host_memcmp,
                      0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
    KMOD_EXPORT_ENTRY("KmodNowMs", KMOD_CONTRACT_KERNEL_TIME_V1, host_now_ms,
                      0u, 1u, KMOD_SYM_FUNC, 0u, 0u),
};

constexpr uint32_t kHostKoutCount =
    static_cast<uint32_t>(sizeof(g_host_kout) / sizeof(g_host_kout[0]));

} /* namespace */

#endif /* __KERNEL_TEST_HOST__ */

/* ================================================================== */
/* KmodKernelExports：把 .kout 段（kmod_export_t 9 字段）投影成加载器  */
/* 使用的精简 (name, addr) 视图。首次调用时构建一次（引导期单线程，        */
/* 之后只读；重复调用幂等）。                                          */
/* ================================================================== */
namespace {

kmod_kernel_export_t g_kexp_pairs[KMOD_MAX_EXPORTS];
uint32_t g_kexp_count = 0u;
bool g_kexp_ready = false;

/* 追加一段导出项（.kout 段或宿主静态表），保留完整元数据供 governance
 * 对 provider=kernel 的导入做 contract/version/size 机械校验。 */
void kexp_append(const kmod_export_t *entries, uint32_t n)
{
    if (!entries)
        return;
    for (uint32_t i = 0u; i < n; i++) {
        if (g_kexp_count >= KMOD_MAX_EXPORTS)
            break;
        kmod_kernel_export_t *e = &g_kexp_pairs[g_kexp_count];
        e->name = entries[i].symbol;
        e->addr = entries[i].addr;
        e->contract = entries[i].contract;
        e->abi_version = entries[i].abi_version;
        e->size = entries[i].size;
        e->kind = entries[i].kind;
        g_kexp_count++;
    }
}

void kexp_finish(void)
{
    g_kexp_ready = true;
    kmod_log("[kmod] kernel exports: %u symbol(s) published\n",
             (unsigned)g_kexp_count);
}

} /* namespace */

extern "C" void KmodKernelExports(const kmod_kernel_export_t **out,
                                  uint32_t *count)
{
    if (!g_kexp_ready) {
#ifdef __KERNEL_TEST_HOST__
        kexp_append(g_host_kout, kHostKoutCount);
        /* klib/sync/algo 共享服务导出项与内核 .kout 同源（kernel_services.cpp
         * 的 initializer；宿主分支用 std libc 实现）。 */
        const kmod_export_t *svc = nullptr;
        uint32_t nsvc = 0u;
        KmodKernelServiceExports(&svc, &nsvc);
        kexp_append(svc, nsvc);
#else
        uintptr_t begin = (uintptr_t)__kout_start;
        uintptr_t end = (uintptr_t)__kout_end;
        uint32_t n = 0u;
        if (end > begin)
            n = (uint32_t)((end - begin) / sizeof(kmod_export_t));
        kexp_append(__kout_start, n);
#endif
        kexp_finish();
    }
    if (out)
        *out = g_kexp_pairs;
    if (count)
        *count = g_kexp_count;
}
