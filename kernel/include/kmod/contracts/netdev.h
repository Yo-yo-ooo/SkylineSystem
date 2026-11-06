//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/netdev.h —— 私有契约 kernel.netdev.v1 / net.stack.v1
 * ============================================================================
 * 提供者：
 *   kernel.netdev.v1   —— 内建组件 kernel.net.netdev（网卡设备注册表）
 *   kernel.net.stack.v1 —— 内建组件 kernel.net.lwip（lwIP 协议栈生命周期）
 * 消费者：
 *   kernel.netdev.v1   —— 网卡驱动组件（B3 的 kernel.driver.e1000）在
 *                         E1000::Init() 成功后注册 netdev 描述；
 *                         kernel.net.lwip 遍历注册表创建 netif（当前版本
 *                         lwIP 胶水仍硬编码 e1000，见下"现状"）。
 *   kernel.net.stack.v1 —— 需要"起协议栈/查询状态"的组件或 .ko。
 *
 * 现状（诚实声明，不作为已完成能力宣传）：
 *   - 现有 kernel/src/drivers/net/e1000_lwip.cpp 的 NetStackInit() 直接调用
 *     E1000::* + netif_add()，**尚未消费本注册表**；本契约是给后续 .ko 网卡
 *     驱动/替换实现用的固定机制边界，B4 只做"注册表 + 生命周期组件化"。
 *   - TX/RX 方向：协议栈通过 set_rx_cb 向驱动注册收包回调；驱动收帧后调用
 *     该回调（event/IRQ 上下文由驱动自己保证不在其中跑协议栈，现有 e1000
 *     胶水用无锁 FIFO 把帧交给 lwIP 线程，模式不变）。
 *
 * 布局冻结：首部固定 { uint32_t version; uint32_t size; }，static_assert 钉死。
 * 错误约定：返回 0 = 成功，负值 = 负 errno。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACTS_NETDEV_H_
#define _KMOD_CONTRACTS_NETDEV_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMOD_CONTRACT_NETDEV_V1    "kernel.netdev.v1"
#define KMOD_CONTRACT_NET_STACK_V1 "kernel.net.stack.v1"

#define KMOD_NETDEV_OPS_VERSION    1u
#define KMOD_NETDEV_DESC_VERSION   1u
#define KMOD_NET_STACK_OPS_VERSION 1u

#define KMOD_NETDEV_NAME_MAX       16u   /* netif 名，如 "e0" */
#define KMOD_NETDEV_MAX_DEVICES    8u
#define KMOD_NETDEV_MAX_FRAME      2048u /* 单帧上限（含 FCS/填充） */

/* desc.flags */
#define KMOD_NETDEV_F_LINK_UP      (1u << 0)
#define KMOD_NETDEV_F_BROADCAST    (1u << 1)
#define KMOD_NETDEV_F_PROMISC      (1u << 2)
#define KMOD_NETDEV_F_IRQ_DRIVEN   (1u << 3)  /* 驱动以 IRQ 收帧；否则轮询 */

/* 收包回调：驱动收到完整以太网帧后调用。
 * frame 只在回调期间有效；返回 0 = 消费方已复制/借走，驱动可复用该缓冲。 */
typedef int32_t (*kmod_netdev_rx_fn)(void *arg, uint8_t *frame, uint16_t len,
                                     uint32_t flags);

/* 发送一帧。返回 0 = 已入环，负值 = 失败（-ENOSPC 环满 / -EIO）。 */
typedef int32_t (*kmod_netdev_tx_fn)(void *ctx, const uint8_t *frame,
                                     uint16_t len, uint32_t flags);

/* 链路状态：1 = up，0 = down。 */
typedef int32_t (*kmod_netdev_link_fn)(void *ctx);

/* ------------------------------------------------------------------ */
/* 网卡设备描述（驱动 -> net.netdev 注册表）。布局冻结：88 字节。      */
/* ------------------------------------------------------------------ */
typedef struct kmod_netdev_desc {
    uint32_t version;                       /* = KMOD_NETDEV_DESC_VERSION */
    uint32_t size;                          /* = sizeof(kmod_netdev_desc_t) */
    char name[KMOD_NETDEV_NAME_MAX];        /* "e0" */
    uint32_t if_index;                      /* 驱动内实例序号 */
    uint32_t mtu;                           /* 1500 典型 */
    uint32_t flags;                         /* KMOD_NETDEV_F_* */
    uint8_t mac[6];
    uint8_t mac_len;                        /* 6 */
    uint8_t reserved0;                      /* 0 */
    uint32_t reserved1;                     /* 0 */
    void *ctx;                              /* 驱动私有（不透明） */
    kmod_netdev_tx_fn tx;                   /* 必填 */
    kmod_netdev_link_fn link_up;            /* 可空 */
    int32_t (*set_rx_cb)(void *ctx, kmod_netdev_rx_fn fn, void *arg);
    uint32_t reserved[2];                   /* 必须为 0 */
} kmod_netdev_desc_t;

/* ------------------------------------------------------------------ */
/* net.netdev 注册表 ops。布局冻结：48 字节（8 + 5 个函数指针）。      */
/* ------------------------------------------------------------------ */
typedef struct kmod_netdev_ops {
    uint32_t version;                       /* = KMOD_NETDEV_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_netdev_ops_t) */
    /* 注册一份描述（内核拷贝，desc 可栈上）；成功返回设备句柄（不透明）。*/
    void *(*register_device)(const kmod_netdev_desc_t *desc);
    int32_t (*unregister_device)(const char *name);
    void *(*find)(const char *name);         /* 设备句柄，NULL = 不存在 */
    uint32_t (*count)(void);
    int32_t (*device_info)(void *dev, kmod_netdev_desc_t *out);
} kmod_netdev_ops_t;

/* ------------------------------------------------------------------ */
/* net.lwip 协议栈生命周期 ops。布局冻结：32 字节。                    */
/* ------------------------------------------------------------------ */
typedef struct kmod_net_stack_ops {
    uint32_t version;                       /* = KMOD_NET_STACK_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_net_stack_ops_t) */
    int32_t (*up)(void);                    /* 同步拉起（幂等；引导期不调用） */
    int32_t (*deferred_up)(void);           /* 起 deferred 内核线程（非阻塞） */
    /* 把已注册 netdev 接入协议栈。v1 现状：-ENOTSUP（lwIP 胶水仍硬编码
     * e1000，见文件头"现状"）；接入后语义 = 创建 netif 并 up。 */
    int32_t (*attach_device)(void *dev);
} kmod_net_stack_ops_t;

#if defined(__cplusplus)
static_assert(sizeof(kmod_netdev_desc_t) == 88u,
              "kmod_netdev_desc_t layout must stay 88 bytes (kernel.netdev.v1)");
static_assert(sizeof(kmod_netdev_ops_t) == 48u,
              "kmod_netdev_ops_t layout must stay 48 bytes (kernel.netdev.v1)");
static_assert(sizeof(kmod_net_stack_ops_t) == 32u,
              "kmod_net_stack_ops_t layout must stay 32 bytes (kernel.net.stack.v1)");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_CONTRACTS_NETDEV_H_ */
