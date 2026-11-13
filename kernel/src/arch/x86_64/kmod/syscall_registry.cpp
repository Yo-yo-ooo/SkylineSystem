//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_registry.cpp —— 固定 syscall 契约表 + 动态命名空间 + 治理哨兵
 * ============================================================================
 * 边界（docs/kernel-modules.md "syscall 治理"）：
 *   - 固定契约槽 0..63：编号/参数/返回/错误码/语义固定，Tier0 不变。
 *     其中 0..28 与 31 的实现由 Tier1 内建组件 (kernel.syscall.<family>)
 *     注册；29(SYSCALL_KMOD_INFO)、30(SYSCALL_KMOD_PROBE) 由 Tier0 治理层
 *     内部实现（30 标记 SWAPPABLE，允许模块在能力(KMOD_CAP_SYSCALL_OVERRIDE)
 *     约束下替换"实现"，语义不变）。32..63 保留未定义。
 *   - 128..511 是动态命名空间槽：模块声明各自命名空间 + 能力
 *     (KMOD_CAP_SYSCALL_IMPL) 才能注册实现；内核只做机械校验。
 *   - syscall.cpp 的固定 dispatch 在治理槽（0..63 + 128..511）上安装
 *     KmodSyscallSentinel() 指针；哨兵本身是一个可调用的 trampoline
 *     （签名与 dispatch 表一致），因此 dispatch 走普通调用路径即可抵达
 *     KmodSyscallEntry(frame)，也便于和 KmodSyscallSentinel() 做指针比较。
 *   - 内建组件（KMOD_MODF_BUILTIN）可注册 FIXED 槽实现（Tier0 构建产物
 *     可信）；.ko 只能注册动态槽或覆写 SWAPPABLE 固定槽。
 *   - 模块实现调用期间持有 epoch（KmodEpochEnterInternal/ExitInternal），
 *     热替换/卸载必须先等 epoch 排空；模块非 READY 返回 -EAGAIN。
 *     例外：exit(9) 固定语义不返回，不做 epoch 包裹（避免计数泄漏）。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#ifdef __KERNEL_TEST_HOST__
#include <cstring>
#else
#include <errno.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/cpu/smap.h>

/* 内核最小 memcpy（klib/klib.h 提供，标准参数序）。这里只做本地声明，
   避免把 klib 巨型头引入 syscall 治理层。 */
extern "C" void *__memcpy(void *d, const void *s, uint64_t n);

/* syscall.cpp 的固定 dispatch 表（Tier0 全局；定义在 schedule/syscall.cpp）。
   类型与 syscall.cpp 中的定义逐字段一致。 */
extern uint64_t (*syscall_lists[KMOD_SYSCALL_MAX])(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    syscall_frame_t *);
#endif

/* 用户可见错误码（固定语义，不能换成 kmod 内部错误码） */
#ifdef __KERNEL_TEST_HOST__
#define KMOD_SC_ENOSYS (-38)
#define KMOD_SC_EAGAIN (-11)
#define KMOD_SC_EINVAL (-22)
#else
#define KMOD_SC_ENOSYS (-ENOSYS)
#define KMOD_SC_EAGAIN (-EAGAIN)
#define KMOD_SC_EINVAL (-EINVAL)
#endif

namespace {

/* ------------------------------------------------------------------ */
/* 固定契约表（0..31，共 32 条；22=mkdir, 23=readdir 均已启用）      */
/* ------------------------------------------------------------------ */
constexpr uint32_t kFixedContractCount = 32u;

const kmod_syscall_contract_t g_contracts[kFixedContractCount] = {
    /* nr, flags, abi, name, owner */
    {0u,  KMOD_SC_FIXED, 1u, "fopen", "kernel"},
    {1u,  KMOD_SC_FIXED, 1u, "fwrite", "kernel"},
    {2u,  KMOD_SC_FIXED, 1u, "fread", "kernel"},
    {3u,  KMOD_SC_FIXED, 1u, "fclose", "kernel"},
    {4u,  KMOD_SC_FIXED, 1u, "flseek", "kernel"},
    {5u,  KMOD_SC_FIXED, 1u, "fsize", "kernel"},
    {6u,  KMOD_SC_FIXED, 1u, "thread_launch", "kernel"},
    {7u,  KMOD_SC_FIXED, 1u, "gettid", "kernel"},
    {8u,  KMOD_SC_FIXED, 1u, "getpid", "kernel"},
    {9u,  KMOD_SC_FIXED, 1u, "exit", "kernel"},
    {10u, KMOD_SC_FIXED, 1u, "pmmap", "kernel"},
    {11u, KMOD_SC_FIXED, 1u, "yield", "kernel"},
    {12u, KMOD_SC_FIXED, 1u, "load", "kernel"},
    {13u, KMOD_SC_FIXED, 1u, "launch", "kernel"},
    {14u, KMOD_SC_FIXED, 1u, "mmap", "kernel"},
    {15u, KMOD_SC_FIXED, 1u, "munmap", "kernel"},
    {16u, KMOD_SC_FIXED, 1u, "sysinfo", "kernel"},
    {17u, KMOD_SC_FIXED, 1u, "time", "kernel"},
    {18u, KMOD_SC_FIXED, 1u, "arch_prctl", "kernel"},
    {19u, KMOD_SC_FIXED, 1u, "kill", "kernel"},
    {20u, KMOD_SC_FIXED, 1u, "getrandom", "kernel"},
    {21u, KMOD_SC_FIXED, 1u, "dev_mmap", "kernel"},
    {22u, KMOD_SC_FIXED, 1u, "mkdir", "kernel"},
    {23u, KMOD_SC_FIXED, 1u, "readdir", "kernel"},
    {24u, KMOD_SC_FIXED, 1u, "dbgsout", "kernel"},
    {25u, KMOD_SC_FIXED, 1u, "dev_getinfo", "kernel"},
    {26u, KMOD_SC_FIXED, 1u, "dev_ioctl", "kernel"},
    {27u, KMOD_SC_FIXED, 1u, "monotonic_ms", "kernel"},
    {28u, KMOD_SC_FIXED, 1u, "sleep_ms", "kernel"},
    {KMOD_SYSCALL_INFO, KMOD_SC_FIXED | KMOD_SC_KMOD, 1u, "kmod_info",
     "kernel"},
    {KMOD_SYSCALL_PROBE, KMOD_SC_FIXED | KMOD_SC_SWAPPABLE, 1u, "kmod_probe",
     "kernel"},
    /* 31: 子进程退出事件（固定语义；实现由内建组件 kernel.syscall.proc 注册） */
    {31u, KMOD_SC_FIXED, 1u, "child_event", "kernel"},
};

/* ------------------------------------------------------------------ */
/* 治理槽表（512 槽）：模块实现 / Tier0 默认实现 / 哨兵分派             */
/* ------------------------------------------------------------------ */
kmod_syscall_slot_t g_slots[KMOD_SYSCALL_MAX];
kmod_syscall_fn g_slot_defaults[KMOD_SYSCALL_MAX];
volatile int32_t g_sc_lock = 0;
uint32_t g_governed_count = 0u;
uint64_t g_ns_audit_count = 0u;

/* ---- 帧布局适配（rax = 第 15 个 u64 / offset 112） ---- */
#ifdef __KERNEL_TEST_HOST__
struct KmodTestFrame {
    uint64_t r[24];
};
static inline uint64_t frame_nr(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[14];
}
static inline uint64_t frame_rdi(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[8];
}
static inline uint64_t frame_rsi(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[9];
}
static inline uint64_t frame_rdx(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[12];
}
static inline uint64_t frame_r10(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[5];
}
static inline uint64_t frame_r8(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[7];
}
static inline uint64_t frame_r9(const void *frame)
{
    return reinterpret_cast<const KmodTestFrame *>(frame)->r[6];
}
#else
static inline uint64_t frame_nr(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->rax;
}
static inline uint64_t frame_rdi(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->rdi;
}
static inline uint64_t frame_rsi(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->rsi;
}
static inline uint64_t frame_rdx(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->rdx;
}
static inline uint64_t frame_r10(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->r10;
}
static inline uint64_t frame_r8(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->r8;
}
static inline uint64_t frame_r9(const void *frame)
{
    return reinterpret_cast<const syscall_frame_t *>(frame)->r9;
}
#endif

static inline uint64_t sc_err(int32_t e)
{
    return (uint64_t)(int64_t)e;
}

/* 组件化阶段：固定契约槽 0..63 全部纳入治理（实现由内建组件注册；
 * 29 INFO / 30 PROBE 仍由 Tier0 syscall_registry 内部分派）。
 * 64..127 是 .ko 不可用的空洞，syscall_lists 保持 NULL -> syscall_handler
 * 直接 -ENOSYS；128..511 动态命名空间槽同样走治理哨兵。 */
static inline bool slot_governed(uint32_t nr)
{
    return nr < KMOD_SYSCALL_CORE_MAX ||
           (nr >= KMOD_SYSCALL_DYNAMIC_MIN && nr <= KMOD_SYSCALL_DYNAMIC_MAX);
}

/* 固定语义：slot 9 = exit 不返回（Schedule::Exit 直接切走线程），
 * epoch 无法配对退出；若进入 epoch 会造成计数永久泄漏。Tier0 知道该固定
 * 语义，因此这一槽的实现直接调用、不做 epoch 包裹。 */
constexpr uint32_t kSlotExit = 9u;

static inline uint16_t slot_initial_flags(uint32_t nr)
{
    if (nr == KMOD_SYSCALL_INFO)
        return (uint16_t)(KMOD_SC_FIXED | KMOD_SC_KMOD);
    if (nr == KMOD_SYSCALL_PROBE)
        return (uint16_t)(KMOD_SC_FIXED | KMOD_SC_SWAPPABLE);
    if (nr < KMOD_SYSCALL_CORE_MAX)
        return (uint16_t)KMOD_SC_FIXED;
    if (nr >= KMOD_SYSCALL_DYNAMIC_MIN)
        return (uint16_t)KMOD_SC_NAMESPACED;
    return 0u;
}

/* ---- Tier0 内建 syscall 实现（固定契约，模块不可替换 29，30 默认） ---- */
uint64_t sc_info_impl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, void *frame)
{
    (void)a3;
    (void)a4;
    (void)a5;
    (void)frame;
    const int32_t rc = KmodSyscallCopyInfoToUser(
        reinterpret_cast<void *>(static_cast<uintptr_t>(a0)), a1, a2);
    if (rc < 0) {
        /* syscall 固定契约的错误码口径是 POSIX errno（用户态可见）；
         * KMOD_ERR_INVAL(-1) 不能直接透传，必须映射成 -EINVAL(22)。 */
        return sc_err(KMOD_SC_EINVAL);
    }
    return (uint64_t)(uint32_t)rc; /* 返回写入的记录数 */
}

uint64_t sc_probe_impl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, void *frame)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)frame;
    return KmodSyscallProbeValue();
}

/* ---- 字符串/记录快照辅助（不依赖 libc） ---- */
void copy_str_fixed(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;
    if (src) {
        while (i + 1u < cap && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void fill_record(kmod_sysinfo_record_t *rec, const kmod_module_t *m)
{
    copy_str_fixed(rec->name, KMOD_NAME_MAX, m->name);
    copy_str_fixed(rec->version, KMOD_VERSION_MAX, m->version);
    rec->tier = m->tier;
    rec->state = m->state;
    rec->flags = m->flags;
    rec->caps_granted = m->caps_granted;
    rec->caps_denied = m->caps_denied;
    rec->refcount = m->refcount;
    rec->epoch_active =
        __atomic_load_n(&m->epoch_active, __ATOMIC_RELAXED);
    rec->n_imports = m->n_imports;
    rec->n_exports = m->n_exports;
    rec->n_deps = m->n_deps;
    rec->n_consumers = m->n_consumers;
}

/* 用户态缓冲写入：内核用 SMAP override + __memcpy，宿主用 libc memcpy */
void user_copy(void *dst, const void *src, uint64_t n)
{
#ifdef __KERNEL_TEST_HOST__
    std::memcpy(dst, src, static_cast<size_t>(n));
#else
    SmapGuard guard;
    __memcpy(dst, src, n);
#endif
}

/* 模块指针 -> g_kmod_modules[] 下标（热替换/回滚按 module 下标清理槽位） */
int32_t module_index(const kmod_module_t *m)
{
    if (!m)
        return -1;
    for (uint32_t i = 0; i < KMOD_MAX_MODULES; i++) {
        if (&g_kmod_modules[i] == m)
            return (int32_t)i;
    }
    return -1;
}

/* ---- 注册撤销栈 ---- */
struct ScUndo {
    uint16_t nr;
    int32_t module;
    kmod_syscall_fn fn;
    uint32_t generation;
};

/* 单个 SYSCALL 导出安装；成功返回 KMOD_OK 并把旧状态压入撤销栈。
 * 规则（组件化阶段）：
 *   - 内建组件（KMOD_MODF_BUILTIN，Tier0 构建产物、可信）：可注册 FIXED 槽
 *     0..63（29/30 除外，它们是 Tier0 自省/探针实现），不要求 SWAPPABLE/
 *     能力位；槽必须为空（module<0 且 fn==null）。
 *   - .ko：只能注册 NAMESPACED 动态槽（CAP_SYSCALL_IMPL），或对 SWAPPABLE
 *     固定槽做覆写（CAP_SYSCALL_OVERRIDE）；其余固定槽一律 POLICY。
 *   - 槽重复注册返回 EXISTS。 */
int32_t install_one(kmod_module_t *m, int32_t idx, bool builtin,
                    const kmod_export_t *e, ScUndo *undo, uint32_t *n_undo,
                    bool *is_fixed, bool *is_builtin_fixed)
{
    const uint32_t nr = e->aux;
    if (nr >= KMOD_SYSCALL_MAX)
        return KMOD_ERR_INVAL;
    if (!slot_governed(nr))
        return KMOD_ERR_POLICY;
    kmod_syscall_fn fn = reinterpret_cast<kmod_syscall_fn>(e->addr);
    if (!fn)
        return KMOD_ERR_INVAL;

    *is_fixed = false;
    *is_builtin_fixed = false;
    kmod_syscall_slot_t *slot = &g_slots[nr];
    if (nr < KMOD_SYSCALL_CORE_MAX) {
        *is_fixed = true;
        *is_builtin_fixed = builtin;
        /* 29 (SYSCALL_KMOD_INFO) 是 Tier0 治理自省槽：任何模块不得替换。
         * 30 (SYSCALL_KMOD_PROBE) 的默认实现由 Tier0 内部分派；组件不得注册，
         * 但 .ko 可凭 CAP_SYSCALL_OVERRIDE 做 SWAPPABLE 覆写（语义固定）。 */
        if (nr == KMOD_SYSCALL_INFO)
            return KMOD_ERR_POLICY;
        if (nr == KMOD_SYSCALL_PROBE && builtin)
            return KMOD_ERR_POLICY;
        const kmod_syscall_contract_t *c = KmodSyscallContract(nr);
        if (!c)
            return KMOD_ERR_POLICY; /* 31..63 未定义的固定槽 */
        if (!builtin) {
            if ((c->flags & KMOD_SC_SWAPPABLE) == 0u)
                return KMOD_ERR_POLICY;
            if ((m->caps_granted & KMOD_CAP_SYSCALL_OVERRIDE) == 0ull)
                return KMOD_ERR_CAP;
        }
    } else {
        /* 动态命名空间槽：必须 SYSCALL_IMPL 能力（内建组件同样要求声明） */
        if ((m->caps_granted & KMOD_CAP_SYSCALL_IMPL) == 0ull)
            return KMOD_ERR_CAP;
    }

    if (slot->module == idx)
        return KMOD_ERR_EXISTS; /* 同模块重复导出同一槽 */
    if (slot->module >= 0)
        return KMOD_ERR_EXISTS; /* 已被其他模块占用 */

    if (nr < KMOD_SYSCALL_CORE_MAX) {
        if (builtin) {
            /* 内建组件首绑：固定槽初始无实现（fn==null） */
            if (slot->fn != nullptr)
                return KMOD_ERR_EXISTS;
        } else {
            /* .ko 覆写可替换槽：此时应仍指向 Tier0 默认实现 */
            if (slot->fn != g_slot_defaults[nr] || slot->fn == nullptr)
                return KMOD_ERR_BUSY;
        }
    } else if (slot->fn != nullptr) {
        return KMOD_ERR_EXISTS;
    }

    if (*n_undo >= KMOD_MAX_EXPORTS)
        return KMOD_ERR_NOMEM;
    undo[*n_undo].nr = static_cast<uint16_t>(nr);
    undo[*n_undo].module = slot->module;
    undo[*n_undo].fn = slot->fn;
    undo[*n_undo].generation = slot->generation;
    (*n_undo)++;

    __atomic_store_n(&slot->fn, fn, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->module, idx, __ATOMIC_RELEASE);
    slot->generation++;
    return KMOD_OK;
}

/* 宿主测试兜底：governance.cpp 未落地时 epoch 原语也能链接（strong 覆盖 weak）。
   语义与治理层一致：模块必须 READY 才允许进入，进入即 epoch_active+1。 */
#ifdef __KERNEL_TEST_HOST__
int32_t test_epoch_enter(const char *name)
{
    if (!name)
        return KMOD_ERR_INVAL;
    for (uint32_t i = 0; i < g_kmod_module_count && i < KMOD_MAX_MODULES;
         i++) {
        const char *a = g_kmod_modules[i].name;
        const char *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a != '\0' || *b != '\0')
            continue;
        if (g_kmod_modules[i].state != KMOD_STATE_READY)
            return KMOD_ERR_BUSY;
        __atomic_fetch_add(&g_kmod_modules[i].epoch_active, 1u,
                           __ATOMIC_SEQ_CST);
        return KMOD_OK;
    }
    return KMOD_ERR_BUSY;
}

void test_epoch_exit(const char *name)
{
    if (!name)
        return;
    for (uint32_t i = 0; i < g_kmod_module_count && i < KMOD_MAX_MODULES;
         i++) {
        const char *a = g_kmod_modules[i].name;
        const char *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a != '\0' || *b != '\0')
            continue;
        uint32_t cur = __atomic_load_n(&g_kmod_modules[i].epoch_active,
                                       __ATOMIC_SEQ_CST);
        if (cur > 0u)
            __atomic_sub_fetch(&g_kmod_modules[i].epoch_active, 1u,
                               __ATOMIC_SEQ_CST);
        return;
    }
}
#endif

} /* namespace */

/* ================================================================== */
/* 宿主兜底：governance.cpp 未编入宿主测试链接时的 epoch 原语（weak）   */
/* ================================================================== */
#ifdef __KERNEL_TEST_HOST__
extern "C" __attribute__((weak)) int32_t
KmodEpochEnterInternal(const char *name)
{
    return test_epoch_enter(name);
}

extern "C" __attribute__((weak)) void KmodEpochExitInternal(const char *name)
{
    test_epoch_exit(name);
}
#endif

/* ================================================================== */
/* 哨兵：可调用的 trampoline，与固定 dispatch 表签名一致                 */
/* ================================================================== */
#ifdef __KERNEL_TEST_HOST__
namespace {
uint64_t sentinel_trampoline(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, void *frame)
{
    return KmodSyscallEntry(frame);
}
} /* namespace */
#else
namespace {
uint64_t sentinel_trampoline(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, syscall_frame_t *frame)
{
    return KmodSyscallEntry(frame);
}
} /* namespace */
#endif

extern "C" void *KmodSyscallSentinel(void)
{
    return reinterpret_cast<void *>(sentinel_trampoline);
}

/* ================================================================== */
/* 初始化：契约表 + 治理槽 + Tier0 默认实现 + 哨兵安装                   */
/* ================================================================== */
extern "C" int32_t KmodSyscallInit(void)
{
    uint64_t flags = kmod_lock_acquire(&g_sc_lock);

    for (uint32_t nr = 0; nr < KMOD_SYSCALL_MAX; nr++) {
        kmod_syscall_slot_t *slot = &g_slots[nr];
        slot->nr = static_cast<uint16_t>(nr);
        slot->flags = slot_initial_flags(nr);
        slot->module = -1;
        slot->fn = nullptr;
        slot->generation = 0u;
        slot->lock = 0;
        g_slot_defaults[nr] = nullptr;
    }

    g_slot_defaults[KMOD_SYSCALL_INFO] = sc_info_impl;
    g_slot_defaults[KMOD_SYSCALL_PROBE] = sc_probe_impl;
    g_slots[KMOD_SYSCALL_INFO].fn = sc_info_impl;
    g_slots[KMOD_SYSCALL_PROBE].fn = sc_probe_impl;

    g_governed_count = KMOD_SYSCALL_CORE_MAX +
                       (KMOD_SYSCALL_DYNAMIC_MAX - KMOD_SYSCALL_DYNAMIC_MIN +
                        1u);
    g_ns_audit_count = 0u;

#ifndef __KERNEL_TEST_HOST__
    /* 在 Tier0 固定 dispatch 表上安装治理哨兵：固定槽 0..63 全部 + 动态槽。
       固定槽实现由内建组件（kernel.syscall.<family>）注册；29/30 由 Tier0
       内部分派。syscall.cpp 的固定入口若走普通调用路径会进入 trampoline；
       若做 "syscall_lists[nr] == KmodSyscallSentinel()" 比较分支也成立。 */
    for (uint32_t nr = 0; nr < KMOD_SYSCALL_CORE_MAX; nr++) {
        syscall_lists[nr] = sentinel_trampoline;
    }
    for (uint32_t nr = KMOD_SYSCALL_DYNAMIC_MIN;
         nr <= KMOD_SYSCALL_DYNAMIC_MAX; nr++) {
        syscall_lists[nr] = sentinel_trampoline;
    }
#endif

    kmod_lock_release(&g_sc_lock, flags);

    kmod_log("[kmod] syscall contracts: %u fixed, %u governed slot(s) "
             "(0..%u + %u..%u), sentinel=%p\n",
             (unsigned)kFixedContractCount, (unsigned)g_governed_count,
             (unsigned)(KMOD_SYSCALL_CORE_MAX - 1u),
             (unsigned)KMOD_SYSCALL_DYNAMIC_MIN,
             (unsigned)KMOD_SYSCALL_DYNAMIC_MAX, KmodSyscallSentinel());
    return KMOD_OK;
}

/* ================================================================== */
/* 契约/槽位观测接口                                                    */
/* ================================================================== */
extern "C" const kmod_syscall_contract_t *KmodSyscallContract(uint32_t nr)
{
    if (nr >= kFixedContractCount)
        return nullptr;
    return &g_contracts[nr];
}

extern "C" uint32_t KmodSyscallContractCount(void)
{
    return kFixedContractCount;
}

extern "C" const kmod_syscall_slot_t *KmodSyscallSlot(uint32_t nr)
{
    if (nr >= KMOD_SYSCALL_MAX)
        return nullptr;
    return &g_slots[nr];
}

extern "C" uint32_t KmodSyscallGovernedCount(void)
{
    return g_governed_count;
}

/* ================================================================== */
/* 提交阶段：注册模块的 SYSCALL / NAMESPACE 导出                        */
/* ================================================================== */
extern "C" int32_t KmodSyscallRegisterModule(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    const int32_t idx = module_index(m);
    if (idx < 0) {
        kmod_log("[kmod] syscall register: module %s not in registry\n",
                 m->name);
        return KMOD_ERR_INVAL;
    }
    if (m->n_exports > KMOD_MAX_EXPORTS)
        return KMOD_ERR_FORMAT;

    ScUndo undo[KMOD_MAX_EXPORTS];
    uint32_t n_undo = 0u;
    uint32_t n_fixed_override = 0u; /* .ko 覆写 SWAPPABLE 固定槽 */
    uint32_t n_builtin_fixed = 0u;  /* 内建组件绑定 FIXED 槽 */
    uint32_t n_dyn = 0u;
    uint32_t n_ns = 0u;
    int32_t rc = KMOD_OK;
    const bool builtin =
        (m->internal_flags & KMOD_MODF_BUILTIN) != 0u;

    uint64_t flags = kmod_lock_acquire(&g_sc_lock);
    for (uint32_t i = 0; i < m->n_exports; i++) {
        const kmod_export_t *e = &m->exports[i];
        if (e->kind == KMOD_SYM_NAMESPACE) {
            /* 命名空间声明只登记审计，不参与回填/分派 */
            if (e->symbol && e->symbol[0]) {
                n_ns++;
                g_ns_audit_count++;
            }
            continue;
        }
        if (e->kind != KMOD_SYM_SYSCALL)
            continue;
        bool is_fixed = false;
        bool is_builtin_fixed = false;
        rc = install_one(m, idx, builtin, e, undo, &n_undo, &is_fixed,
                         &is_builtin_fixed);
        if (rc != KMOD_OK)
            break;
        if (is_builtin_fixed)
            n_builtin_fixed++;
        else if (is_fixed)
            n_fixed_override++;
        else
            n_dyn++;
    }

    if (rc != KMOD_OK) {
        /* 本次已安装部分逆序回滚（头文件契约） */
        while (n_undo > 0u) {
            n_undo--;
            const ScUndo &u = undo[n_undo];
            kmod_syscall_slot_t *slot = &g_slots[u.nr];
            __atomic_store_n(&slot->fn, u.fn, __ATOMIC_RELEASE);
            __atomic_store_n(&slot->module, u.module, __ATOMIC_RELEASE);
            slot->generation = u.generation;
        }
        kmod_lock_release(&g_sc_lock, flags);
        kmod_log("[kmod] syscall register %s FAILED rc=%d\n", m->name,
                 (int)rc);
        return rc;
    }

    if (n_dyn || n_builtin_fixed)
        __atomic_fetch_add(&g_kmod_stats.syscall_binds,
                           (uint64_t)(n_dyn + n_builtin_fixed),
                           __ATOMIC_RELAXED);
    if (n_fixed_override)
        __atomic_fetch_add(&g_kmod_stats.syscall_overrides,
                           (uint64_t)n_fixed_override, __ATOMIC_RELAXED);
    kmod_lock_release(&g_sc_lock, flags);

    if (n_ns) {
        kmod_log("[kmod] syscall ns: %s declares %u namespace export(s)\n",
                 m->name, (unsigned)n_ns);
    }
    if (n_dyn || n_builtin_fixed || n_fixed_override) {
        kmod_log("[kmod] syscall bind: %s%s -> %u dynamic, %u builtin fixed, "
                 "%u fixed override\n",
                 m->name, builtin ? " (builtin)" : "", (unsigned)n_dyn,
                 (unsigned)n_builtin_fixed, (unsigned)n_fixed_override);
    }
    return KMOD_OK;
}

extern "C" void KmodSyscallUnregisterModule(kmod_module_t *m)
{
    const int32_t idx = module_index(m);
    if (idx < 0)
        return; /* 未注册/重复调用安全 no-op */
    uint64_t flags = kmod_lock_acquire(&g_sc_lock);
    for (uint32_t nr = 0; nr < KMOD_SYSCALL_MAX; nr++) {
        kmod_syscall_slot_t *slot = &g_slots[nr];
        if (slot->module != idx)
            continue;
        __atomic_store_n(&slot->fn, g_slot_defaults[nr], __ATOMIC_RELEASE);
        __atomic_store_n(&slot->module, -1, __ATOMIC_RELEASE);
        slot->generation++;
    }
    kmod_lock_release(&g_sc_lock, flags);
}

extern "C" int32_t KmodSyscallRepublishModule(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    /* 热替换：先按 module 下标清掉旧实现，再按 m->exports 当前值重新发布 */
    KmodSyscallUnregisterModule(m);
    return KmodSyscallRegisterModule(m);
}

/* ================================================================== */
/* 固定 dispatch 钩子：KmodSyscallEntry(frame)                          */
/* ================================================================== */
extern "C" uint64_t KmodSyscallEntry(void *frame)
{
    if (!frame)
        return sc_err(KMOD_SC_EINVAL);
    const uint32_t nr = static_cast<uint32_t>(frame_nr(frame));
    if (nr >= KMOD_SYSCALL_MAX || !slot_governed(nr))
        return sc_err(KMOD_SC_ENOSYS);

    kmod_syscall_slot_t *slot = &g_slots[nr];
    kmod_syscall_fn fn =
        __atomic_load_n(&slot->fn, __ATOMIC_ACQUIRE);
    int32_t module = __atomic_load_n(&slot->module, __ATOMIC_ACQUIRE);
    if (!fn)
        return sc_err(KMOD_SC_ENOSYS);

    const uint64_t a0 = frame_rdi(frame);
    const uint64_t a1 = frame_rsi(frame);
    const uint64_t a2 = frame_rdx(frame);
    const uint64_t a3 = frame_r10(frame);
    const uint64_t a4 = frame_r8(frame);
    const uint64_t a5 = frame_r9(frame);

    if (module < 0)
        return fn(a0, a1, a2, a3, a4, a5, frame); /* Tier0 内建实现 */

    if (static_cast<uint32_t>(module) >= g_kmod_module_count)
        return sc_err(KMOD_SC_ENOSYS);
    const char *name = g_kmod_modules[module].name;

    /* exit 的固定语义是"不返回"（Schedule::Exit 切走线程），epoch 无法配对
       退出；若进入 epoch 会让该模块计数永久泄漏。Tier0 已知固定语义，
       这里直接调用（实现是内核静态代码，生命周期由 Tier0 保证）。 */
    if (nr == kSlotExit)
        return fn(a0, a1, a2, a3, a4, a5, frame);

    /* epoch 治理：模块必须 READY/可进入，否则用户态看到 -EAGAIN */
    if (KmodEpochEnterInternal(name) != KMOD_OK)
        return sc_err(KMOD_SC_EAGAIN);

    /* epoch 已持有：热替换/卸载必须等我们退出，实现指针在此区间稳定。
       仍重新读取一次，覆盖"进入 epoch 前刚完成替换"的窗口。 */
    fn = __atomic_load_n(&slot->fn, __ATOMIC_ACQUIRE);
    uint64_t ret = sc_err(KMOD_SC_ENOSYS);
    if (fn)
        ret = fn(a0, a1, a2, a3, a4, a5, frame);
    KmodEpochExitInternal(name);
    return ret;
}

/* ================================================================== */
/* SYSCALL_KMOD_INFO：模块表快照写用户缓冲                              */
/* ================================================================== */
extern "C" int32_t KmodSyscallCopyInfoToUser(void *ubuf, uint64_t usize,
                                             uint64_t flags)
{
    if (!ubuf || flags != 0ull)
        return KMOD_ERR_INVAL;
    if (usize < sizeof(kmod_sysinfo_t))
        return KMOD_ERR_INVAL;

    kmod_sysinfo_t hdr = {};
    hdr.abi_version = KMOD_ABI_VERSION;

    uint64_t lock_flags = kmod_lock_acquire(&g_kmod_lock);
    uint32_t total = g_kmod_module_count;
    if (total > KMOD_MAX_MODULES)
        total = KMOD_MAX_MODULES;
    hdr.stats = g_kmod_stats;
    kmod_lock_release(&g_kmod_lock, lock_flags);

    uint64_t rec_bytes = usize - sizeof(kmod_sysinfo_t);
    uint32_t max_rec = static_cast<uint32_t>(
        rec_bytes / sizeof(kmod_sysinfo_record_t));
    uint32_t n = total < max_rec ? total : max_rec;
    hdr.total = total;
    hdr.count = n;

    user_copy(ubuf, &hdr, sizeof(hdr));
    uint8_t *dst = reinterpret_cast<uint8_t *>(ubuf) + sizeof(kmod_sysinfo_t);
    for (uint32_t i = 0; i < n; i++) {
        kmod_sysinfo_record_t rec = {};
        lock_flags = kmod_lock_acquire(&g_kmod_lock);
        if (i < g_kmod_module_count)
            fill_record(&rec, &g_kmod_modules[i]);
        kmod_lock_release(&g_kmod_lock, lock_flags);
        user_copy(dst, &rec, sizeof(rec));
        dst += sizeof(rec);
    }
    return static_cast<int32_t>(n);
}

/* ================================================================== */
/* SYSCALL_KMOD_PROBE：KMOD_SYSCALL_PROBE_MAGIC | 激活实现代次 << 32    */
/* ================================================================== */
extern "C" uint64_t KmodSyscallProbeValue(void)
{
    uint64_t flags = kmod_lock_acquire(&g_sc_lock);
    int32_t module = g_slots[KMOD_SYSCALL_PROBE].module;
    /* Tier0 默认实现的代次恒为 0（PROBE 默认值 = MAGIC，见固定契约）；
       模块 override 生效时返回模块代次（每次成功热替换 +1）。 */
    uint32_t gen = 0u;
    if (module >= 0 && static_cast<uint32_t>(module) < g_kmod_module_count)
        gen = g_kmod_modules[module].generation;
    kmod_lock_release(&g_sc_lock, flags);
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | ((uint64_t)gen << 32);
}
