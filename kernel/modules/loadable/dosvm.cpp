//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * dosvm.cpp —— Tier2 可加载模块（dosvm.ko）
 * ----------------------------------------------------------------------------
 * 定位：单核可逆退出 IA-32e 子系统对用户态的**动态 syscall 门面**。
 *   本模块只做两件事：把固定 dispatch ABI 的寄存器参数拆成契约参数、转调
 *   Tier1 内建组件（kernel/modules/builtin/x86_64/dosvm/）导出的 dosvm_*
 *   函数；不接触 CR0/CR3/CR4/EFER、GDT/IDT、页表、IPI 或调度器。
 *
 * 注册槽：DOSVM_SYSCALL_READ  = 210  (user_buf, cap) -> 4016 / 负 errno
 *         DOSVM_SYSCALL_ENTER = 211  (cpu_id, ...)   -> 0 / 负 errno
 *         DOSVM_SYSCALL_STAGE = 212  (user_img, len, user_args, args_len)
 *         DOSVM_SYSCALL_KEYS  = 213  (user_keys, count)
 *   四者都落在动态命名空间槽 [128,511]，与 demo_syscall_v1/v2 的 200 不冲突。
 *
 * 依赖：provider "kernel" 的 .kout 导出表（kernel_exports.cpp）以
 *       contract "kernel.dosvm.v1" 发布 5 个 dosvm_* 符号；导入槽在加载期
 *       由治理层按 (name, contract, abi) 机械校验并回填绝对地址。
 *
 * flags: KMOD_MF_BOOT | KMOD_MF_UNLOADABLE —— KmodBootLoad() 开机自动加载
 *        （因此 res/limine.conf 必须逐项声明本 .ko 的 module_path）。
 * caps : SYSCALL_IMPL | LOG | EXPERIMENTAL（STRICT 策略需 kBuiltinTrust 授予）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>

#include <stdint.h>
#include <arch/x86_64/dosvm/dosvm_contract.h>

/* 契约头用 extern "C" 声明了 5 个 dosvm_* 函数；导入槽统一加 imp_ 前缀，
 * 避免与这些函数声明在同一翻译单元内重名。symbol_ 仍是内核 .kout 导出名。 */
KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

KMOD_IMPORT_FN(imp_dosvm_read, long, (void *, uint64_t), "kernel",
               "dosvm_read", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_enter, int, (uint32_t), "kernel",
               "dosvm_enter", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_generation, uint64_t, (void), "kernel",
               "dosvm_generation", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_last_cpu, uint64_t, (void), "kernel",
               "dosvm_last_cpu", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_trips, uint64_t, (void), "kernel",
               "dosvm_trips", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_stage_com, int, (void *, uint64_t, void *, uint64_t),
               "kernel", "dosvm_stage_com", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_push_keys, int, (void *, uint64_t), "kernel",
               "dosvm_push_keys", DOSVM_KERNEL_CONTRACT);
KMOD_IMPORT_FN(imp_dosvm_mem_rw, int, (uint64_t, void *, uint64_t, uint32_t),
               "kernel", "dosvm_mem_rw", DOSVM_KERNEL_CONTRACT);

/* 编译期钉住跨层契约：任何一侧漂移都会在 make modules 阶段暴露。 */
static_assert(DOSVM_BUFSZ == 4016u,
              "DOSVM 80x25 snapshot must remain 4016 bytes");
static_assert(DOSVM_SYSCALL_READ >= KMOD_SYSCALL_DYNAMIC_MIN &&
              DOSVM_SYSCALL_READ <= KMOD_SYSCALL_DYNAMIC_MAX,
              "DOSVM_SYSCALL_READ must be a governed dynamic slot");
static_assert(DOSVM_SYSCALL_ENTER >= KMOD_SYSCALL_DYNAMIC_MIN &&
              DOSVM_SYSCALL_ENTER <= KMOD_SYSCALL_DYNAMIC_MAX,
              "DOSVM_SYSCALL_ENTER must be a governed dynamic slot");
static_assert(DOSVM_SYSCALL_READ != 200u && DOSVM_SYSCALL_ENTER != 200u,
              "DOSVM slots must not collide with demo slot 200");
static_assert(DOSVM_SYSCALL_READ != DOSVM_SYSCALL_ENTER,
              "DOSVM READ/ENTER slots must be distinct");
static_assert(DOSVM_SYSCALL_STAGE >= KMOD_SYSCALL_DYNAMIC_MIN &&
              DOSVM_SYSCALL_STAGE <= KMOD_SYSCALL_DYNAMIC_MAX,
              "DOSVM_SYSCALL_STAGE must be a governed dynamic slot");
static_assert(DOSVM_SYSCALL_KEYS >= KMOD_SYSCALL_DYNAMIC_MIN &&
              DOSVM_SYSCALL_KEYS <= KMOD_SYSCALL_DYNAMIC_MAX,
              "DOSVM_SYSCALL_KEYS must be a governed dynamic slot");

/* 导入缺失时用户态应看到 -ENOSYS，而不是空指针解引用导致整机 #PF。
 * init 已把这 6 个槽全部校验为必需；这里的检查只是纵深防御。 */
#define DOSVM_ENOSYS (-38)

/* 210 READ：a0 = 用户态快照缓冲区地址，a1 = cap（字节数上限）。
 * long 返回值（4016 / 负 errno）按模 2^64 转成 uint64_t；用户态按 int64
 * 解读即可还原负 errno，成功值与 DOSVM_BUFSZ 直接比较。 */
static uint64_t dosvm_read_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5,
                                   void *frame)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    if (!imp_dosvm_read)
        return static_cast<uint64_t>(static_cast<int64_t>(DOSVM_ENOSYS));
    const long n = imp_dosvm_read(reinterpret_cast<void *>(a0), a1);
    return static_cast<uint64_t>(n);
}

/* 211 ENTER：a0 = cpu_id（0 = 自动挑选非 BSP 的 AP）。
 * 门面刻意保持薄：按契约把 a0 原样透传给 Tier1 的 dosvm_enter()。
 * 「自动选 AP」的语义由 Tier1 实现（只有它持有 SMP 拓扑），且必须先把 0
 * 映射成 1..smp_last_cpu 的真实 AP 逻辑 id 再调 dosvm_core_offline()——
 * Tier0 的 dosvm_core_offline(0) 返回 -EINVAL（BSP 不可摘）。本 .ko 不
 * include smp.h、不接触调度器，绝不在门面里做该映射或直接 offline BSP。
 * int 返回 0 或负 errno，符号扩展成 uint64_t 后回传。 */
static uint64_t dosvm_enter_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5,
                                    void *frame)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    if (!imp_dosvm_enter)
        return static_cast<uint64_t>(static_cast<int64_t>(DOSVM_ENOSYS));
    const int rc = imp_dosvm_enter(static_cast<uint32_t>(a0));
    return static_cast<uint64_t>(static_cast<int64_t>(rc));
}

/* 212 STAGE：a0 = 用户态 .COM 映像地址，a1 = 字节数，
 *             a2 = 命令行尾地址，a3 = 命令行字节数。
 * a1 == 0 表示卸载映像、回到内置 banner 演示。所有用户内存访问都在
 * Tier1 里用 CopyFromUser 完成（门面不碰页表）。 */
static uint64_t dosvm_stage_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5,
                                    void *frame)
{
    (void)a4; (void)a5; (void)frame;
    if (!imp_dosvm_stage_com)
        return static_cast<uint64_t>(static_cast<int64_t>(DOSVM_ENOSYS));
    const int rc = imp_dosvm_stage_com(reinterpret_cast<void *>(a0), a1,
                                       reinterpret_cast<void *>(a2), a3);
    return static_cast<uint64_t>(static_cast<int64_t>(rc));
}

/* 213 KEYS：a0 = 用户态 u16 数组地址，a1 = 元素个数（最多 32）。
 * 元素编码 (scan << 8) | ascii。实模式里 IF=0，没有活键盘路径。 */
static uint64_t dosvm_keys_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5,
                                   void *frame)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)frame;
    if (!imp_dosvm_push_keys)
        return static_cast<uint64_t>(static_cast<int64_t>(DOSVM_ENOSYS));
    const int rc = imp_dosvm_push_keys(reinterpret_cast<void *>(a0), a1);
    return static_cast<uint64_t>(static_cast<int64_t>(rc));
}

/* 214 MEM：a0 = 相对 0x70000 的偏移，a1 = 用户缓冲区，a2 = 字节数，
 *          a3 != 0 表示写（用户 -> 低内存），否则读。
 * 16 位程序的映像、PSP、挂起上下文和文件请求块都住在那 64 KiB 里，而
 * 用户态看不见物理内存，所以这是唯一的通道；越界由 Tier1 卡死。 */
static uint64_t dosvm_mem_syscall(uint64_t a0, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5,
                                  void *frame)
{
    (void)a4; (void)a5; (void)frame;
    if (!imp_dosvm_mem_rw)
        return static_cast<uint64_t>(static_cast<int64_t>(DOSVM_ENOSYS));
    const int rc = imp_dosvm_mem_rw(a0, reinterpret_cast<void *>(a1), a2,
                                    static_cast<uint32_t>(a3));
    return static_cast<uint64_t>(static_cast<int64_t>(rc));
}

static int32_t dosvm_facade_init(void)
{
    /* 8 个导入（7 个 dosvm_* + 日志出口）全部是 REQUIRED；任一为空说明
     * 内核 .kout 未发布 Tier1 实现，加载事务必须失败而不是留半可用门面。
     * generation / last_cpu / trips 是契约规定必须存在的可观测面：影屏
     * header 由 Tier1 填充，门面保留导入供审计与后续扩展，并在此校验非空。 */
    if (!kprintf_ || !imp_dosvm_read || !imp_dosvm_enter ||
        !imp_dosvm_generation || !imp_dosvm_last_cpu || !imp_dosvm_trips ||
        !imp_dosvm_stage_com || !imp_dosvm_push_keys || !imp_dosvm_mem_rw)
        return KMOD_ERR_INVAL;
    kprintf_("[dosvm] facade loaded (syscall 210/211/212/213/214)\n");
    return KMOD_OK;
}

static void dosvm_facade_exit(void)
{
    if (kprintf_)
        kprintf_("[dosvm] facade unloaded (syscall 210/211)\n");
}

KMOD_EXPORT_SYSCALL(DOSVM_SYSCALL_READ, dosvm_read_syscall);
KMOD_EXPORT_SYSCALL(DOSVM_SYSCALL_ENTER, dosvm_enter_syscall);
KMOD_EXPORT_SYSCALL(DOSVM_SYSCALL_STAGE, dosvm_stage_syscall);
KMOD_EXPORT_SYSCALL(DOSVM_SYSCALL_KEYS, dosvm_keys_syscall);
KMOD_EXPORT_SYSCALL(DOSVM_SYSCALL_MEM, dosvm_mem_syscall);
KMOD_EXPORT_NAMESPACE("vendor.dosvm.syscall", dosvm_ns);

KMOD_MANIFEST_FULL("vendor.dosvm", "1.0.0", KMOD_TIER_IMPL,
                   (uint32_t)(KMOD_MF_BOOT | KMOD_MF_UNLOADABLE),
                   (uint64_t)(KMOD_CAP_SYSCALL_IMPL | KMOD_CAP_LOG |
                              KMOD_CAP_EXPERIMENTAL),
                   0, "dosvm: syscall facade 210/211/212/213 (Tier2)",
                   dosvm_facade_init, dosvm_facade_exit, 0, 0, 500u, 200u);
