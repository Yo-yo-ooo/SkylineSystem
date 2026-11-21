//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* mount_hook.cpp —— 挂载点查找的 Tier0 跳板。
 *
 * 挂载点表实现已迁入 kernel.dev.core.ko；kelf_read_file 在桌面/用户进程
 * 装载时调用 GetMount，此时 dev.core 已 READY。未注册时返回 nullptr。 */
#include <fs/fd.h>

static __hmap_s_mp *(*g_get_mount)(const char *) = nullptr;

extern "C" void mount_set_lookup(__hmap_s_mp *(*fn)(const char *))
{
    __atomic_store_n(&g_get_mount, fn, __ATOMIC_RELEASE);
}

__hmap_s_mp *GetMount(const char *path)
{
    __hmap_s_mp *(*fn)(const char *) = __atomic_load_n(&g_get_mount, __ATOMIC_ACQUIRE);
    return fn ? fn(path) : nullptr;
}
