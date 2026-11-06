//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * tests/kmod/syscall_test.cpp —— kmod syscall 治理 + Tier1 座位宿主测试
 * ----------------------------------------------------------------------------
 * 约定（tests/kmod/Makefile + test_main.cpp）：
 *   - 唯一 main 在 test_main.cpp；本文件只提供 extern "C" test_syscall_run()。
 *   - 返回 0 = 全部通过；非 0 = 失败（失败前 printf 具体断言行）。
 * 覆盖：
 *   1) 固定契约表 0..30（名字/flags/owner），22/23 保留占位；
 *   2) KmodSyscallInit 幂等 + 治理槽计数 386（29/30 + 128..511）+ 哨兵；
 *   3) KmodSyscallEntry 直调（FakeFrame 24*u64, rax@14*8）：Tier0 默认
 *      PROBE=MAGIC、INFO 快照、ENOSYS/EAGAIN/EINVAL；
 *   4) 动态槽注册/能力拒绝/重复 EXISTS/部分失败回滚/Unregister/Republish；
 *   5) 固定槽 30 override 安装/回退/KmodSyscallProbeValue；
 *   6) 座位注册表：随机座位 Tier0 默认、模块 attach/detach/rollback、错误码、
 *      KmodSeatRandomU64/Fill（含 fill=NULL 回退、非对齐、len=0/NULL）。
 * 宿主链接说明：真实 governance.cpp 若参与链接则用它的 epoch 原语；否则
 *   syscall_registry.cpp 的 weak 兜底保证本测试仍可运行。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>
#include <kmod/kmod_seats.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "test_common.h"

namespace {

/* ------------------------------------------------------------------ */
/* 固定常量 / 帧构造                                                   */
/* ------------------------------------------------------------------ */
constexpr uint64_t kEnosys = static_cast<uint64_t>(static_cast<int64_t>(-38));
constexpr uint64_t kEagain = static_cast<uint64_t>(static_cast<int64_t>(-11));
constexpr uint64_t kEinval = static_cast<uint64_t>(static_cast<int64_t>(-22));

struct TestFrame {
    uint64_t r[24];
};

void frame_reset(TestFrame *f)
{
    std::memset(f, 0, sizeof(*f));
}

/* rax 在 syscall_frame_t 中偏移 14*8；a0/a1/a2 = rdi/rsi/rdx = r[8]/r[9]/r[12] */
uint64_t call_slot(uint32_t nr, uint64_t a0 = 0, uint64_t a1 = 0,
                   uint64_t a2 = 0)
{
    TestFrame f;
    frame_reset(&f);
    f.r[14] = nr;
    f.r[8] = a0;
    f.r[9] = a1;
    f.r[12] = a2;
    return KmodSyscallEntry(&f);
}

/* ------------------------------------------------------------------ */
/* 假模块构造（直接写 host_platform.cpp 提供的 weak g_kmod_modules[]）  */
/* ------------------------------------------------------------------ */
void copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (src) {
        while (i + 1 < cap && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

kmod_module_t *new_module(uint32_t index, const char *name, uint64_t caps)
{
    if (index >= KMOD_MAX_MODULES)
        return nullptr;
    if (g_kmod_module_count <= index)
        g_kmod_module_count = index + 1u;
    kmod_module_t *m = &g_kmod_modules[index];
    std::memset(m, 0, sizeof(*m));
    copy_str(m->name, KMOD_NAME_MAX, name);
    copy_str(m->version, KMOD_VERSION_MAX, "1.0.0");
    m->state = KMOD_STATE_READY;
    m->tier = KMOD_TIER_IMPL;
    m->caps_granted = caps;
    m->generation = 1u;
    m->pool_index = -1;
    return m;
}

void reset_all(void)
{
    std::memset(&g_kmod_stats, 0, sizeof(g_kmod_stats));
    std::memset(g_kmod_modules, 0, sizeof(g_kmod_modules));
    g_kmod_module_count = 0u;
    (void)KmodSeatInit();
    (void)KmodSyscallInit();
}

/* ------------------------------------------------------------------ */
/* 导出项构造                                                          */
/* ------------------------------------------------------------------ */
typedef uint64_t (*TestSyscallFn)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, void *);

void *fn_addr(TestSyscallFn fn)
{
    return reinterpret_cast<void *>(fn);
}

void set_syscall_export(kmod_export_t *e, uint32_t slot, TestSyscallFn fn,
                        const char *symbol)
{
    e->symbol = symbol;
    e->contract = KMOD_CONTRACT_KERNEL_SYSCALL;
    e->addr = fn_addr(fn);
    e->size = 0u;
    e->abi_version = 1u;
    e->kind = KMOD_SYM_SYSCALL;
    e->flags = 0u;
    e->aux = slot;
    e->reserved = 0u;
}

void set_seat_export(kmod_export_t *e, const char *seat_name,
                     const void *ops, uint32_t size, uint32_t abi)
{
    e->symbol = seat_name;
    e->contract = seat_name;
    e->addr = const_cast<void *>(ops);
    e->size = size;
    e->abi_version = abi;
    e->kind = KMOD_SYM_SEAT;
    e->flags = KMOD_EXF_HOTREPLACEABLE;
    e->aux = 0u;
    e->reserved = 0u;
}

void set_namespace_export(kmod_export_t *e, const char *ns)
{
    e->symbol = ns;
    e->contract = KMOD_CONTRACT_KERNEL_SYSCALL;
    e->addr = nullptr;
    e->size = 0u;
    e->abi_version = 1u;
    e->kind = KMOD_SYM_NAMESPACE;
    e->flags = 0u;
    e->aux = 0u;
    e->reserved = 0u;
}

/* ------------------------------------------------------------------ */
/* 测试桩函数                                                          */
/* ------------------------------------------------------------------ */
uint64_t g_dyn_calls = 0u;

uint64_t stub_dyn_fn(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, void *frame)
{
    (void)a3;
    (void)a4;
    (void)a5;
    (void)frame;
    g_dyn_calls++;
    return a0 + a1 + a2 + 0xD1D1D1D1ull;
}

uint64_t stub_dyn_fn2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, void *frame)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)frame;
    return 0x22222222ull;
}

uint64_t stub_probe_v1(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, void *frame)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)frame;
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (1ull << 32);
}

uint64_t stub_probe_v2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, void *frame)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)frame;
    return (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (2ull << 32);
}

uint64_t g_seat_counter = 0u;

uint64_t stub_seat_u64(void)
{
    return 0x1111111111111111ull + g_seat_counter++;
}

void stub_seat_fill(uint8_t *buf, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
        buf[i] = 0xABu;
}

/* 测试用座位 ops（abi/ops_size 与 kmod_seats.h 一致） */
kmod_seat_random_ops_t g_seat_ops_null_fill = {
    KMOD_SEAT_RANDOM_ABI,
    (uint32_t)sizeof(kmod_seat_random_ops_t),
    stub_seat_u64,
    nullptr,
};

kmod_seat_random_ops_t g_seat_ops_ab_fill = {
    KMOD_SEAT_RANDOM_ABI,
    (uint32_t)sizeof(kmod_seat_random_ops_t),
    stub_seat_u64,
    stub_seat_fill,
};

/* 导出数组（静态存储保证 addr 稳定） */
kmod_export_t g_exp_syscall[4];
kmod_export_t g_exp_seat[4];
kmod_export_t g_exp_nocap[1];
kmod_export_t g_exp_bad[1];
kmod_export_t g_exp_dup[2];
kmod_export_t g_exp_probe[1];
kmod_export_t g_exp_builtin[4];

/* ================================================================== */
/* 1) 固定契约表                                                       */
/* ================================================================== */
int32_t test_contract_table(void)
{
    KCHECK_EQ_U64(KmodSyscallContractCount(), 31u);

    struct NameCase {
        uint32_t nr;
        const char *name;
    };
    const NameCase names[] = {
        {0u, "fopen"},          {1u, "fwrite"},
        {2u, "fread"},          {3u, "fclose"},
        {4u, "flseek"},         {5u, "fsize"},
        {6u, "thread_launch"},  {7u, "gettid"},
        {8u, "getpid"},         {9u, "exit"},
        {10u, "pmmap"},         {11u, "yield"},
        {12u, "load"},          {13u, "launch"},
        {14u, "mmap"},          {15u, "munmap"},
        {16u, "sysinfo"},       {17u, "time"},
        {18u, "arch_prctl"},    {19u, "kill"},
        {20u, "getrandom"},     {21u, "dev_mmap"},
        {24u, "dbgsout"},       {25u, "dev_getinfo"},
        {26u, "dev_ioctl"},     {27u, "monotonic_ms"},
        {28u, "sleep_ms"},      {KMOD_SYSCALL_INFO, "kmod_info"},
        {KMOD_SYSCALL_PROBE, "kmod_probe"},
    };
    for (const NameCase &c : names) {
        const kmod_syscall_contract_t *ct = KmodSyscallContract(c.nr);
        KCHECK_MSG(ct != nullptr, "contract slot %u missing", c.nr);
        KCHECK_EQ_U64(ct->nr, c.nr);
        KCHECK_EQ_U64(ct->abi_version, 1u);
        KCHECK_MSG(std::strcmp(ct->name, c.name) == 0,
                   "slot %u name=%s want=%s", c.nr, ct->name, c.name);
        KCHECK_MSG(std::strcmp(ct->owner, "kernel") == 0, "slot %u owner=%s",
                   c.nr, ct->owner);
    }
    /* 22/23 为保留空洞（只做占位审计） */
    for (uint32_t nr = 22u; nr <= 23u; nr++) {
        const kmod_syscall_contract_t *ct = KmodSyscallContract(nr);
        KCHECK(ct != nullptr);
        KCHECK_MSG(std::strcmp(ct->name, "reserved") == 0,
                   "slot %u should be reserved", nr);
    }
    /* flags：0..28 FIXED；29 FIXED|KMOD；30 FIXED|SWAPPABLE */
    for (uint32_t nr = 0; nr <= 28u; nr++) {
        const kmod_syscall_contract_t *ct = KmodSyscallContract(nr);
        KCHECK_MSG((ct->flags & KMOD_SC_FIXED) != 0u, "slot %u not FIXED", nr);
        KCHECK_MSG((ct->flags & KMOD_SC_SWAPPABLE) == 0u,
                   "slot %u must not be swappable", nr);
    }
    KCHECK_EQ_U64(KmodSyscallContract(KMOD_SYSCALL_INFO)->flags,
                  (uint32_t)(KMOD_SC_FIXED | KMOD_SC_KMOD));
    KCHECK_EQ_U64(KmodSyscallContract(KMOD_SYSCALL_PROBE)->flags,
                  (uint32_t)(KMOD_SC_FIXED | KMOD_SC_SWAPPABLE));
    KCHECK(KmodSyscallContract(31u) == nullptr);
    KCHECK(KmodSyscallContract(KMOD_SYSCALL_CORE_MAX) == nullptr);
    KCHECK(KmodSyscallContract(200u) == nullptr);
    return 0;
}

/* ================================================================== */
/* 2) Init / 治理槽 / 哨兵                                             */
/* ================================================================== */
int32_t test_init_and_slots(void)
{
    reset_all();
    KCHECK_EQ(KmodSyscallInit(), (int32_t)KMOD_OK);
    KCHECK_EQ(KmodSyscallInit(), (int32_t)KMOD_OK); /* 幂等 */

    KCHECK_EQ_U64(KmodSyscallGovernedCount(),
                  (uint64_t)KMOD_SYSCALL_CORE_MAX +
                      (KMOD_SYSCALL_DYNAMIC_MAX - KMOD_SYSCALL_DYNAMIC_MIN +
                       1u));
    KCHECK(KmodSyscallSentinel() != nullptr);

    const kmod_syscall_slot_t *slot = KmodSyscallSlot(0u);
    KCHECK(slot != nullptr);
    KCHECK_EQ_U64(slot->flags, (uint32_t)KMOD_SC_FIXED);
    KCHECK_EQ(slot->module, -1);
    KCHECK(slot->fn == nullptr); /* 0..28 的实现在 syscall.cpp，不在治理表 */

    slot = KmodSyscallSlot(KMOD_SYSCALL_INFO);
    KCHECK(slot != nullptr);
    KCHECK_EQ_U64(slot->flags, (uint32_t)(KMOD_SC_FIXED | KMOD_SC_KMOD));
    KCHECK_EQ(slot->module, -1);
    KCHECK(slot->fn != nullptr); /* Tier0 内建实现 */
    KCHECK_EQ_U64(slot->generation, 0u);

    slot = KmodSyscallSlot(KMOD_SYSCALL_PROBE);
    KCHECK(slot != nullptr);
    KCHECK_EQ_U64(slot->flags, (uint32_t)(KMOD_SC_FIXED | KMOD_SC_SWAPPABLE));
    KCHECK(slot->fn != nullptr);
    KCHECK_EQ_U64(slot->generation, 0u);

    slot = KmodSyscallSlot(200u);
    KCHECK(slot != nullptr);
    KCHECK_EQ_U64(slot->flags, (uint32_t)KMOD_SC_NAMESPACED);
    KCHECK(slot->fn == nullptr);
    KCHECK_EQ(slot->module, -1);

    KCHECK(KmodSyscallSlot(127u) != nullptr);
    KCHECK_EQ_U64(KmodSyscallSlot(127u)->flags, 0u); /* 非治理区间 */
    KCHECK(KmodSyscallSlot(KMOD_SYSCALL_MAX) == nullptr);
    return 0;
}

/* ================================================================== */
/* 3) Entry 默认行为 + INFO 快照                                        */
/* ================================================================== */
int32_t test_entry_defaults_and_info(void)
{
    reset_all();
    /* Tier0 默认 PROBE：MAGIC（代次 0 -> 高 32 位为 0） */
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_PROBE),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC);
    KCHECK_EQ_U64(KmodSyscallProbeValue(),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC);

    /* 未注册动态槽 / 非治理固定槽 / 越界槽 -> ENOSYS */
    KCHECK_EQ_U64(call_slot(200u), kEnosys);
    KCHECK_EQ_U64(call_slot(10u), kEnosys);
    KCHECK_EQ_U64(call_slot(127u), kEnosys);
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_MAX + 7u), kEnosys);
    KCHECK_EQ_U64(KmodSyscallEntry(nullptr), kEinval);

    /* 哨兵 trampoline 可调用：等价 KmodSyscallEntry(frame) */
    TestFrame f;
    frame_reset(&f);
    f.r[14] = KMOD_SYSCALL_PROBE;
    auto tramp = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t,
                                               uint64_t, uint64_t, uint64_t,
                                               void *)>(KmodSyscallSentinel());
    KCHECK_EQ_U64(tramp(0, 0, 0, 0, 0, 0, &f),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC);

    /* ---- INFO 参数校验 ---- */
    static uint8_t info_buf[sizeof(kmod_sysinfo_t) +
                             4u * sizeof(kmod_sysinfo_record_t) + 16u];
    std::memset(info_buf, 0, sizeof(info_buf));
    KCHECK_EQ(KmodSyscallCopyInfoToUser(nullptr, sizeof(info_buf), 0u),
              (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSyscallCopyInfoToUser(info_buf, sizeof(info_buf), 1u),
              (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSyscallCopyInfoToUser(info_buf,
                                        sizeof(kmod_sysinfo_t) - 1u, 0u),
              (int32_t)KMOD_ERR_INVAL);

    /* 两个假模块的快照 */
    new_module(0u, "test.mod.alpha", KMOD_CAP_LOG);
    kmod_module_t *m1 = new_module(1u, "test.mod.beta", KMOD_CAP_MEM_ALLOC);
    KCHECK(m1 != nullptr);
    g_kmod_modules[0].refcount = 3u;
    g_kmod_modules[0].n_imports = 2u;
    g_kmod_modules[0].n_exports = 5u;
    g_kmod_stats.loads_ok = 42u;

    int32_t rc = KmodSyscallCopyInfoToUser(info_buf, sizeof(info_buf), 0u);
    KCHECK_EQ(rc, 2);
    kmod_sysinfo_t hdr;
    std::memcpy(&hdr, info_buf, sizeof(hdr));
    KCHECK_EQ_U64(hdr.abi_version, KMOD_ABI_VERSION);
    KCHECK_EQ_U64(hdr.count, 2u);
    KCHECK_EQ_U64(hdr.total, 2u);
    KCHECK_EQ_U64(hdr.stats.loads_ok, 42u);

    kmod_sysinfo_record_t rec;
    std::memcpy(&rec, info_buf + sizeof(kmod_sysinfo_t), sizeof(rec));
    KCHECK_MSG(std::strcmp(rec.name, "test.mod.alpha") == 0, "rec0 name=%s",
               rec.name);
    KCHECK_EQ_U64(rec.refcount, 3u);
    KCHECK_EQ_U64(rec.n_imports, 2u);
    KCHECK_EQ_U64(rec.n_exports, 5u);
    KCHECK_EQ_U64(rec.tier, (uint64_t)KMOD_TIER_IMPL);
    KCHECK_EQ_U64(rec.state, (uint64_t)KMOD_STATE_READY);
    KCHECK_EQ_U64(rec.caps_granted, (uint64_t)KMOD_CAP_LOG);

    std::memcpy(&rec,
                info_buf + sizeof(kmod_sysinfo_t) +
                    sizeof(kmod_sysinfo_record_t),
                sizeof(rec));
    KCHECK_MSG(std::strcmp(rec.name, "test.mod.beta") == 0, "rec1 name=%s",
               rec.name);

    /* 截断：只够 1 条记录 -> count=1, total=2 */
    uint64_t one = sizeof(kmod_sysinfo_t) + sizeof(kmod_sysinfo_record_t);
    std::memset(info_buf, 0, sizeof(info_buf));
    KCHECK_EQ(KmodSyscallCopyInfoToUser(info_buf, one, 0u), 1);
    std::memcpy(&hdr, info_buf, sizeof(hdr));
    KCHECK_EQ_U64(hdr.count, 1u);
    KCHECK_EQ_U64(hdr.total, 2u);

    /* 只够头部 -> 0 条 */
    std::memset(info_buf, 0, sizeof(info_buf));
    KCHECK_EQ(KmodSyscallCopyInfoToUser(info_buf, sizeof(kmod_sysinfo_t), 0u),
              0);
    std::memcpy(&hdr, info_buf, sizeof(hdr));
    KCHECK_EQ_U64(hdr.count, 0u);
    KCHECK_EQ_U64(hdr.total, 2u);

    /* 非对齐用户缓冲（实现按字节 memcpy，不应要求对齐） */
    std::memset(info_buf, 0, sizeof(info_buf));
    KCHECK_EQ(KmodSyscallCopyInfoToUser(info_buf + 1u, one, 0u), 1);
    std::memcpy(&hdr, info_buf + 1u, sizeof(hdr));
    KCHECK_EQ_U64(hdr.count, 1u);

    /* 通过 syscall 29 帧调用同样返回记录数 */
    std::memset(info_buf, 0, sizeof(info_buf));
    uint64_t r = call_slot(KMOD_SYSCALL_INFO,
                           reinterpret_cast<uint64_t>(info_buf),
                           sizeof(info_buf), 0u);
    KCHECK_EQ_U64(r, 2u);
    std::memcpy(&hdr, info_buf, sizeof(hdr));
    KCHECK_EQ_U64(hdr.count, 2u);

    /* flags 非 0 经 syscall 返回 -EINVAL(22) */
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_INFO,
                            reinterpret_cast<uint64_t>(info_buf),
                            sizeof(info_buf), 1u),
                  kEinval);
    return 0;
}

/* ================================================================== */
/* 4) 动态命名空间槽注册                                              */
/* ================================================================== */
int32_t test_dynamic_registration(void)
{
    reset_all();

    /* 带 NAMESPACE 导出（只登记审计，不影响安装） */
    kmod_module_t *m0 = new_module(
        0u, "test.syscall.owner",
        (uint64_t)(KMOD_CAP_SYSCALL_IMPL | KMOD_CAP_LOG));
    set_syscall_export(&g_exp_syscall[0], 200u, stub_dyn_fn, "dyn200");
    set_namespace_export(&g_exp_syscall[1], "test.syscall.owner");
    m0->exports = g_exp_syscall;
    m0->n_exports = 2u;

    KCHECK_EQ(KmodSyscallRegisterModule(m0), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(g_kmod_stats.syscall_binds, 1u);

    const kmod_syscall_slot_t *slot = KmodSyscallSlot(200u);
    KCHECK_EQ(slot->module, 0);
    KCHECK(slot->fn != nullptr);
    KCHECK_EQ_U64(slot->generation, 1u);

    g_dyn_calls = 0u;
    KCHECK_EQ_U64(call_slot(200u, 7u, 11u, 13u),
                  7u + 11u + 13u + 0xD1D1D1D1ull);
    KCHECK_EQ_U64(g_dyn_calls, 1u);
    KCHECK_EQ_U64(g_kmod_modules[0].epoch_active, 0u); /* epoch 已退出 */

    /* 同模块重复注册 -> EXISTS；不同模块占同槽 -> EXISTS */
    KCHECK_EQ(KmodSyscallRegisterModule(m0), (int32_t)KMOD_ERR_EXISTS);
    kmod_module_t *m1 =
        new_module(1u, "test.syscall.thief", (uint64_t)(KMOD_CAP_SYSCALL_IMPL));
    m1->exports = g_exp_syscall;
    m1->n_exports = 1u; /* 同一个 slot 200 导出 */
    KCHECK_EQ(KmodSyscallRegisterModule(m1), (int32_t)KMOD_ERR_EXISTS);

    /* 缺 KMOD_CAP_SYSCALL_IMPL -> CAP */
    kmod_module_t *m2 = new_module(2u, "test.syscall.nocap", 0ull);
    set_syscall_export(&g_exp_nocap[0], 201u, stub_dyn_fn, "dyn201");
    m2->exports = g_exp_nocap;
    m2->n_exports = 1u;
    KCHECK_EQ(KmodSyscallRegisterModule(m2), (int32_t)KMOD_ERR_CAP);

    /* 非治理槽（<128 且非 29/30）-> POLICY；越界 -> INVAL */
    set_syscall_export(&g_exp_bad[0], 100u, stub_dyn_fn, "bad100");
    m2->exports = g_exp_bad;
    KCHECK_EQ(KmodSyscallRegisterModule(m2), (int32_t)KMOD_ERR_POLICY);
    set_syscall_export(&g_exp_bad[0], 40u, stub_dyn_fn, "bad40");
    KCHECK_EQ(KmodSyscallRegisterModule(m2), (int32_t)KMOD_ERR_POLICY);
    set_syscall_export(&g_exp_bad[0], KMOD_SYSCALL_MAX, stub_dyn_fn, "bad512");
    KCHECK_EQ(KmodSyscallRegisterModule(m2), (int32_t)KMOD_ERR_INVAL);
    set_syscall_export(&g_exp_bad[0], 202u, stub_dyn_fn, "dyn202");
    g_exp_bad[0].addr = nullptr;
    KCHECK_EQ(KmodSyscallRegisterModule(m2), (int32_t)KMOD_ERR_INVAL);

    /* 模块不在 g_kmod_modules[] 内 -> INVAL */
    kmod_module_t stack_mod;
    std::memset(&stack_mod, 0, sizeof(stack_mod));
    copy_str(stack_mod.name, KMOD_NAME_MAX, "test.syscall.stack");
    stack_mod.caps_granted = KMOD_CAP_SYSCALL_IMPL;
    stack_mod.exports = g_exp_bad;
    stack_mod.n_exports = 0u;
    KCHECK_EQ(KmodSyscallRegisterModule(&stack_mod), (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSyscallRegisterModule(nullptr), (int32_t)KMOD_ERR_INVAL);

    /* 部分失败回滚：同模块两个导出同槽 -> 第二个 EXISTS，第一个必须撤销 */
    reset_all();
    kmod_module_t *m3 =
        new_module(0u, "test.syscall.dup", (uint64_t)(KMOD_CAP_SYSCALL_IMPL));
    set_syscall_export(&g_exp_dup[0], 203u, stub_dyn_fn, "dup_a");
    set_syscall_export(&g_exp_dup[1], 203u, stub_dyn_fn2, "dup_b");
    m3->exports = g_exp_dup;
    m3->n_exports = 2u;
    KCHECK_EQ(KmodSyscallRegisterModule(m3), (int32_t)KMOD_ERR_EXISTS);
    KCHECK_EQ(KmodSyscallSlot(203u)->module, -1);
    KCHECK(KmodSyscallSlot(203u)->fn == nullptr);
    KCHECK_EQ_U64(call_slot(203u), kEnosys);
    KCHECK_EQ_U64(g_kmod_stats.syscall_binds, 0u);

    /* 正常注册后 EAGAIN：模块非 READY 时拒绝分派 */
    m3->n_exports = 1u;
    KCHECK_EQ(KmodSyscallRegisterModule(m3), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(call_slot(203u), 0xD1D1D1D1ull);
    g_kmod_modules[0].state = KMOD_STATE_QUIESCING;
    KCHECK_EQ_U64(call_slot(203u), kEagain);
    KCHECK_EQ_U64(g_kmod_modules[0].epoch_active, 0u);
    g_kmod_modules[0].state = KMOD_STATE_READY;
    KCHECK_EQ_U64(call_slot(203u), 0xD1D1D1D1ull);

    /* Unregister：清槽、重复卸载 no-op、Entry 回到 ENOSYS */
    KmodSyscallUnregisterModule(m3);
    KCHECK_EQ(KmodSyscallSlot(203u)->module, -1);
    KCHECK(KmodSyscallSlot(203u)->fn == nullptr);
    KCHECK_EQ_U64(call_slot(203u), kEnosys);
    KmodSyscallUnregisterModule(m3); /* 幂等 no-op */
    KmodSyscallUnregisterModule(nullptr);

    /* Republish（热替换）：导出表切换后重新发布新实现 */
    m3->n_exports = 1u;
    set_syscall_export(&g_exp_dup[0], 203u, stub_dyn_fn, "v1");
    KCHECK_EQ(KmodSyscallRegisterModule(m3), (int32_t)KMOD_OK);
    set_syscall_export(&g_exp_dup[0], 203u, stub_dyn_fn2, "v2");
    m3->generation = 2u;
    KCHECK_EQ(KmodSyscallRepublishModule(m3), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(call_slot(203u), 0x22222222ull);
    KmodSyscallUnregisterModule(m3);
    KCHECK_EQ_U64(call_slot(203u), kEnosys);
    KCHECK_EQ(KmodSyscallRepublishModule(nullptr), (int32_t)KMOD_ERR_INVAL);
    return 0;
}

/* ================================================================== */
/* 5) 固定槽 30 override / 回退 / ProbeValue                          */
/* ================================================================== */
int32_t test_fixed_override(void)
{
    reset_all();
    KCHECK_EQ_U64(KmodSyscallProbeValue(),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC);

    /* 无 CAP_SYSCALL_OVERRIDE -> CAP */
    kmod_module_t *m0 = new_module(
        0u, "test.probe.nocap", (uint64_t)KMOD_CAP_SYSCALL_IMPL);
    set_syscall_export(&g_exp_probe[0], KMOD_SYSCALL_PROBE, stub_probe_v1,
                       "probe_v1");
    m0->exports = g_exp_probe;
    m0->n_exports = 1u;
    KCHECK_EQ(KmodSyscallRegisterModule(m0), (int32_t)KMOD_ERR_CAP);

    /* 固定槽但非 SWAPPABLE（10 普通固定槽 / 29 kmod 自省槽）-> POLICY */
    kmod_module_t *m1 = new_module(
        1u, "test.probe.override",
        (uint64_t)(KMOD_CAP_SYSCALL_OVERRIDE | KMOD_CAP_SYSCALL_IMPL));
    m1->exports = g_exp_probe;
    m1->n_exports = 1u;
    set_syscall_export(&g_exp_probe[0], 10u, stub_dyn_fn, "fixed10");
    KCHECK_EQ(KmodSyscallRegisterModule(m1), (int32_t)KMOD_ERR_POLICY);
    set_syscall_export(&g_exp_probe[0], KMOD_SYSCALL_INFO, stub_dyn_fn,
                       "info29");
    KCHECK_EQ(KmodSyscallRegisterModule(m1), (int32_t)KMOD_ERR_POLICY);

    /* 合法 override：Entry 走模块实现、ProbeValue 反映模块代次 */
    set_syscall_export(&g_exp_probe[0], KMOD_SYSCALL_PROBE, stub_probe_v1,
                       "probe_v1");
    KCHECK_EQ(KmodSyscallRegisterModule(m1), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(g_kmod_stats.syscall_overrides, 1u);
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_PROBE),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (1ull << 32));
    KCHECK_EQ_U64(KmodSyscallProbeValue(),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (1ull << 32));

    /* 再注册一遍 -> EXISTS */
    KCHECK_EQ(KmodSyscallRegisterModule(m1), (int32_t)KMOD_ERR_EXISTS);

    /* Unregister -> 回退 Tier0 默认实现（MAGIC, generation=0） */
    KmodSyscallUnregisterModule(m1);
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_PROBE),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC);
    KCHECK_EQ_U64(KmodSyscallProbeValue(),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC);
    KCHECK_EQ_U64(g_kmod_stats.syscall_overrides, 1u);

    /* Republish：v1 -> v2 热替换后固定槽实现切换 */
    set_syscall_export(&g_exp_probe[0], KMOD_SYSCALL_PROBE, stub_probe_v1, "v1");
    KCHECK_EQ(KmodSyscallRegisterModule(m1), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_PROBE),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (1ull << 32));
    set_syscall_export(&g_exp_probe[0], KMOD_SYSCALL_PROBE, stub_probe_v2, "v2");
    m1->generation = 2u;
    KCHECK_EQ(KmodSyscallRepublishModule(m1), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(call_slot(KMOD_SYSCALL_PROBE),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (2ull << 32));
    KCHECK_EQ_U64(KmodSyscallProbeValue(),
                  (uint64_t)KMOD_SYSCALL_PROBE_MAGIC | (2ull << 32));
    KmodSyscallUnregisterModule(m1);
    return 0;
}

/* ================================================================== */
/* 5b) 内建组件注册 FIXED 槽（组件化阶段）                              */
/* ================================================================== */
int32_t test_builtin_fixed_registration(void)
{
    reset_all();

    /* Tier0 构建产物的内建组件：caps=0 也可注册 FIXED 槽（可信），
       固定槽 0/1 初始无实现（fn==nullptr，由组件首绑）。 */
    kmod_module_t *bm0 = new_module(0u, "kernel.syscall.file.fake", 0ull);
    bm0->internal_flags = KMOD_MODF_BUILTIN;
    set_syscall_export(&g_exp_builtin[0], 0u, stub_dyn_fn, "sys_fopen");
    set_syscall_export(&g_exp_builtin[1], 1u, stub_dyn_fn, "sys_fwrite");
    bm0->exports = g_exp_builtin;
    bm0->n_exports = 2u;
    KCHECK_EQ(KmodSyscallRegisterModule(bm0), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(g_kmod_stats.syscall_binds, 2u);
    KCHECK_EQ_U64(g_kmod_stats.syscall_overrides, 0u);
    KCHECK_EQ(KmodSyscallSlot(0u)->module, 0);
    KCHECK(KmodSyscallSlot(0u)->fn != nullptr);
    KCHECK_EQ(KmodSyscallSlot(1u)->module, 0);

    /* Entry 正常分派到内建组件实现，epoch 配对退出 */
    g_dyn_calls = 0u;
    KCHECK_EQ_U64(call_slot(0u, 5u, 6u, 7u),
                  5u + 6u + 7u + 0xD1D1D1D1ull);
    KCHECK_EQ_U64(g_dyn_calls, 1u);
    KCHECK_EQ_U64(g_kmod_modules[0].epoch_active, 0u);

    /* 同模块重复注册 / 其他模块占槽 -> EXISTS */
    KCHECK_EQ(KmodSyscallRegisterModule(bm0), (int32_t)KMOD_ERR_EXISTS);
    kmod_module_t *bm1 = new_module(1u, "kernel.syscall.other.fake", 0ull);
    bm1->internal_flags = KMOD_MODF_BUILTIN;
    set_syscall_export(&g_exp_builtin[2], 0u, stub_dyn_fn2, "sys_dup0");
    bm1->exports = &g_exp_builtin[2];
    bm1->n_exports = 1u;
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_ERR_EXISTS);

    /* 29/30 是 Tier0 内部实现（INFO/PROBE），内建组件也不得注册 -> POLICY */
    set_syscall_export(&g_exp_builtin[2], KMOD_SYSCALL_INFO, stub_dyn_fn2,
                       "info29");
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_ERR_POLICY);
    set_syscall_export(&g_exp_builtin[2], KMOD_SYSCALL_PROBE, stub_dyn_fn2,
                       "probe30");
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_ERR_POLICY);

    /* 31..63 无契约定义的固定槽 -> POLICY */
    set_syscall_export(&g_exp_builtin[2], 40u, stub_dyn_fn2, "slot40");
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_ERR_POLICY);

    /* 内建组件的动态命名空间槽仍要求 CAP_SYSCALL_IMPL */
    set_syscall_export(&g_exp_builtin[2], 210u, stub_dyn_fn2, "dyn210");
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_ERR_CAP);
    bm1->caps_granted = (uint64_t)KMOD_CAP_SYSCALL_IMPL;
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_OK);
    KCHECK_EQ(KmodSyscallSlot(210u)->module, 1);
    KmodSyscallUnregisterModule(bm1);

    /* 非内建（.ko）注册 FIXED 非 SWAPPABLE 槽 -> POLICY */
    kmod_module_t *k0 = new_module(
        2u, "test.ko.override", (uint64_t)(KMOD_CAP_SYSCALL_OVERRIDE |
                                           KMOD_CAP_SYSCALL_IMPL));
    set_syscall_export(&g_exp_builtin[2], 2u, stub_dyn_fn2, "sys_fread");
    k0->exports = &g_exp_builtin[2];
    k0->n_exports = 1u;
    KCHECK_EQ(KmodSyscallRegisterModule(k0), (int32_t)KMOD_ERR_POLICY);

    /* exit(9) 固定语义"不返回"：治理层调用该槽实现不做 epoch 包裹，
       避免 epoch 计数永久泄漏（用普通 stub 验证计数保持 0）。 */
    bm1->caps_granted = 0ull;
    set_syscall_export(&g_exp_builtin[2], 9u, stub_dyn_fn2, "sys_exit_fake");
    KCHECK_EQ(KmodSyscallRegisterModule(bm1), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(call_slot(9u), 0x22222222ull);
    KCHECK_EQ_U64(g_kmod_modules[1].epoch_active, 0u);
    KmodSyscallUnregisterModule(bm1);

    /* 卸载清理：全部固定槽回到无实现，Entry -> ENOSYS；重复卸载 no-op */
    KmodSyscallUnregisterModule(bm0);
    KCHECK(KmodSyscallSlot(0u)->fn == nullptr);
    KCHECK_EQ(KmodSyscallSlot(0u)->module, -1);
    KCHECK_EQ_U64(call_slot(0u), kEnosys);
    KmodSyscallUnregisterModule(bm0); /* 幂等 no-op */
    KCHECK_EQ(KmodSyscallSlot(0u)->module, -1);
    return 0;
}

/* ================================================================== */
/* 6) 座位注册表 / 随机数座位                                          */
/* ================================================================== */
int32_t test_seats(void)
{
    reset_all();

    KCHECK_EQ_U64(KmodSeatCount(), 1u);
    KCHECK(KmodSeatFind("no.such.seat.v1") == nullptr);
    const kmod_seat_t *seat = KmodSeatFind(KMOD_SEAT_RANDOM);
    KCHECK(seat != nullptr);
    KCHECK_EQ_U64(seat->abi_version, (uint64_t)KMOD_SEAT_RANDOM_ABI);
    KCHECK_EQ_U64(seat->ops_size, (uint64_t)sizeof(kmod_seat_random_ops_t));
    KCHECK(seat->kernel_ops != nullptr);
    KCHECK_EQ(seat->provider, -1);
    KCHECK(seat->module_ops == nullptr);
    KCHECK(KmodSeatAt(0u) == seat);
    KCHECK(KmodSeatAt(KmodSeatCount()) == nullptr);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) == seat->kernel_ops);
    KCHECK(KmodSeatCurrent("no.such.seat.v1") == nullptr);

    /* Tier0 默认随机源：64 次调用应有多样性，且 Fill 覆盖整段 */
    uint64_t prev = KmodSeatRandomU64();
    uint32_t changes = 0u;
    for (uint32_t i = 0; i < 64u; i++) {
        uint64_t v = KmodSeatRandomU64();
        if (v != prev)
            changes++;
        prev = v;
    }
    KCHECK_MSG(changes >= 32u, "kernel rng not random enough (%u)", changes);

    uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));
    KmodSeatRandomFill(nullptr, 16u); /* NULL buf: 安全 no-op */
    KmodSeatRandomFill(buf, 0u);      /* len=0: 安全 no-op */
    KmodSeatRandomFill(buf, sizeof(buf));
    uint32_t nonzero = 0u;
    for (uint32_t i = 0; i < sizeof(buf); i++)
        if (buf[i] != 0u)
            nonzero++;
    KCHECK_MSG(nonzero >= 56u, "kernel rng fill wrote %u/64 non-zero", nonzero);

    /* 非对齐 + 尾部：只写 [1, 1+13)，前后哨兵保持不变 */
    std::memset(buf, 0, sizeof(buf));
    KmodSeatRandomFill(buf + 1u, 13u);
    KCHECK_EQ_U64(buf[0], 0u);
    KCHECK_EQ_U64(buf[14], 0u);

    /* ---- 模块提供者（fill=NULL，走循环 u64 回退） ---- */
    g_seat_counter = 0u;
    kmod_module_t *m0 = new_module(
        0u, "test.seat.provider",
        (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_LOG));
    set_seat_export(&g_exp_seat[0], KMOD_SEAT_RANDOM, &g_seat_ops_null_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    m0->exports = g_exp_seat;
    m0->n_exports = 1u;
    KCHECK_EQ(KmodSeatAttachModule(m0), (int32_t)KMOD_OK);
    KCHECK_EQ_U64(g_kmod_stats.seat_swaps, 1u);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) == &g_seat_ops_null_fill);
    KCHECK_EQ(KmodSeatFind(KMOD_SEAT_RANDOM)->provider, 0);
    KCHECK_EQ_U64(KmodSeatFind(KMOD_SEAT_RANDOM)->generation >= 2u ? 1u : 0u,
                  1u);
    KCHECK_EQ_U64(KmodSeatRandomU64() >> 60, 1u); /* 0x1111... 高 4 位 */
    KCHECK_EQ_U64(g_kmod_modules[0].epoch_active, 0u);

    std::memset(buf, 0, sizeof(buf));
    KmodSeatRandomFill(buf, 17u); /* fill=NULL -> 循环 u64 填字节 */
    nonzero = 0u;
    for (uint32_t i = 0; i < 17u; i++)
        if (buf[i] != 0u)
            nonzero++;
    KCHECK_MSG(nonzero >= 15u, "seat NULL-fill fallback wrote %u/17 non-zero",
               nonzero);
    KCHECK_EQ_U64(buf[17], 0u);

    /* 模块非 READY：KmodSeatRandomU64 回退内核默认源（不崩、epoch 不残留） */
    g_kmod_modules[0].state = KMOD_STATE_QUIESCING;
    (void)KmodSeatRandomU64();
    KCHECK_EQ_U64(g_kmod_modules[0].epoch_active, 0u);
    g_kmod_modules[0].state = KMOD_STATE_READY;

    /* ---- 模块提供者（fill 非 NULL）：直接使用模块 fill ---- */
    set_seat_export(&g_exp_seat[0], KMOD_SEAT_RANDOM, &g_seat_ops_ab_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    /* 同模块重发布（热替换场景）幂等：更新实现而不是 BUSY */
    KCHECK_EQ(KmodSeatAttachModule(m0), (int32_t)KMOD_OK);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) == &g_seat_ops_ab_fill);
    std::memset(buf, 0, sizeof(buf));
    KmodSeatRandomFill(buf, 3u);
    KCHECK_EQ_U64(buf[0], 0xABu);
    KCHECK_EQ_U64(buf[1], 0xABu);
    KCHECK_EQ_U64(buf[2], 0xABu);
    KCHECK_EQ_U64(buf[3], 0u);

    /* ---- 错误码 ---- */
    /* 无 KMOD_CAP_SEAT_PROVIDER -> CAP */
    kmod_module_t *bad =
        new_module(1u, "test.seat.bad", (uint64_t)KMOD_CAP_LOG);
    set_seat_export(&g_exp_seat[1], KMOD_SEAT_RANDOM, &g_seat_ops_ab_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    bad->exports = &g_exp_seat[1];
    bad->n_exports = 1u;
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_CAP);

    /* 能力齐备后逐项机械校验 */
    bad->caps_granted = (uint64_t)KMOD_CAP_SEAT_PROVIDER;
    set_seat_export(&g_exp_seat[1], KMOD_SEAT_RANDOM, &g_seat_ops_ab_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI + 1u);
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_VERSION);
    set_seat_export(&g_exp_seat[1], KMOD_SEAT_RANDOM, &g_seat_ops_ab_fill, 8u,
                    KMOD_SEAT_RANDOM_ABI);
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_CONTRACT);
    set_seat_export(&g_exp_seat[1], "no.such.seat.v1", &g_seat_ops_ab_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_NOTFOUND);

    /* 错误 ops 结构（模块内 abi/ops_size/函数指针非法） */
    kmod_seat_random_ops_t bad_ops = g_seat_ops_ab_fill;
    bad_ops.abi_version = KMOD_SEAT_RANDOM_ABI + 1u;
    set_seat_export(&g_exp_seat[1], KMOD_SEAT_RANDOM, &bad_ops,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_VERSION);
    bad_ops = g_seat_ops_ab_fill;
    bad_ops.ops_size = 4u;
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_CONTRACT);
    bad_ops = g_seat_ops_ab_fill;
    bad_ops.u64 = nullptr;
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_INVAL);

    /* 座位已被 module 0 占用 -> 其它模块 BUSY */
    set_seat_export(&g_exp_seat[1], KMOD_SEAT_RANDOM, &g_seat_ops_ab_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    KCHECK_EQ(KmodSeatAttachModule(bad), (int32_t)KMOD_ERR_BUSY);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) == &g_seat_ops_ab_fill);

    /* 部分失败回滚：先释放座位，再验证"第 1 个导出安装、第 2 个失败 -> 全撤销" */
    KmodSeatDetachModule(m0);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) ==
           KmodSeatFind(KMOD_SEAT_RANDOM)->kernel_ops);
    kmod_module_t *m2 = new_module(
        2u, "test.seat.partial", (uint64_t)(KMOD_CAP_SEAT_PROVIDER));
    set_seat_export(&g_exp_seat[2], KMOD_SEAT_RANDOM, &g_seat_ops_null_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI); /* 先成功安装 */
    set_seat_export(&g_exp_seat[3], "no.such.seat.v1", &g_seat_ops_null_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI); /* 再失败 */
    m2->exports = &g_exp_seat[2];
    m2->n_exports = 2u;
    KCHECK_EQ(KmodSeatAttachModule(m2), (int32_t)KMOD_ERR_NOTFOUND);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) ==
           KmodSeatFind(KMOD_SEAT_RANDOM)->kernel_ops);
    KCHECK_EQ(KmodSeatFind(KMOD_SEAT_RANDOM)->provider, -1);
    /* 恢复 m0 提供者，供后续 Detach 语义用例继续 */
    KCHECK_EQ(KmodSeatAttachModule(m0), (int32_t)KMOD_OK);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) == &g_seat_ops_ab_fill);

    /* 模块不在 registry / nullptr -> INVAL */
    kmod_module_t stack_mod;
    std::memset(&stack_mod, 0, sizeof(stack_mod));
    copy_str(stack_mod.name, KMOD_NAME_MAX, "test.seat.stack");
    stack_mod.caps_granted = KMOD_CAP_SEAT_PROVIDER;
    stack_mod.exports = &g_exp_seat[1];
    stack_mod.n_exports = 0u;
    KCHECK_EQ(KmodSeatAttachModule(&stack_mod), (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSeatAttachModule(nullptr), (int32_t)KMOD_ERR_INVAL);

    /* ---- Detach / Rollback ---- */
    KmodSeatDetachModule(m0);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) ==
           KmodSeatFind(KMOD_SEAT_RANDOM)->kernel_ops);
    KCHECK_EQ(KmodSeatFind(KMOD_SEAT_RANDOM)->provider, -1);
    KmodSeatDetachModule(m0); /* 幂等 no-op */
    KmodSeatDetachModule(nullptr);

    set_seat_export(&g_exp_seat[0], KMOD_SEAT_RANDOM, &g_seat_ops_ab_fill,
                    (uint32_t)sizeof(kmod_seat_random_ops_t),
                    KMOD_SEAT_RANDOM_ABI);
    m0->exports = &g_exp_seat[0];
    m0->n_exports = 1u;
    KCHECK_EQ(KmodSeatAttachModule(m0), (int32_t)KMOD_OK);
    KCHECK_EQ(KmodSeatRollbackToKernel(KMOD_SEAT_RANDOM), (int32_t)KMOD_OK);
    KCHECK(KmodSeatCurrent(KMOD_SEAT_RANDOM) ==
           KmodSeatFind(KMOD_SEAT_RANDOM)->kernel_ops);
    KCHECK_EQ(KmodSeatRollbackToKernel(KMOD_SEAT_RANDOM), (int32_t)KMOD_OK);
    KCHECK_EQ(KmodSeatRollbackToKernel("no.such.seat.v1"),
              (int32_t)KMOD_ERR_NOTFOUND);
    KCHECK_EQ(KmodSeatRollbackToKernel(nullptr), (int32_t)KMOD_ERR_INVAL);

    /* ---- 新增内核座位 + 注册参数校验 ---- */
    KCHECK_EQ(KmodSeatRegisterKernel("test.seat.aux.v1", 1u,
                                     &g_seat_ops_null_fill,
                                     (uint32_t)sizeof(g_seat_ops_null_fill),
                                     0ull),
              (int32_t)KMOD_OK);
    KCHECK_EQ_U64(KmodSeatCount(), 2u);
    KCHECK(KmodSeatFind("test.seat.aux.v1") != nullptr);
    /* 同名重复注册=幂等更新 */
    KCHECK_EQ(KmodSeatRegisterKernel("test.seat.aux.v1", 1u,
                                     &g_seat_ops_null_fill,
                                     (uint32_t)sizeof(g_seat_ops_null_fill),
                                     0ull),
              (int32_t)KMOD_OK);
    KCHECK_EQ_U64(KmodSeatCount(), 2u);
    KCHECK_EQ(KmodSeatRegisterKernel(nullptr, 1u, &g_seat_ops_null_fill,
                                     (uint32_t)sizeof(g_seat_ops_null_fill),
                                     0ull),
              (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSeatRegisterKernel("test.seat.aux.v1", 1u, nullptr, 8u, 0ull),
              (int32_t)KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSeatRegisterKernel("test.seat.aux.v1", 1u,
                                     &g_seat_ops_null_fill, 0u, 0ull),
              (int32_t)KMOD_ERR_INVAL);
    char long_name[KMOD_CONTRACT_MAX + 2u];
    std::memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1u] = '\0';
    KCHECK_EQ(KmodSeatRegisterKernel(long_name, 1u, &g_seat_ops_null_fill,
                                     (uint32_t)sizeof(g_seat_ops_null_fill),
                                     0ull),
              (int32_t)KMOD_ERR_NAME);
    return 0;
}

/* ================================================================== */
/* 入口                                                                */
/* ================================================================== */
int32_t run_one(int32_t (*fn)(void), const char *name)
{
    int32_t rc = fn();
    if (rc != 0)
        std::printf("  [info] subtest %s failed (rc=%d)\n", name, (int)rc);
    return rc;
}

} /* namespace */

extern "C" int32_t test_syscall_run(void)
{
    std::printf("  [info] kmod syscall/seat host tests begin\n");

    int32_t rc = run_one(test_contract_table, "contract-table");
    if (rc != 0)
        return rc;
    rc = run_one(test_init_and_slots, "init-slots");
    if (rc != 0)
        return rc;
    rc = run_one(test_entry_defaults_and_info, "entry-info");
    if (rc != 0)
        return rc;
    rc = run_one(test_dynamic_registration, "dynamic-registration");
    if (rc != 0)
        return rc;
    rc = run_one(test_fixed_override, "fixed-override");
    if (rc != 0)
        return rc;
    rc = run_one(test_builtin_fixed_registration, "builtin-fixed");
    if (rc != 0)
        return rc;
    rc = run_one(test_seats, "seats");
    if (rc != 0)
        return rc;

    std::printf("  [info] kmod syscall/seat host tests passed\n");
    return 0;
}
