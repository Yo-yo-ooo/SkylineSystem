//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/acpi.h —— 私有契约 kernel.acpi.v1
 * ============================================================================
 * 提供者组件: kernel.arch.acpi（内建组件，Tier1 POLICY，导出 KMOD_SYM_CONTRACT）
 * 消费者:     kernel.arch.apic / kernel.pci.core / 未来平台驱动 .ko / 组件
 * 线格式:     纯 C 结构 + 版本/尺寸头字段；.ko 与内建组件共用同一布局。
 *
 * 语义：ACPI 根表（RSDP -> RSDT/XSDT）与 MADT/MCFG 表的**只读查询**。
 * 本契约不解释表内容、不做 AML 解释、不提供唤醒/电源管理动作 —— 只是把
 * Tier0 已经解析过的表指针机械地暴露给平台组件。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACT_ACPI_H_
#define _KMOD_CONTRACT_ACPI_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>   /* KMOD_ABI_STATIC_ASSERT */

#define KMOD_CONTRACT_ACPI_V1   "kernel.acpi.v1"
#define KMOD_ACPI_ABI_V1        1u

/* 契约 ops。头两字段是机械校验的版本/尺寸，不得移动。 */
typedef struct kmod_acpi_ops {
    uint32_t abi_version;   /* = KMOD_ACPI_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_acpi_ops_t) */

    /* ACPI 根表是否已成功解析（0 = 不可用，其余接口返回 NULL/安全空值）。 */
    int32_t (*available)(void);
    /* MADT 表指针（provider 缓存的只读视图；未找到返回 NULL）。 */
    void *(*madt)(void);
    /* MCFG 表指针（未找到返回 NULL；PCI 枚举按此分支）。 */
    void *(*mcfg)(void);
    /* 按 4 字符 signature 查表（ACPICA 风格；不允许 NUL 之外的多余字符）。 */
    void *(*find_table)(const char *signature);
} kmod_acpi_ops_t;

KMOD_ABI_STATIC_ASSERT(sizeof(kmod_acpi_ops_t) == 40u,
                       "kmod_acpi_ops_t ABI freeze");

#endif /* _KMOD_CONTRACT_ACPI_H_ */
