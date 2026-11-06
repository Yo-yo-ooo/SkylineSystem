//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_core.cpp —— kmod 子系统引导与公共 API 门面（Tier 0）
 * ----------------------------------------------------------------------------
 * 负责：
 *   - 全局注册表/统计/锁的定义（governance/seat/syscall 共用）；
 *   - KmodInit：能力策略 -> 事务治理 -> 座位 -> syscall 契约表 的初始化顺序；
 *   - KmodBootIngest / KmodBootLoad：把 Limine module 里的 .ko 存入镜像池并
 *     按清单 KMOD_MF_BOOT 自动加载（依赖由事务层负责按拓扑序拉起）；
 *   - 观测：KmodDump / KmodStats / KmodModuleInfoAt / KmodModuleState；
 *   - epoch 公共包装（模块消费者进入/退出受治理调用区间）。
 * 真正的加载事务/热替换/卸载状态机在 governance.cpp，本文件只做门面与编排。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

/* main.cpp 提供的最小引导模块访问接口（避免把 limine.h 带进 kmod 层） */
extern "C" size_t GetBootModuleCount(void);
extern "C" bool GetBootModuleInfo(size_t index, const uint8_t **data,
                                  uint64_t *size, const char **path);

volatile int32_t g_kmod_lock = 0;
kmod_stats_t g_kmod_stats;
kmod_module_t g_kmod_modules[KMOD_MAX_MODULES];
uint32_t g_kmod_module_count = 0;

namespace {

bool str_endswith(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return false;
    uint64_t n = 0, m = 0;
    while (s[n]) n++;
    while (suffix[m]) m++;
    if (m > n)
        return false;
    for (uint64_t i = 0; i < m; i++) {
        if (s[n - m + i] != suffix[i])
            return false;
    }
    return true;
}

void stats_reset(void)
{
    uint8_t *p = (uint8_t *)&g_kmod_stats;
    for (uint64_t i = 0; i < sizeof(g_kmod_stats); i++)
        p[i] = 0;
}

} /* namespace */

extern "C" int32_t KmodInit(void)
{
    g_kmod_lock = 0;
    stats_reset();
    g_kmod_module_count = 0;
    g_kmod_image_count = 0;
    for (uint32_t i = 0; i < KMOD_MAX_MODULES; i++) {
        uint8_t *p = (uint8_t *)&g_kmod_modules[i];
        for (uint64_t k = 0; k < sizeof(g_kmod_modules[i]); k++)
            p[k] = 0;
    }

    KmodCapInit();
    int32_t rc = KmodGovInit();
    if (rc != KMOD_OK) {
        kmod_log("[kmod] init FAILED: governance rc=%d\n", (int)rc);
        return rc;
    }
    rc = KmodSeatInit();
    if (rc != KMOD_OK) {
        kmod_log("[kmod] init FAILED: seats rc=%d\n", (int)rc);
        return rc;
    }
    rc = KmodSyscallInit();
    if (rc != KMOD_OK) {
        kmod_log("[kmod] init FAILED: syscall registry rc=%d\n", (int)rc);
        return rc;
    }
    kmod_log("[kmod] core ready: abi=%u policy=%u tier0=fixed loader=ET_REL/x86_64\n",
             (unsigned)KMOD_ABI_VERSION, (unsigned)KmodCapMode());
    return KMOD_OK;
}

extern "C" int32_t KmodBootComponents(void)
{
    return KmodBuiltinBootLoad();
}

extern "C" uint32_t KmodComponentCount(void)
{
    uint32_t n = KmodBuiltinCount();
    uint32_t declared = 0;
    for (uint32_t i = 0; i < n; i++) {
        const kmod_builtin_raw_t *raw = KmodBuiltinAt(i);
        if (raw && raw->magic == KMOD_BUILTIN_MAGIC)
            declared++;
    }
    return declared;
}

extern "C" int32_t KmodBootIngest(void)
{
    size_t total = GetBootModuleCount();
    uint32_t ok = 0, bad = 0;
    for (size_t i = 0; i < total; i++) {
        const uint8_t *data = nullptr;
        uint64_t size = 0;
        const char *path = nullptr;
        if (!GetBootModuleInfo(i, &data, &size, &path))
            continue;
        if (!str_endswith(path, ".ko"))
            continue;
        int32_t rc = KmodPoolAdd(data, size, false);
        if (rc >= 0) {
            ok++;
        } else if (rc == KMOD_ERR_EXISTS) {
            /* 重复登记不是错误（同版本多份镜像按忽略处理） */
        } else {
            bad++;
            kmod_log("[kmod] ingest FAILED %s rc=%d (%s)\n", path ? path : "?",
                     (int)rc, KmodElfErrorString(rc));
        }
    }
    kmod_log("[kmod] boot ingest: %u image(s) accepted, %u rejected (pool=%u)\n",
             ok, bad, g_kmod_image_count);
    return KMOD_OK;
}

extern "C" int32_t KmodBootLoad(void)
{
    uint32_t attempted = 0, loaded = 0, failed = 0;
    /* 镜像池顺序 = Limine 配置顺序；模块间依赖由 KmodGovLoad 事务拉平，
       这里的循环顺序不影响正确性（已加载的 provider 会被复用）。 */
    for (uint32_t i = 0; i < g_kmod_image_count; i++) {
        kmod_image_t *img = &g_kmod_images[i];
        if (img->parse_ret != KMOD_OK)
            continue;
        if (!(img->flags & KMOD_MF_BOOT))
            continue;
        attempted++;
        if (KmodModuleFind(img->name) != nullptr) {
            loaded++;
            continue;
        }
        int32_t rc = KmodGovLoad(img->name, nullptr, KMOD_LOAD_NORMAL);
        if (rc == KMOD_OK) {
            loaded++;
        } else {
            failed++;
            kmod_log("[kmod] boot load FAILED %s@%s rc=%d (%s)\n",
                     img->name, img->version, (int)rc, KmodElfErrorString(rc));
        }
    }
    kmod_log("[kmod] boot load: %u requested, %u ready, %u failed\n",
             attempted, loaded, failed);
    return KMOD_OK;
}

extern "C" int32_t KmodLoad(const char *name, uint32_t flags)
{
    return KmodGovLoad(name, nullptr, flags);
}

extern "C" int32_t KmodLoadVersion(const char *name, const char *version, uint32_t flags)
{
    return KmodGovLoad(name, version, flags);
}

extern "C" int32_t KmodUnload(const char *name)
{
    return KmodGovUnload(name, 0);
}

extern "C" int32_t KmodHotReplace(const char *name, const char *new_version)
{
    return KmodGovHotReplace(name, new_version, 0);
}

extern "C" int32_t KmodEpochEnter(const char *name)
{
    return KmodEpochEnterInternal(name);
}

extern "C" void KmodEpochExit(const char *name)
{
    KmodEpochExitInternal(name);
}

extern "C" const kmod_stats_t *KmodStats(void)
{
    return &g_kmod_stats;
}

extern "C" uint32_t KmodModuleCount(void)
{
    return g_kmod_module_count;
}

extern "C" int32_t KmodModuleInfoAt(uint32_t index, kmod_module_info_t *out)
{
    if (!out)
        return KMOD_ERR_INVAL;
    uint64_t flags = kmod_lock_acquire(&g_kmod_lock);
    int32_t rc = KMOD_ERR_NOTFOUND;
    if (index < g_kmod_module_count &&
        g_kmod_modules[index].state != KMOD_STATE_EMPTY &&
        g_kmod_modules[index].state != KMOD_STATE_REMOVED) {
        kmod_module_t *m = &g_kmod_modules[index];
        for (uint64_t i = 0; i < sizeof(*out); i++)
            ((uint8_t *)out)[i] = 0;
        for (uint32_t i = 0; i + 1 < KMOD_NAME_MAX && m->name[i]; i++)
            out->name[i] = m->name[i];
        for (uint32_t i = 0; i + 1 < KMOD_VERSION_MAX && m->version[i]; i++)
            out->version[i] = m->version[i];
        out->tier = m->tier;
        out->state = m->state;
        out->flags = m->flags;
        out->caps_granted = m->caps_granted;
        out->caps_denied = m->caps_denied;
        out->refcount = m->refcount;
        out->epoch_active = __atomic_load_n(&m->epoch_active, __ATOMIC_RELAXED);
        out->n_imports = m->n_imports;
        out->n_exports = m->n_exports;
        out->n_deps = m->n_deps;
        out->n_consumers = m->n_consumers;
        out->image_size = m->img.size > 0xffffffffull ? 0xffffffffu
                                                      : (uint32_t)m->img.size;
        out->generation = m->generation;
        rc = KMOD_OK;
    }
    kmod_lock_release(&g_kmod_lock, flags);
    return rc;
}

extern "C" int32_t KmodModuleState(const char *name)
{
    if (!name)
        return KMOD_ERR_INVAL;
    uint64_t flags = kmod_lock_acquire(&g_kmod_lock);
    int32_t rc = KMOD_ERR_NOTFOUND;
    for (uint32_t i = 0; i < g_kmod_module_count; i++) {
        kmod_module_t *m = &g_kmod_modules[i];
        const char *a = m->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            rc = m->state;
            break;
        }
    }
    kmod_lock_release(&g_kmod_lock, flags);
    return rc;
}

extern "C" int32_t KmodDump(void)
{
    uint64_t flags = kmod_lock_acquire(&g_kmod_lock);
    kmod_log("[kmod] ---- module table (%u) ----\n", g_kmod_module_count);
    for (uint32_t i = 0; i < g_kmod_module_count; i++) {
        kmod_module_t *m = &g_kmod_modules[i];
        kmod_log("[kmod] #%u %s@%s state=%s tier=%u flags=0x%x caps=g:0x%llx d:0x%llx\n",
                 i, m->name, m->version, KmodStateName(m->state),
                 (unsigned)m->tier, (unsigned)m->flags,
                 (unsigned long long)m->caps_granted,
                 (unsigned long long)m->caps_denied);
        kmod_log("[kmod]    img=%lluB gen=%u seq=%llu refcount=%u epoch=%u "
                 "imports=%u exports=%u deps=%u consumers=%u\n",
                 (unsigned long long)m->img.size, m->generation,
                 (unsigned long long)m->load_seq, m->refcount,
                 __atomic_load_n(&m->epoch_active, __ATOMIC_RELAXED),
                 m->n_imports, m->n_exports, m->n_deps, m->n_consumers);
        for (uint32_t d = 0; d < m->n_deps; d++) {
            uint32_t pi = m->deps[d];
            if (pi < g_kmod_module_count)
                kmod_log("[kmod]    edge %s -> %s\n",
                         g_kmod_modules[pi].name, m->name);
        }
    }
    kmod_log("[kmod] ---- stats: load=%llu/%llu unload=%llu/%llu hotswap=%llu/%llu "
             "rollback=%llu cap=%llu cycle=%llu ver=%llu fmt=%llu reloc=%llu "
             "name=%llu policy=%llu busy=%llu seat=%llu sc=%llu/%llu epochwait=%llu "
             "budget=%llu st=%llu/%llu ----\n",
             (unsigned long long)g_kmod_stats.loads_ok,
             (unsigned long long)g_kmod_stats.loads_fail,
             (unsigned long long)g_kmod_stats.unloads_ok,
             (unsigned long long)g_kmod_stats.unloads_fail,
             (unsigned long long)g_kmod_stats.hotswaps_ok,
             (unsigned long long)g_kmod_stats.hotswaps_fail,
             (unsigned long long)g_kmod_stats.rollbacks,
             (unsigned long long)g_kmod_stats.reject_cap,
             (unsigned long long)g_kmod_stats.reject_cycle,
             (unsigned long long)g_kmod_stats.reject_version,
             (unsigned long long)g_kmod_stats.reject_format,
             (unsigned long long)g_kmod_stats.reject_reloc,
             (unsigned long long)g_kmod_stats.reject_name,
             (unsigned long long)g_kmod_stats.reject_policy,
             (unsigned long long)g_kmod_stats.reject_busy,
             (unsigned long long)g_kmod_stats.seat_swaps,
             (unsigned long long)g_kmod_stats.syscall_binds,
             (unsigned long long)g_kmod_stats.syscall_overrides,
             (unsigned long long)g_kmod_stats.epoch_waits,
             (unsigned long long)g_kmod_stats.budget_exceeded,
             (unsigned long long)g_kmod_stats.selftest_pass,
             (unsigned long long)g_kmod_stats.selftest_fail);
    kmod_lock_release(&g_kmod_lock, flags);
    return KMOD_OK;
}
