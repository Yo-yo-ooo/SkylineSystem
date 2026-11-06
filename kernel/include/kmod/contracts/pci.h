//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/pci.h —— 私有契约 kernel.pci.v1 (PCI 设备驱动)
 * ============================================================================
 * 提供者组件: kernel.pci.core   （内建组件，Tier1 POLICY，导出 KMOD_SYM_CONTRACT）
 * 消费者:     kernel.driver.{ahci,nvme,xhci,e1000} 以及未来的 PCI .ko 驱动
 * 线格式: 纯 C 结构 + 版本/尺寸头字段；.ko 与内建组件用同一份布局。
 *
 * 两阶段设计（与 kernel.pci.enumerate 组件配合）：
 *   1) 各 driver 组件的 init 只调用 ops->register_driver() 登记 match tuple；
 *      此时**不触碰任何设备**（枚举尚未发生）；
 *   2) kernel.pci.enumerate 组件（依赖 kernel.pci.core + 全部 driver.*）的 init
 *      调用 ops->enumerate()，provider 内部按 ACPI MCFG 分支执行
 *      EnumeratePCI()/DoPCIWithoutMCFG()；枚举过程中命中 match 的驱动 probe 被
 *      即刻调用（与旧 init.cpp 的 scan->trigger 行为完全一致）。
 *
 * 约定：本契约不解释 probe 的语义；pci_device 是 provider 私有的不透明句柄，
 * 只能回传给本 ops 的 find/config/BAR 接口使用。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACT_PCI_H_
#define _KMOD_CONTRACT_PCI_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>   /* KMOD_ABI_STATIC_ASSERT */

#define KMOD_CONTRACT_PCI_V1     "kernel.pci.v1"
#define KMOD_PCI_ABI_V1          1u
#define KMOD_PCI_DRIVERS_MAX     16u   /* provider 侧注册槽上限（v1） */
#define KMOD_PCI_BARS_MAX        6u    /* PCI 配置空间 BAR0..BAR5 */

/* BAR 类型（与 provider 内部 PCI_BAR_TYPE_ENUM 解耦的线格式值） */
#define KMOD_PCI_BAR_NONE        0u
#define KMOD_PCI_BAR_MMIO32      1u
#define KMOD_PCI_BAR_MMIO64      2u
#define KMOD_PCI_BAR_IO          3u

/* 驱动 match 描述符：注册动作只登记 tuple 与 probe，不访问设备。
 * v1 provider 的匹配语义 = class_code/subclass/prog_if 三元组（与旧
 * PCI::PCIDriver 完全一致）；vendor_id/device_id 非 0 时作为附加过滤，
 * 0 = 任意（现有驱动全部填 0，行为不变）。 */
typedef struct kmod_pci_driver_desc {
    const char *name;          /* 驱动名（日志/审计） */
    uint16_t vendor_id;        /* 0 = 任意厂商 */
    uint16_t device_id;        /* 0 = 任意设备 */
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  reserved;         /* 必须 0 */
    void (*probe)(void *pci_device);  /* pci_device 为 provider 私有句柄 */
} kmod_pci_driver_desc_t;

/* 设备身份快照（审计/观测用；bus/slot/function 在 ECAM 枚举下由句柄地址解出） */
typedef struct kmod_pci_device_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;   /* 仅 header type 0 有意义，否则 0 */
    uint16_t subsystem_id;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint8_t  revision;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  reserved;
} kmod_pci_device_info_t;

/* BAR 描述：地址为 provider 已解出的直接可用地址/端口号。 */
typedef struct kmod_pci_bar_info {
    uint64_t address;          /* MMIO 地址 或 IO 端口号（按 type 解释） */
    uint64_t size;             /* 字节数；0 = 不可用 */
    uint32_t type;             /* KMOD_PCI_BAR_* */
    uint32_t index;            /* BAR 序号 0..5 */
} kmod_pci_bar_info_t;

/* 契约 ops。头两字段是机械校验的版本/尺寸，不得移动。 */
typedef struct kmod_pci_ops {
    uint32_t abi_version;      /* = KMOD_PCI_ABI_V1 */
    uint32_t ops_size;         /* = sizeof(kmod_pci_ops_t) */
    /* 登记 PCI 驱动 match/probe（两阶段的第一阶段；幂等，重复登记同 desc 返回 OK）。
     * 返回 KMOD_OK / 负错误码（槽满 KMOD_ERR_NOMEM、参数非法 KMOD_ERR_INVAL）。 */
    int32_t (*register_driver)(const kmod_pci_driver_desc_t *desc);
    /* 按 class/subclass/prog_if 查找已枚举设备；返回不透明句柄或 NULL。 */
    void *(*find_device)(uint8_t class_code, uint8_t subclass, uint8_t prog_if);
    /* 读取设备身份快照。 */
    int32_t (*device_info)(void *pci_device, kmod_pci_device_info_t *out);
    /* 读取 BAR（provider 负责 size probe，会临时覆盖 BAR 后恢复）。 */
    int32_t (*read_bar)(void *pci_device, uint32_t bar_index,
                        kmod_pci_bar_info_t *out);
    /* 原始配置空间 32 位读写（reg 按 dword 对齐；越界由 provider 钳制）。 */
    uint32_t (*config_read32)(void *pci_device, uint32_t reg);
    void (*config_write32)(void *pci_device, uint32_t reg, uint32_t value);
    /* 触发枚举（第二阶段；幂等：重复调用直接返回 KMOD_OK）。
     * ACPI MCFG 存在 -> EnumeratePCI(mcfg)，否则 DoPCIWithoutMCFG()。 */
    int32_t (*enumerate)(void);
} kmod_pci_ops_t;

/* ABI 布局冻结：任何字段变更必须提升 KMOD_PCI_ABI_V1 并保留旧结构。 */
KMOD_ABI_STATIC_ASSERT(sizeof(kmod_pci_driver_desc_t) == 24u,
                       "kmod_pci_driver_desc_t ABI freeze");
KMOD_ABI_STATIC_ASSERT(sizeof(kmod_pci_device_info_t) == 16u,
                       "kmod_pci_device_info_t ABI freeze");
KMOD_ABI_STATIC_ASSERT(sizeof(kmod_pci_bar_info_t) == 24u,
                       "kmod_pci_bar_info_t ABI freeze");
KMOD_ABI_STATIC_ASSERT(sizeof(kmod_pci_ops_t) == 64u,
                       "kmod_pci_ops_t ABI freeze");

#endif /* _KMOD_CONTRACT_PCI_H_ */
