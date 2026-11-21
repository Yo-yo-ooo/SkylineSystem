//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* fd_hook.cpp —— fd_manager 的 Tier0 跳板。
 * fd.cpp 已迁入 kernel.fs.fdcore.ko；sched.core 显式依赖 fdcore，确保
 * Schedule::Init 创建 idle 进程前 hook 已注册。 */
#include <fs/fd.h>
#include <klib/kio.h>

namespace {
void (*g_init)(fd_manager_t *) = nullptr;
void (*g_destroy)(fd_manager_t *) = nullptr;
int32_t (*g_dup)(fd_manager_t *, fd_manager_t *) = nullptr;
}

extern "C" void fd_set_hooks(void (*init_fn)(fd_manager_t *),
                             void (*destroy_fn)(fd_manager_t *),
                             int32_t (*dup_fn)(fd_manager_t *, fd_manager_t *))
{
    __atomic_store_n(&g_init, init_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_destroy, destroy_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_dup, dup_fn, __ATOMIC_RELEASE);
}

void fd_manager_init(fd_manager_t *m) { auto fn = __atomic_load_n(&g_init, __ATOMIC_ACQUIRE); if (fn) fn(m); }
void fd_manager_destroy(fd_manager_t *m) { auto fn = __atomic_load_n(&g_destroy, __ATOMIC_ACQUIRE); if (fn) fn(m); }
int32_t fd_manager_dup(fd_manager_t *d, fd_manager_t *s) { auto fn = __atomic_load_n(&g_dup, __ATOMIC_ACQUIRE); return fn ? fn(d, s) : -1; }

extern "C" int32_t file_cache_writeback_callback(
    uint64_t file_id, const uint8_t *key, uint32_t key_len, void *data, size_t data_len)
{
    if (!key || key_len == 0 || !data || data_len == 0) return -1;
    if (file_id == 0) return -1;
    fd_t *orig = (fd_t *)file_id;
    if (!orig->FSOPS || !orig->filedesc) return -1;
    orig->FSOPS->lseek(orig->filedesc, 0, 0);
    size_t wcnt = 0;
    int32_t err = orig->FSOPS->write(orig->filedesc, data, data_len, &wcnt);
    if (err != 0 || wcnt != data_len) {
        kinfo("[FC_WRITEBACK] Incomplete writeback (wrote %zu/%zu)\n", wcnt, data_len);
        return -1;
    }
    return 0;
}
