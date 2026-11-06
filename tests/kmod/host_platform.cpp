// tests/kmod/host_platform.cpp — kmod_platform.h 的宿主实现
// ----------------------------------------------------------------------------
// 内核实现见 kernel/src/arch/x86_64/kmod/platform_kernel.cpp（不参与宿主构建）。
// 这里用 libc + C++ 标准库提供同一组原语，使 elf_loader / topology / capability
// / governance / seat / syscall_registry 的真实源码可以在宿主上被 ASAN 测试。
//
// 关键点：kmod_alloc 用 mmap(PROT_READ|PROT_WRITE|PROT_EXEC) 分配——加载后的
// 模块镜像要在宿主上直接执行（fixture 函数指针），普通 malloc 的 NX 页面会段错误。
// 分配块表同时用于"KmodElfFreeImage 后无泄漏"的精确断言（ASAN/LSAN 是第二道保险）。
//
// 额外职责（因为宿主编译不包含 kmod_core.cpp / platform_kernel.cpp）：
//   * 为 kmod_internal.h 里"定义在 kmod_core.cpp"的全局注册表提供 weak 兜底
//     （kmod_pool.cpp 提供 g_kmod_images/计数强定义，weak 会被覆盖）；
//   * 为 kernel_exports.cpp 缺失时的 KmodKernelExports 提供 weak 空表兜底。
// 这些兜底只在宿主测试链接里生效，不改变内核源码。
// ----------------------------------------------------------------------------
#include <kmod/kmod_platform.h>
#include <kmod/kmod_internal.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <sys/mman.h>

#include "host_platform.h"

// ============================================================================
// 内建组件描述表：链接脚本 __kbuiltin_desc_start/__kbuiltin_desc_end 的宿主替身
// ----------------------------------------------------------------------------
// builtin.cpp 直接用这两个符号之差 / sizeof(kmod_builtin_raw_t) 得到描述符数量。
// 宿主在自定义段 .hostbuiltin.desc 里放恰好 1 个槽位（48 字节），start 指向槽位
// 开头、end 指向槽位末尾；测试通过 kmod_host_builtin_set_raw() 写入/清空。
// ============================================================================
static_assert(sizeof(kmod_builtin_raw_t) == 48,
              "kmod_builtin_raw_t ABI freeze (host builtin table)");

__asm__(
    ".pushsection .hostbuiltin.desc,\"aw\",@progbits\n"
    ".balign 8\n"
    ".globl __kbuiltin_desc_start\n"
    "__kbuiltin_desc_start:\n"
    ".space 48\n"
    ".globl __kbuiltin_desc_end\n"
    "__kbuiltin_desc_end:\n"
    ".popsection\n");

extern "C" const kmod_builtin_raw_t __kbuiltin_desc_start[];

void kmod_host_builtin_set_raw(const kmod_builtin_raw_t *raw)
{
    kmod_builtin_raw_t *slot =
        const_cast<kmod_builtin_raw_t *>(__kbuiltin_desc_start);
    if (raw)
        *slot = *raw;
    else
        std::memset(slot, 0, sizeof(*slot));
}

uint32_t kmod_host_builtin_slots(void)
{
    return 1u;
}

// ============================================================================
// 分配记账：mmap RWX / munmap，外加存活块表供泄漏断言
// ============================================================================
namespace {

std::mutex g_alloc_lock;
std::unordered_map<void *, uint64_t> g_live_allocs;
uint64_t g_live_bytes = 0;
uint64_t g_total_allocs = 0;

uint64_t lock_slot(volatile int32_t *lock)
{
    // 按地址散列到 8 个 std::mutex（规格要求的做法）；宿主单线程为主，
    // 这里只保证多线程混沌测试不死锁（8 桶冲突时退化为串行，语义仍正确）。
    uintptr_t a = reinterpret_cast<uintptr_t>(lock);
    return ((a >> 4) ^ (a >> 12)) & 7u;
}

std::mutex g_locks[8];

} // namespace

extern "C" {

void *kmod_alloc(uint64_t size)
{
    if (size == 0)
        return nullptr;
    size_t len = static_cast<size_t>(size);
    void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    // mmap 返回页对齐（>=16），满足 ABI；内核镜像在宿主上被直接执行。
    {
        std::lock_guard<std::mutex> g(g_alloc_lock);
        g_live_allocs[p] = size;
        g_live_bytes += size;
        g_total_allocs++;
    }
    return p;
}

void kmod_free(void *ptr, uint64_t size)
{
    (void)size;
    if (!ptr)
        return;
    uint64_t len = 0;
    {
        std::lock_guard<std::mutex> g(g_alloc_lock);
        auto it = g_live_allocs.find(ptr);
        if (it != g_live_allocs.end()) {
            g_live_bytes -= it->second;
            len = it->second;
            g_live_allocs.erase(it);
        }
    }
    if (len != 0) {
        munmap(ptr, static_cast<size_t>(len));
    } else {
        // 非本分配器返回的指针：宿主侧只可能是测试误用，明确报错而不是静默泄漏
        std::fprintf(stderr, "[kmod-host] kmod_free(%p): unknown block\n", ptr);
    }
}

void kmod_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

uint64_t kmod_uptime_ms(void)
{
    static const auto t0 = std::chrono::steady_clock::now();
    auto d = std::chrono::steady_clock::now() - t0;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

void kmod_wait_ms(uint32_t ms)
{
    if (ms == 0) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint64_t kmod_lock_acquire(volatile int32_t *lock)
{
    g_locks[lock_slot(lock)].lock();
    return 0; // 宿主无中断标志
}

void kmod_lock_release(volatile int32_t *lock, uint64_t flags)
{
    (void)flags;
    g_locks[lock_slot(lock)].unlock();
}

uint32_t kmod_cpu_id(void)
{
    return 0;
}

} // extern "C"

// ============================================================================
// 测试侧记账钩子
// ============================================================================
uint64_t kmod_host_live_alloc_count(void)
{
    std::lock_guard<std::mutex> g(g_alloc_lock);
    return static_cast<uint64_t>(g_live_allocs.size());
}

uint64_t kmod_host_live_alloc_bytes(void)
{
    std::lock_guard<std::mutex> g(g_alloc_lock);
    return g_live_bytes;
}

uint64_t kmod_host_total_alloc_count(void)
{
    std::lock_guard<std::mutex> g(g_alloc_lock);
    return g_total_allocs;
}

void kmod_host_alloc_reset_stats(void)
{
    std::lock_guard<std::mutex> g(g_alloc_lock);
    g_total_allocs = static_cast<uint64_t>(g_live_allocs.size());
}

// ============================================================================
// kmod_core.cpp 全局的弱兜底（真实内核构建由 kmod_core.cpp 强定义覆盖；
// 宿主构建里 g_kmod_images / g_kmod_image_count 由 kmod_pool.cpp 强定义）
// ============================================================================
extern "C" {

__attribute__((weak)) volatile int32_t g_kmod_lock = 0;
__attribute__((weak)) kmod_stats_t g_kmod_stats;
__attribute__((weak)) kmod_image_t g_kmod_images[KMOD_MAX_IMAGES];
__attribute__((weak)) uint32_t g_kmod_image_count = 0;
__attribute__((weak)) kmod_module_t g_kmod_modules[KMOD_MAX_MODULES];
__attribute__((weak)) uint32_t g_kmod_module_count = 0;

// kernel_exports.cpp 尚未落地（或宿主分支未提供）时的空表兜底：
// elf_loader 只按传入表解析，governance/seat/syscall 若调用本函数也能链接。
__attribute__((weak)) void KmodKernelExports(const kmod_kernel_export_t **out,
                                             uint32_t *count)
{
    if (out)
        *out = nullptr;
    if (count)
        *count = 0;
}

} // extern "C"
