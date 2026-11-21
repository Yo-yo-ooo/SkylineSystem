//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_component.h — resident DynModule 模型（原内建组件）
 * ============================================================================
 * Tier1/Tier2 已合并：本文件是统一 DynModule 的 resident 后端（代码已在
 * 内核镜像内）。relocatable 后端（外部 .ko）用 kmod_module.h；两者共享
 * 同一 manifest/.in/.out、拓扑、能力与生命周期，并都由 KdllInject 注入。
 *
 * 目标：整个内核的组件都进同一张组件图，而不只是外部 .ko。
 *
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

/* 带版本区间/结构尺寸校验的契约导入（与 KMOD_IMPORT_EX 同语义）。 */
#define KCOMPONENT_IMPORT_EX(ID, name, type, provider_, symbol_, contract_, \
                             vmin_, vmax_, size_)                          \
    static type name = 0;                                                  \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))       \
    static kmod_import_t __kimp_##ID##_##name = {                          \
        provider_, symbol_, contract_, (void **)&name, (uint32_t)(vmin_),  \
        (uint32_t)(vmax_), (uint32_t)(size_), KMOD_IMP_REQUIRED };

#define KCOMPONENT_IMPORT_DYNLINK(ID, name) \
    KCOMPONENT_IMPORT_EX(ID, name, const kmod_dynlink_ops_t *, "kernel", \
                         KMOD_CONTRACT_KERNEL_DYNLINK,                   \
                         KMOD_CONTRACT_KERNEL_DYNLINK,                   \
                         KMOD_DYNLINK_OPS_ABI, 0u,                       \
                         (uint32_t)sizeof(kmod_dynlink_ops_t))

#define KCOMPONENT_DEPENDS(ID, provider_, tag_) \
    __attribute__((used, section(KMOD_COMP_SEC_IN(ID)), aligned(8)))  \
    static kmod_import_t __kdep_##ID##_##tag_ = {                     \
        provider_, "", "", 0, 0u, 0u, 0u,                             \
        KMOD_IMP_REQUIRED | KMOD_IMP_DEP_EDGE };                      \
    __attribute__((used, section(KMOD_SEC_DEP_NAMES)))                \
    static const char __kdepname_##ID##_##tag_[] = provider_;

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

/* 实现已随组件迁入本 .ko：地址在本镜像内，不需要 EXTERNAL 标记。 */
#define KCOMPONENT_EXPORT_SYSCALL_LOCAL(ID, slot_, fn) \
    KCOMPONENT_EXPORT_SYSCALL(ID, slot_, fn)

/* 声明动态 syscall 命名空间（自省/审计；与 KMOD_EXPORT_NAMESPACE 同义）。 */
#define KCOMPONENT_EXPORT_NAMESPACE(ID, ns_, tag_) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_ns_##ID##_##tag_ = {                   \
        ns_, KMOD_CONTRACT_KERNEL_SYSCALL, 0, 0u, 1u,                  \
        KMOD_SYM_NAMESPACE, 0u, 0u, 0u };

/* 按 ELF 符号名原样导出：把已迁入本 .ko 的实现函数暴露给其他 .ko 的
 * 未定义符号解析器（符号名必须与 nm 的 mangled name 完全一致）。 */
#define KCOMPONENT_EXPORT_RAW(ID, tag_, sym_, addr_, size_, abi_, kind_, flags_, aux_) \
    __attribute__((used, section(KMOD_COMP_SEC_OUT(ID)), aligned(8)))  \
    static kmod_export_t __kexp_raw_##ID##_##tag_ = {                  \
        sym_, "kernel.component.v1", (void *)(uintptr_t)(addr_), \
        (uint32_t)(size_), (uint32_t)(abi_), (kind_), (flags_), \
        (uint32_t)(aux_), 0u };

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

/* ==========================================================================
 * KMOD_COMPONENT_AS_KO：把同一份组件壳源码编译成独立 ET_REL .ko
 * --------------------------------------------------------------------------
 * 原 Tier 1 组件壳（KCOMPONENT_BEGIN/END）现在有两种构建方式：
 *   - 默认：resident，编进内核镜像，.kbuiltin.* 段（兼容旧路径）；
 *   - -DKMOD_COMPONENT_AS_KO=1：独立 .ko，manifest/.in/.out 落到
 *     .kmod_manifest/.in/.out，由 Limine 的 modules.saf 载入。
 * 壳代码本身不改；实现代码仍在 kernel 镜像内，壳通过 kernel.component.v1
 * 的生成导出表解析实现符号（loader 的 .kout 路径）。
 * ========================================================================== */
#ifdef KMOD_COMPONENT_AS_KO

#include <kmod/kmod_module.h>

#undef KCOMPONENT_BEGIN
#undef KCOMPONENT_END
#undef KCOMPONENT_IMPORT
#undef KCOMPONENT_IMPORT_FN
#undef KCOMPONENT_IMPORT_EX
#undef KCOMPONENT_IMPORT_DYNLINK
#undef KCOMPONENT_DEPENDS
#undef KCOMPONENT_EXPORT_FN
#undef KCOMPONENT_EXPORT_DATA
#undef KCOMPONENT_EXPORT_CONTRACT
#undef KCOMPONENT_EXPORT_SEAT
#undef KCOMPONENT_EXPORT_SYSCALL
#undef KCOMPONENT_EXPORT_SYSCALL_LOCAL
#undef KCOMPONENT_EXPORT_NAMESPACE
#undef KCOMPONENT_EXPORT_RAW

/* 组件 manifest -> .kmod_manifest；所有组件 .ko 都是启动模块。 */
#define KCOMPONENT_BEGIN(ID, nm_, ver_, tier_, flags_, req_caps_, opt_caps_, \
                         desc_, init_fn_, exit_fn_, quiesce_fn_, drain_fn_,   \
                         init_budget_, quiesce_budget_)                       \
    KMOD_MANIFEST_FULL(nm_, ver_, tier_,                                  \
                       (uint32_t)((flags_) | KMOD_MF_BOOT),               \
                       req_caps_, opt_caps_, desc_, init_fn_, exit_fn_,   \
                       quiesce_fn_, drain_fn_, init_budget_, quiesce_budget_)

#define KCOMPONENT_END(ID) /* 无 builtin 描述符 */

#define KCOMPONENT_IMPORT(ID, name, type, provider_, symbol_, contract_) \
    KMOD_IMPORT(name, type, provider_, symbol_, contract_)

#define KCOMPONENT_IMPORT_FN(ID, name, ret, params, provider_, symbol_, contract_) \
    KMOD_IMPORT_FN(name, ret, params, provider_, symbol_, contract_)

#define KCOMPONENT_IMPORT_EX(ID, name, type, provider_, symbol_, contract_, \
                             vmin_, vmax_, size_)                          \
    KMOD_IMPORT_EX(name, type, provider_, symbol_, contract_, vmin_, vmax_, size_)

#define KCOMPONENT_IMPORT_DYNLINK(ID, name) KMOD_IMPORT_DYNLINK(name)

#define KCOMPONENT_DEPENDS(ID, provider_, tag_) KMOD_DEPENDS(provider_, tag_)

#define KCOMPONENT_EXPORT_FN(ID, fn, contract_, abi_) \
    KMOD_EXPORT_FN(fn, contract_, abi_)

#define KCOMPONENT_EXPORT_DATA(ID, obj, contract_, abi_, size_) \
    KMOD_EXPORT_DATA(obj, contract_, abi_, size_)

#define KCOMPONENT_EXPORT_CONTRACT(ID, contract_, ops_var, abi_) \
    KMOD_EXPORT_CONTRACT(contract_, ops_var, abi_)

#define KCOMPONENT_EXPORT_SEAT(ID, seat_, ops_var, abi_) \
    KMOD_EXPORT_SEAT(seat_, ops_var, abi_)

/* syscall 实现函数在 kernel 镜像里，.ko 只持有地址：需要 EXTERNAL 标记。 */
#define KCOMPONENT_EXPORT_SYSCALL(ID, slot_, fn) \
    KMOD_EXPORT_SYSCALL_EXTERNAL(slot_, fn)
#define KCOMPONENT_EXPORT_SYSCALL_LOCAL(ID, slot_, fn) \
    KMOD_EXPORT_SYSCALL(slot_, fn)

#define KCOMPONENT_EXPORT_NAMESPACE(ID, ns_, tag_) \
    KMOD_EXPORT_NAMESPACE(ns_, tag_)

#define KCOMPONENT_EXPORT_RAW(ID, tag_, sym_, addr_, size_, abi_, kind_, flags_, aux_) \
    __attribute__((used, section(KMOD_SEC_OUT), aligned(8)))           \
    static kmod_export_t __kexp_raw_##ID##_##tag_ = {                  \
        sym_, "kernel.component.v1", (void *)(uintptr_t)(addr_), \
        (uint32_t)(size_), (uint32_t)(abi_), (kind_), (flags_), \
        (uint32_t)(aux_), 0u };

#endif /* KMOD_COMPONENT_AS_KO */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_COMPONENT_H_ */
