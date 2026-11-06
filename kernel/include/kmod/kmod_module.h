//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_module.h —— .ko 模块作者使用的声明宏（模块侧）
 * ============================================================================
 * 用法骨架（完整示例见 kernel/modules/demo_*.cpp 与 docs/kernel-modules.md）：
 *
 *   #include <kmod/kmod_module.h>
 *
 *   KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
 *                  "kernel.log.v1");
 *
 *   static int32_t hello_init(void) { kprintf_("hello from ko\n"); return KMOD_OK; }
 *   static void hello_exit(void) {}
 *
 *   KMOD_EXPORT_FN(hello_fn, "vendor.demo.hello.v1", 1);
 *   KMOD_MANIFEST("vendor.demo.hello", "1.0.0", KMOD_TIER_IMPL,
 *                 KMOD_MF_BOOT, KMOD_CAP_LOG, hello_init, hello_exit);
 *
 * 约束：
 *   - .ko 用 -fno-pic -fno-pie -mcmodel=large 编译（见 kernel/Makefile 的
 *     modules 目标）。加载器只接受有限重定位集，遇到未知类型整体拒绝；
 *   - 不得引用未声明导入的外部符号（编译器可能为 memcpy/memset 生成外部
 *     引用，这些由内核 Tier0 导出表兜底解析）；
 *   - 模块间只通过私有契约/导出符号互相约定，内核不做语义转换。
 * ==========================================================================*/
#ifndef _KMOD_MODULE_H_
#define _KMOD_MODULE_H_

#include <kmod/kmod_abi.h>
#include <kmod/kmod_services.h>   /* kernel.klib.v1 / kernel.sync.v1 / kernel.algo.v1 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 导入（.in 段）                                                      */
/* ------------------------------------------------------------------ */

/* 泛型导入：type 可为函数指针 typedef 或数据指针类型。
 * 用法：KMOD_IMPORT(kmalloc, kmalloc_fn_t, "kernel", "kmalloc", "kernel.mem.v1") */
#define KMOD_IMPORT(name, type, provider_, symbol_, contract_) \
    static type name = 0;                                                    \
    __attribute__((used, section(KMOD_SEC_IN), aligned(8)))                  \
    static kmod_import_t __kmod_imp_##name = {                               \
        provider_, symbol_, contract_, (void **)&name, 1u, 0u, 0u,           \
        KMOD_IMP_REQUIRED };

/* 函数指针导入：params 含形参表（可为 varargs），直接以 name(args) 调用 */
#define KMOD_IMPORT_FN(name, ret, params, provider_, symbol_, contract_) \
    static ret (*name) params = 0;                                           \
    __attribute__((used, section(KMOD_SEC_IN), aligned(8)))                  \
    static kmod_import_t __kmod_imp_##name = {                               \
        provider_, symbol_, contract_, (void **)&name, 1u, 0u, 0u,           \
        KMOD_IMP_REQUIRED };

/* 可选导入：provider 未加载/符号缺失时加载继续，槽保持 NULL（用 KMOD_IMPORT_READY 判断） */
#define KMOD_IMPORT_OPTIONAL(name, type, provider_, symbol_, contract_) \
    static type name = 0;                                                    \
    __attribute__((used, section(KMOD_SEC_IN), aligned(8)))                  \
    static kmod_import_t __kmod_imp_##name = {                               \
        provider_, symbol_, contract_, (void **)&name, 0u, 0u, 0u,           \
        KMOD_IMP_OPTIONAL };

/* 带版本区间/结构尺寸校验的完整导入 */
#define KMOD_IMPORT_EX(name, type, provider_, symbol_, contract_, vmin, vmax, size_) \
    static type name = 0;                                                    \
    __attribute__((used, section(KMOD_SEC_IN), aligned(8)))                  \
    static kmod_import_t __kmod_imp_##name = {                               \
        provider_, symbol_, contract_, (void **)&name, (uint32_t)(vmin),     \
        (uint32_t)(vmax), (uint32_t)(size_), KMOD_IMP_REQUIRED };

/* 纯模块级依赖边（不带符号回填）：用于声明加载顺序/生命周期依赖 */
#define KMOD_DEPENDS(provider_, dep_tag_) \
    __attribute__((used, section(KMOD_SEC_IN), aligned(8)))                  \
    static kmod_import_t __kmod_dep_##dep_tag_ = {                           \
        provider_, "", "", 0, 0u, 0u, 0u, KMOD_IMP_REQUIRED | KMOD_IMP_DEP_EDGE };

/* 导入是否已回填（供 optional import 使用） */
#define KMOD_IMPORT_READY(name) ((name) != 0)

/* ------------------------------------------------------------------ */
/* Tier0 共享服务导入（kernel.klib.v1 / kernel.sync.v1 / kernel.algo.v1） */
/* ------------------------------------------------------------------ */
/* 用法（.ko 与内建组件都可）：
 *   KMOD_IMPORT_KLIB(klib);   KMOD_IMPORT_SYNC(sync);   KMOD_IMPORT_ALGO(algo);
 *   init() { klib->strlen("x"); sync->irq_save(); ... }
 * 导入项带精确 expect_size=sizeof(ops)，内核绑定阶段做 contract/abi/size
 * 机械校验；不匹配则整个加载事务回滚（不会出现半绑定）。
 * 另外，klib 中 C 链接函数（strlen/strcmp/sprintf_/snprintf_、hashmap_* 与
 * art_* 容器、spinlock_* 等）已加入内核 .kout 导出表：模块可直接声明
 * extern "C" 后调用，加载期按 ELF 未定义符号名直接解析，无需 .in 声明。 */
#define KMOD_IMPORT_KLIB(name)                                                \
    KMOD_IMPORT_EX(name, const kmod_klib_ops_t *, "kernel",                   \
                   KMOD_CONTRACT_KERNEL_KLIB, KMOD_CONTRACT_KERNEL_KLIB,       \
                   1u, 0u, (uint32_t)sizeof(kmod_klib_ops_t));

#define KMOD_IMPORT_SYNC(name)                                                \
    KMOD_IMPORT_EX(name, const kmod_sync_ops_t *, "kernel",                   \
                   KMOD_CONTRACT_KERNEL_SYNC, KMOD_CONTRACT_KERNEL_SYNC,       \
                   1u, 0u, (uint32_t)sizeof(kmod_sync_ops_t));

#define KMOD_IMPORT_ALGO(name)                                                \
    KMOD_IMPORT_EX(name, const kmod_algo_ops_t *, "kernel",                   \
                   KMOD_CONTRACT_KERNEL_ALGO, KMOD_CONTRACT_KERNEL_ALGO,       \
                   1u, 0u, (uint32_t)sizeof(kmod_algo_ops_t));

/* ------------------------------------------------------------------ */
/* 导出（.out 段）                                                     */
/* ------------------------------------------------------------------ */

/* 导出函数（跨模块调用只能经由导入槽/导出表，内核回填绝对地址） */
#define KMOD_EXPORT_FN(fn, contract_, abi_) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))                 \
    static kmod_export_t __kmod_exp_##fn = {                                 \
        #fn, contract_, (void *)&fn, 0u, (uint32_t)(abi_), KMOD_SYM_FUNC,    \
        0u, 0u, 0u };

/* 导出数据对象 */
#define KMOD_EXPORT_DATA(obj, contract_, abi_, size_) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))                 \
    static kmod_export_t __kmod_exp_##obj = {                                \
        #obj, contract_, (void *)&obj, (uint32_t)(size_), (uint32_t)(abi_),  \
        KMOD_SYM_DATA, 0u, 0u, 0u };

/* 导出私有契约 ops：symbol = contract 命名空间，size = sizeof(ops_var)。
 * 消费者用 KMOD_IMPORT_EX(..., vmin, vmax, sizeof(consumer_expected_ops)) 绑定，
 * 尺寸/版本不匹配即加载失败（内核只做机械校验，不解释语义）。 */
#define KMOD_EXPORT_CONTRACT(contract_, ops_var, abi_) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))                 \
    static kmod_export_t __kmod_exp_contract_##ops_var = {                   \
        contract_, contract_, (void *)&ops_var, (uint32_t)sizeof(ops_var),   \
        (uint32_t)(abi_), KMOD_SYM_CONTRACT, 0u, 0u, 0u };

/* 导出 Tier1 座位实现（内核固定机制的策略/实现提供者）。
 * seat 名必须已由 Tier0 内核注册（如 KMOD_SEAT_RANDOM），ops 尺寸必须与
 * 内核座位 ABI 完全一致，否则拒绝加载。 */
#define KMOD_EXPORT_SEAT(seat_, ops_var, abi_) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))                 \
    static kmod_export_t __kmod_exp_seat_##ops_var = {                       \
        seat_, seat_, (void *)&ops_var, (uint32_t)sizeof(ops_var),           \
        (uint32_t)(abi_), KMOD_SYM_SEAT, KMOD_EXF_HOTREPLACEABLE, 0u, 0u };

/* 导出 syscall 实现：slot 必须在动态命名空间区间
 * [KMOD_SYSCALL_DYNAMIC_MIN, KMOD_SYSCALL_DYNAMIC_MAX]，或指向被标记为
 * KMOD_SC_SWAPPABLE 的固定契约槽（覆写实现，语义不变）。
 * 实现函数签名必须与固定 dispatch ABI 一致（见 kmod_syscall_fn 内核定义）：
 *   uint64_t fn(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
 *               uint64_t a4, uint64_t a5, void *frame); */
#define KMOD_EXPORT_SYSCALL(slot_, fn) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))                 \
    static kmod_export_t __kmod_exp_syscall_##fn = {                         \
        #fn, KMOD_CONTRACT_KERNEL_SYSCALL, (void *)&fn, 0u, 1u,              \
        KMOD_SYM_SYSCALL, KMOD_EXF_HOTREPLACEABLE, (uint32_t)(slot_), 0u };

/* 声明本模块拥有的动态 syscall 命名空间（审计/自省用；不影响回填规则）。
 * tag_ 为模块内唯一标记（模块名简写即可），用于生成不冲突的变量名。 */
#define KMOD_EXPORT_NAMESPACE(ns_, tag_) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))                 \
    static kmod_export_t __kmod_exp_ns_##tag_ = {                            \
        ns_, KMOD_CONTRACT_KERNEL_SYSCALL, 0, 0u, 1u, KMOD_SYM_NAMESPACE,    \
        0u, 0u, 0u };

/* ------------------------------------------------------------------ */
/* 模块清单（.kmod_manifest 段）                                       */
/* ------------------------------------------------------------------ */

/* 完整清单（生命周期回调可为 NULL；budget 0 = 内核默认） */
#define KMOD_MANIFEST_FULL(nm_, ver_, tier_, flags_, req_caps_, opt_caps_, desc_, \
                           init_fn_, exit_fn_, quiesce_fn_, drain_fn_,           \
                           init_budget_, quiesce_budget_)                         \
    static kmod_manifest_t __kmod_manifest                                      \
        __attribute__((used, section(KMOD_SEC_MANIFEST), aligned(8))) = {       \
        .magic = KMOD_MANIFEST_MAGIC,                                           \
        .abi_version = KMOD_ABI_VERSION,                                        \
        .tier = (uint16_t)(tier_),                                              \
        .flags = (uint32_t)(flags_),                                            \
        .name = nm_,                                                            \
        .version = ver_,                                                        \
        .description = desc_,                                                   \
        .required_caps = (uint64_t)(req_caps_),                                 \
        .optional_caps = (uint64_t)(opt_caps_),                                 \
        .init = (init_fn_),                                                     \
        .exit_hook = (exit_fn_),                                                \
        .quiesce = (quiesce_fn_),                                               \
        .drain = (drain_fn_),                                                   \
        .init_budget_ms = (uint32_t)(init_budget_),                             \
        .quiesce_budget_ms = (uint32_t)(quiesce_budget_),                       \
        .requires = 0, .n_requires = 0, .provides = 0, .n_provides = 0 };

/* 常用简写 */
#define KMOD_MANIFEST(nm_, ver_, tier_, flags_, req_caps_, init_fn_, exit_fn_) \
    KMOD_MANIFEST_FULL(nm_, ver_, tier_, flags_, req_caps_, 0, "",              \
                       init_fn_, exit_fn_, 0, 0, 0u, 0u)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_MODULE_H_ */
