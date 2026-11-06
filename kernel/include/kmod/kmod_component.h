//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_component.h —??内建组件 (builtin component) 模型
 * ============================================================================
 * 目标??*整个内核的组??*都进同一张组件图，而不只是外部 .ko?? *
 * 内核镜像里静态链接的组件（调度策略、分配器、文件系统、驱动、协议栈?? * syscall 实现家族……）与可加载 .ko 使用**完全相同**的线格式?? *   - manifest : kmod_manifest_t（名??版本/Tier/能力/生命周期/预算?? *   - .in      : kmod_import_t 数组（{provider ??+ 回填地址??+ 私有契约}?? *   - .out     : kmod_export_t 数组（函??数据/私有契约/座位/syscall 实现?? * Tier0 组件管理器在启动时扫描内建组件描述表，按拓扑图（provider 先序?? * 统一 bind/apply/init；因此内建组件与 .ko 可以?? *   - 同一个拓扑图共存??ko 可以导入内建组件导出的私有契约）?? *   - 共同参与能力治理/引用计数/quiesce/epoch?? *   - 内建组件可被 .ko **热替??*（回滚时恢复内建实现），这是"整个内核
 *     组件??的可替换性证据；Tier0 核心组件（boot/页表/中断/入口/加载??
 *     回滚/能力注册表）不在此列?? *
 * 实现机制（无动态构造函数，链接期静态布局）：
 *   每个组件 TU 里：
 *     KCOMPONENT_BEGIN(ID, "acme.fs.ext4", "1.0.0", KMOD_TIER_POLICY, flags,
 *                      req_caps, opt_caps, "desc", init, exit, quiesce, drain,
 *                      init_budget, quiesce_budget)
 *     KCOMPONENT_IMPORT(ID, kmalloc, kmalloc_fn_t, "kernel", "kmalloc",
 *                       "kernel.mem.v1")
 *     ...
 *     KCOMPONENT_EXPORT_CONTRACT(ID, fs_ops_var, "acme.fs.v1", 1)
 *     KCOMPONENT_END(ID)
 *   导入/导出条目落在 **每个组件独立的段** `.kbuiltin.in.<ID>` /
 *   `.kbuiltin.out.<ID>`，BEGIN/END 在段内放边界哨兵；END 再往
 *   `.kbuiltin.desc` 放一条描述符（manifest 指针 + in/out 边界）?? *   链接脚本保留 `.kbuiltin.desc` 并给??__kbuiltin_desc_start/end?? * ==========================================================================*/
#ifndef _KMOD_COMPONENT_H_
#define _KMOD_COMPONENT_H_

#include <kmod/kmod_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内建组件描述符魔数（??.ko 区分，防止误把原始段当描述符??*/
#define KMOD_BUILTIN_MAGIC  0x4B434F4D50454E44ull   /* "KCOMPEND" */

/* 组件条目边界哨兵（和条目交错放置在同一段内；加载期??8 字节步进跳过??*/
#define KMOD_BUILTIN_BEGIN_MAGIC 0x4B434F4D50454E42ull /* "KCOMPBEG" */
#define KMOD_BUILTIN_END_MAGIC   0x4B434F4D50454E44ull /* "KCOMPEND" */

/* 内建组件描述符：BEGIN/END 宏生成，链接期收集到 .kbuiltin.desc ??*/
typedef struct kmod_builtin_raw {
    uint64_t magic;                    /* KMOD_BUILTIN_MAGIC */
    const kmod_manifest_t *manifest;   /* 组件清单（普??.data??*/
    const uint8_t *in_begin;           /* .kbuiltin.in.<ID> 段内哨兵 */
    const uint8_t *in_end;
    const uint8_t *out_begin;          /* .kbuiltin.out.<ID> 段内哨兵 */
    const uint8_t *out_end;
} kmod_builtin_raw_t;

/* ---------------- 组件作者宏（与 .ko 宏一一对应??---------------- */

#define KMOD_COMP_STR_(x) #x
#define KMOD_COMP_STR(x) KMOD_COMP_STR_(x)
#define KMOD_COMP_SEC_IN_(id)  ".kbuiltin.in."  KMOD_COMP_STR(id)
#define KMOD_COMP_SEC_OUT_(id) ".kbuiltin.out." KMOD_COMP_STR(id)
#define KMOD_COMP_SEC_IN(id)   KMOD_COMP_SEC_IN_(id)
#define KMOD_COMP_SEC_OUT(id)  KMOD_COMP_SEC_OUT_(id)

/* 组件清单 + 段边界哨兵。必须在组件 TU 最前使用，随后 IMPORT/EXPORT?? * 最??KCOMPONENT_END(ID)。生命周期回调可??NULL??*/
#define KCOMPONENT_BEGIN(ID, nm_, ver_, tier_, flags_, req_caps_, opt_caps_, desc_, \
                         init_fn_, exit_fn_, quiesce_fn_, drain_fn_,               \
                         init_budget_, quiesce_budget_)                             \
    static kmod_manifest_t __kcomp_manifest_##ID                                      \
        __attribute__((used, aligned(8))) = {                                        \
        .magic = KMOD_MANIFEST_MAGIC,                                                \
        .abi_version = KMOD_ABI_VERSION,                                             \
        .tier = (uint16_t)(tier_),                                                   \
        .flags = (uint32_t)(flags_),                                                 \
        .name = nm_,                                                                 \
        .version = ver_,                                                             \
        .description = desc_,                                                        \
        .required_caps = (uint64_t)(req_caps_),                                      \
        .optional_caps = (uint64_t)(opt_caps_),                                      \
        .init = (init_fn_),                                                          \
        .exit_hook = (exit_fn_),                                                     \
        .quiesce = (quiesce_fn_),                                                    \
        .drain = (drain_fn_),                                                        \
        .init_budget_ms = (uint32_t)(init_budget_),                                  \
        .quiesce_budget_ms = (uint32_t)(quiesce_budget_),                            \
        .requires = 0, .n_requires = 0, .provides = 0, .n_provides = 0 };            \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))                 \
    static uint64_t __kcomp_in_begin_##ID = KMOD_BUILTIN_BEGIN_MAGIC;          \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))                \
    static uint64_t __kcomp_out_begin_##ID = KMOD_BUILTIN_BEGIN_MAGIC;

/* 导入（与 KMOD_IMPORT 同语义；ID 绑定组件??*/
#define KCOMPONENT_IMPORT(ID, name, type, provider_, symbol_, contract_) \
    static type name = 0;                                                 \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))      \
    static kmod_import_t __kimp_##ID##_##name = {                         \
        provider_, symbol_, contract_, (void **)&name, 1u, 0u, 0u,        \
        KMOD_IMP_REQUIRED };

#define KCOMPONENT_IMPORT_FN(ID, name, ret, params, provider_, symbol_, contract_) \
    static ret (*name) params = 0;                                                 \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))               \
    static kmod_import_t __kimp_##ID##_##name = {                                  \
        provider_, symbol_, contract_, (void **)&name, 1u, 0u, 0u,                 \
        KMOD_IMP_REQUIRED };

#define KCOMPONENT_DEPENDS(ID, provider_, tag_) \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))  \
    static kmod_import_t __kdep_##ID##_##tag_ = {                     \
        provider_, "", "", 0, 0u, 0u, 0u,                             \
        KMOD_IMP_REQUIRED | KMOD_IMP_DEP_EDGE };

/* 导出（与 KMOD_EXPORT_* 同语义） */
#define KCOMPONENT_EXPORT_FN(ID, fn, contract_, abi_) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_##ID##_##fn = {                        \
        #fn, contract_, (void *)&fn, 0u, (uint32_t)(abi_),             \
        KMOD_SYM_FUNC, 0u, 0u, 0u };

#define KCOMPONENT_EXPORT_DATA(ID, obj, contract_, abi_, size_) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_##ID##_##obj = {                       \
        #obj, contract_, (void *)&obj, (uint32_t)(size_),              \
        (uint32_t)(abi_), KMOD_SYM_DATA, 0u, 0u, 0u };

#define KCOMPONENT_EXPORT_CONTRACT(ID, contract_, ops_var, abi_) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_contract_##ID##_##ops_var = {          \
        contract_, contract_, (void *)&ops_var, (uint32_t)sizeof(ops_var), \
        (uint32_t)(abi_), KMOD_SYM_CONTRACT, 0u, 0u, 0u };

#define KCOMPONENT_EXPORT_SEAT(ID, seat_, ops_var, abi_) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_seat_##ID##_##ops_var = {              \
        seat_, seat_, (void *)&ops_var, (uint32_t)sizeof(ops_var),     \
        (uint32_t)(abi_), KMOD_SYM_SEAT, KMOD_EXF_HOTREPLACEABLE,      \
        0u, 0u };

#define KCOMPONENT_EXPORT_SYSCALL(ID, slot_, fn) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_syscall_##ID##_##fn = {                \
        #fn, KMOD_CONTRACT_KERNEL_SYSCALL, (void *)&fn, 0u, 1u,        \
        KMOD_SYM_SYSCALL, KMOD_EXF_HOTREPLACEABLE, (uint32_t)(slot_),  \
        0u };

/* 组件定义收尾：段内结束哨??+ 描述符登??*/
#define KCOMPONENT_END(ID) \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))   \
    static uint64_t __kcomp_in_end_##ID = KMOD_BUILTIN_END_MAGIC; \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static uint64_t __kcomp_out_end_##ID = KMOD_BUILTIN_END_MAGIC; \
    __attribute__((used, section(".kbuiltin.desc"), aligned(8)))       \
    static kmod_builtin_raw_t __kcomp_desc_##ID = {                    \
        KMOD_BUILTIN_MAGIC,                                            \
        &__kcomp_manifest_##ID,                                        \
        (const uint8_t *)&__kcomp_in_begin_##ID,                       \
        (const uint8_t *)&__kcomp_in_end_##ID,                         \
        (const uint8_t *)&__kcomp_out_begin_##ID,                      \
        (const uint8_t *)&__kcomp_out_end_##ID };

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_COMPONENT_H_ */
