//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * net_netdev.cpp —— 内建组件 kernel.net.netdev（网卡设备注册表）
 * ----------------------------------------------------------------------------
 * 定位：网络设备抽象的**注册表组件**，导出 kernel.netdev.v1（见
 * kernel/include/kmod/contracts/netdev.h）。网卡驱动（B3 的
 * kernel.driver.e1000，或未来 .ko 驱动）在 E1000::Init() 成功后导入本契约并
 * register_device(desc)；协议栈侧（kernel.net.lwip 或未来 .ko 协议栈）可以
 * find/count 遍历注册表。
 *
 * 与现有实现的关系（行为不变性）：
 *   - 现有 kernel/src/drivers/net/e1000_lwip.cpp 的 NetStackInit() 仍直接调用
 *     E1000::Send/SetInputCallback/netif_add，**不消费本注册表**；本组件只加
 *     "独立注册表 + 固定契约"，不改动现有网卡/协议栈任何代码路径；
 *   - e1000 组件（B3）默认只注册 PCI match，不注册 netdev（时间紧则留白，
 *     已与 mod-demos 对齐）；因此本组件 init 后设备数可以为 0，这是正常的，
 *     不返回错误（QEMU 无网卡也能启动）。
 *
 * 依赖：无内建组件前置依赖（纯 Tier0 堆/自旋锁 + 日志）；lwip 组件通过导入
 * 本契约自动形成 provider 先序边。任务描述里的 "依赖 core" 对应 Tier0 内核
 * 节点（"kernel" provider），没有独立的 kernel.core 组件。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/netdev.h>

#include <arch/x86_64/smp/smp.h>
#include <klib/errno.h>
#include <klib/klib.h>
#include <mem/heap.h>

static int32_t comp_net_netdev_init(void);
static void comp_net_netdev_exit(void);

KCOMPONENT_BEGIN(net_netdev, "kernel.net.netdev", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_NET | KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC),
                 0, "NIC device registry (kernel.netdev.v1); drivers register, stacks consume",
                 comp_net_netdev_init, comp_net_netdev_exit, 0, 0, 500u, 100u)

KCOMPONENT_IMPORT_FN(net_netdev, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")

namespace {

struct NetdevEntry {
    bool used;
    kmod_netdev_desc_t desc;   /* 注册表持有副本（调用方栈对象生命周期不受限） */
};

NetdevEntry g_devs[KMOD_NETDEV_MAX_DEVICES];
spinlock_t g_devs_lock = 0;
bool g_netdev_ready = false;

bool valid_desc(const kmod_netdev_desc_t *desc)
{
    if (!desc)
        return false;
    if (desc->version != KMOD_NETDEV_DESC_VERSION ||
        desc->size != (uint32_t)sizeof(kmod_netdev_desc_t))
        return false;
    if (!desc->tx)
        return false;
    if (desc->mac_len != 6u)
        return false;
    if (!desc->name[0])
        return false;
    for (uint32_t i = 0; i < KMOD_NETDEV_NAME_MAX; i++) {
        if (desc->name[i] == '\0')
            return true;
    }
    return false; /* 无 NUL 终止 -> 名字非法 */
}

bool same_name(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

void *netdev_register_device(const kmod_netdev_desc_t *desc)
{
    if (!valid_desc(desc))
        return nullptr;
    void *handle = nullptr;
    spinlock_lock(&g_devs_lock);
    for (uint32_t i = 0; i < KMOD_NETDEV_MAX_DEVICES; i++) {
        /* 同名重复注册 = 驱动重入/热替换：覆盖旧描述更安全（旧 handle 仍有效，
         * 因为 handle 指向槽位而不是描述内容本身）。 */
        if (g_devs[i].used && same_name(g_devs[i].desc.name, desc->name)) {
            g_devs[i].desc = *desc;
            handle = (void *)&g_devs[i];
            goto out;
        }
    }
    for (uint32_t i = 0; i < KMOD_NETDEV_MAX_DEVICES; i++) {
        if (!g_devs[i].used) {
            g_devs[i].used = true;
            g_devs[i].desc = *desc;
            handle = (void *)&g_devs[i];
            goto out;
        }
    }
out:
    spinlock_unlock(&g_devs_lock);
    return handle;
}

int32_t netdev_unregister_device(const char *name)
{
    if (!name || !name[0])
        return -EINVAL;
    int32_t rc = -ENOENT;
    spinlock_lock(&g_devs_lock);
    for (uint32_t i = 0; i < KMOD_NETDEV_MAX_DEVICES; i++) {
        if (g_devs[i].used && same_name(g_devs[i].desc.name, name)) {
            g_devs[i].used = false;
            _memset(&g_devs[i].desc, 0, sizeof(g_devs[i].desc));
            rc = 0;
            break;
        }
    }
    spinlock_unlock(&g_devs_lock);
    return rc;
}

void *netdev_find(const char *name)
{
    if (!name || !name[0])
        return nullptr;
    void *handle = nullptr;
    spinlock_lock(&g_devs_lock);
    for (uint32_t i = 0; i < KMOD_NETDEV_MAX_DEVICES; i++) {
        if (g_devs[i].used && same_name(g_devs[i].desc.name, name)) {
            handle = (void *)&g_devs[i];
            break;
        }
    }
    spinlock_unlock(&g_devs_lock);
    return handle;
}

uint32_t netdev_count(void)
{
    uint32_t n = 0;
    spinlock_lock(&g_devs_lock);
    for (uint32_t i = 0; i < KMOD_NETDEV_MAX_DEVICES; i++) {
        if (g_devs[i].used)
            n++;
    }
    spinlock_unlock(&g_devs_lock);
    return n;
}

int32_t netdev_device_info(void *dev, kmod_netdev_desc_t *out)
{
    if (!dev || !out)
        return -EINVAL;
    if (out->version != KMOD_NETDEV_DESC_VERSION ||
        out->size != (uint32_t)sizeof(kmod_netdev_desc_t))
        return -EINVAL;
    NetdevEntry *e = static_cast<NetdevEntry *>(dev);
    int32_t rc = -ENOENT;
    spinlock_lock(&g_devs_lock);
    if (e >= &g_devs[0] && e < &g_devs[KMOD_NETDEV_MAX_DEVICES] && e->used) {
        *out = e->desc;
        rc = 0;
    }
    spinlock_unlock(&g_devs_lock);
    return rc;
}

kmod_netdev_ops_t g_netdev_ops = {
    KMOD_NETDEV_OPS_VERSION,
    (uint32_t)sizeof(kmod_netdev_ops_t),
    netdev_register_device,
    netdev_unregister_device,
    netdev_find,
    netdev_count,
    netdev_device_info,
};

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(net_netdev, KMOD_CONTRACT_NETDEV_V1, g_netdev_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_net_netdev_init(void)
{
    if (g_netdev_ready)
        return KMOD_OK; /* 幂等 */
    _memset(g_devs, 0, sizeof(g_devs));
    g_netdev_ready = true;
    kprintf_("[net.netdev] ready: registry %u slots, kernel.netdev.v1 published "
             "(0 devices is normal until a NIC driver registers)\n",
             (unsigned)KMOD_NETDEV_MAX_DEVICES);
    return KMOD_OK;
}

static void comp_net_netdev_exit(void)
{
    spinlock_lock(&g_devs_lock);
    _memset(g_devs, 0, sizeof(g_devs));
    spinlock_unlock(&g_devs_lock);
    g_netdev_ready = false;
    kprintf_("[net.netdev] exit: registry cleared\n");
}

KCOMPONENT_END(net_netdev)
