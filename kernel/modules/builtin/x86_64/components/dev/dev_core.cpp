//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * dev_core.cpp —— 内建组件 kernel.dev.core (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：
 *   InitFunc("VsDev", Dev::Init());           // 原 line 225
 *   InitFunc("File & MP MAN", InitFFMAN());   // 原 line 226
 *
 * 依赖 sched.core：Dev::Init 的 hashmap / InitFFMAN 的挂载点表在旧顺序里位于
 * Schedule 之后（原 line 209/210 -> 225/226）；B3/B4 的驱动与 VFS 组件都依赖
 * 本组件提供 Dev 注册表与挂载点表。
 *
 * 导出契约 kernel.dev.v1（v1 内联固定布局）：is_ready()。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <drivers/dev/dev.h>
#include <fs/fd.h>   /* InitFFMAN */

#define KMOD_CONTRACT_DEV_V1 "kernel.dev.v1"
#define KMOD_DEV_ABI_V1      1u

static int32_t dev_core_init(void);
static void dev_core_exit(void);

KCOMPONENT_BEGIN(dev_core, "kernel.dev.core", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_DEVICE |
                                          KMOD_CAP_LOG),
                 0, "Dev registry + file/mount manager (Dev::Init + InitFFMAN)",
                 dev_core_init, dev_core_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(dev_core, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(dev_core, "kernel.sched.core", sched_core)

namespace {

typedef struct kmod_dev_ops {
    uint32_t abi_version;   /* = KMOD_DEV_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_dev_ops_t) */
    int32_t (*is_ready)(void);
} kmod_dev_ops_t;

bool g_dev_ready = false;

int32_t dev_is_ready(void)
{
    return g_dev_ready ? 1 : 0;
}

const kmod_dev_ops_t g_dev_ops = {
    KMOD_DEV_ABI_V1,
    (uint32_t)sizeof(kmod_dev_ops_t),
    dev_is_ready,
};

} /* namespace */

extern "C" void mount_set_lookup(__hmap_s_mp *(*fn)(const char *));
extern __hmap_s_mp *GetMountImpl(const char *path);

static int32_t dev_core_init(void)
{
    Dev::Init();
    InitFFMAN();
    mount_set_lookup(&GetMountImpl);
    g_dev_ready = true;
    kprintf_("[comp.dev.core] init: Dev registry + file/mount manager ready\n");
    return KMOD_OK;
}

static void dev_core_exit(void)
{
    g_dev_ready = false;
    kprintf_("[comp.dev.core] exit (Dev/MP tables owned by Tier0 subsystems)\n");
}

KCOMPONENT_EXPORT_CONTRACT(dev_core, KMOD_CONTRACT_DEV_V1, g_dev_ops,
                           KMOD_DEV_ABI_V1)

/* Dev 注册表实现已迁入本 .ko；驱动/FS/syscall 组件按 mangled name 解析。 */
KCOMPONENT_EXPORT_RAW(dev_core, add_storage,
    "_ZN3Dev16AddStorageDeviceE9VsDevType6DevOPSmPv",
    &Dev::AddStorageDevice, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, add_device,
    "_ZN3Dev9AddDeviceE7DevList9VsDevType6DevOPS",
    &Dev::AddDevice, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, get_name,
    "_ZN3Dev7GetSDEVEPKc",
    (VDL * (*)(const char *))Dev::GetSDEV, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, get_type,
    "_ZN3Dev7GetSDEVE9VsDevTypej",
    (VDL * (*)(VsDevType, uint32_t))Dev::GetSDEV, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, read,
    "_ZN3Dev4ReadEP7DevListmjPv", &Dev::Read, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, write,
    "_ZN3Dev5WriteEP7DevListmjPv", &Dev::Write, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, read_bytes,
    "_ZN3Dev9ReadBytesEP7DevListmjPv",
    &Dev::ReadBytes, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, write_bytes,
    "_ZN3Dev10WriteBytesEP7DevListmjPv",
    &Dev::WriteBytes, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, find_device,
    "_ZN3Dev10FindDeviceE9VsDevTypej",
    &Dev::FindDevice, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, device_read,
    "_ZN3Dev10DeviceReadE9VsDevTypejmPvm",
    &Dev::DeviceRead, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, device_write,
    "_ZN3Dev11DeviceWriteE9VsDevTypejmPvm",
    &Dev::DeviceWrite, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, device_mmap,
    "_ZN3Dev15DeviceMemoryMapE9VsDevTypejmmmm",
    &Dev::DeviceMemoryMap, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
/* 挂载点表实现已迁入本 .ko（fd.cpp 只保留 fd_manager）。 */
KCOMPONENT_EXPORT_RAW(dev_core, hmap_mp, "HMapS_MP",
    &HMapS_MP, 0u, 1u, KMOD_SYM_DATA, 0u, 0u);
KCOMPONENT_EXPORT_RAW(dev_core, mount_name, "GetMountPointName",
    (char *(*)(const char *))GetMountPointName, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);

KCOMPONENT_END(dev_core)
