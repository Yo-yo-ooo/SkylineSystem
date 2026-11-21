//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* fs_fdcore.cpp —— kernel.fs.fdcore：fd_manager 核心独立 .ko。
 * sched.core 依赖本组件，保证 idle/进程创建前 hook 已注册。 */
#include <kmod/kmod_component.h>
#include <fs/fd.h>

static int32_t fs_fdcore_init(void);

KCOMPONENT_BEGIN(fs_fdcore, "kernel.fs.fdcore", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_LOG), 0,
                 "fd_manager init/destroy/dup core",
                 fs_fdcore_init, nullptr, 0, 0, 2000u, 200u)

extern "C" void fd_set_hooks(void (*)(fd_manager_t *), void (*)(fd_manager_t *),
                             int32_t (*)(fd_manager_t *, fd_manager_t *));

static int32_t fs_fdcore_init(void)
{
    fd_set_hooks(&fd_manager_init, &fd_manager_destroy, &fd_manager_dup);
    return KMOD_OK;
}

KCOMPONENT_EXPORT_RAW(fs_fdcore, fd_init, "fd_manager_init",
    &fd_manager_init, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(fs_fdcore, fd_destroy, "fd_manager_destroy",
    &fd_manager_destroy, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_EXPORT_RAW(fs_fdcore, fd_dup, "fd_manager_dup",
    &fd_manager_dup, 0u, 1u, KMOD_SYM_FUNC, 0u, 0u);
KCOMPONENT_END(fs_fdcore)
