//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kdll.cpp —— 统一内核动态链接层（DynModule / Kdll）
 * ----------------------------------------------------------------------------
 * Tier 1（内建组件）与 Tier 2（外部 .ko）在这里合并成同一个模块模型。一个
 * DynModule 的"镜像后端"只有两种：
 *
 *   resident    代码已在内核镜像内（原内建组件；无重定位）；
 *   relocatable 外部 ET_REL .ko（映射/重定位由 governance 完成）。
 *
 * 两者都使用 kmod_manifest_t + .in/.out；都进入同一张依赖图；都通过
 * KmodBindImports/KmodApplyExports/KmodCallInit 提交；都由 KdllInject 注入。
 *
 * DLL 式 API：
 *   KdllInject(name, PIN)     LoadLibrary：按名注入，依赖自动拉起；
 *   KdllResolve(provider, s)   GetProcAddress；
 *   KdllEject(name)            FreeLibrary：释放 PIN，引用归零且可卸载才卸载；
 *   KdllInjectImage/File       从内存/文件系统注入（真正的运行期按路径加载）。
 *
 * 常驻核心服务（pmm/vmm/heap/syscall 入口）登记为 resident DynModule，于是
 * dosvm 可以显式注入 kernel.mem.pmm / kernel.arch.acpi，与注入 .ko 同路径。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#ifndef __KERNEL_TEST_HOST__
#include <elf/elf_image.h>
#include <mem/heap.h>
#endif

#include <stdint.h>
#include <stddef.h>

namespace {

constexpr uint32_t KDLL_MAX_RESIDENT = 8u;

struct kdll_stats {
    uint64_t injects_ok;
    uint64_t injects_fail;
    uint64_t ejects_ok;
    uint64_t ejects_fail;
    uint64_t images;
    uint64_t files;
    uint64_t resolve_hits;
    uint64_t resolve_miss;
};

kdll_stats g_kdll_stats;
kmod_manifest_t g_res_manifest[KDLL_MAX_RESIDENT];
uint32_t g_res_count = 0u;
bool g_kdll_ready = false;

inline bool str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

inline void str_copy(char *dst, uint32_t cap, const char *src)
{
    if (!dst || cap == 0u)
        return;
    uint32_t i = 0;
    if (src) {
        for (; i + 1u < cap && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 在 g_kmod_lock 保护下给模块加/减一次注入 PIN。 */
int32_t pin_locked(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    if (m->state != KMOD_STATE_READY)
        return (m->state == KMOD_STATE_LOADING || m->state == KMOD_STATE_REPLACING)
                   ? KMOD_ERR_BUSY
                   : KMOD_ERR_STATE;
    m->pin_count++;
    m->refcount++;
    m->internal_flags |= KMOD_MODF_INJECTED;
    return KMOD_OK;
}

/* 调用方持 g_kmod_lock。避免 KmodModuleFind 的"查完解锁再重新加锁"窗口
 * 里模块被卸载/槽位复用（resolve/eject 的热替换竞态）。 */
kmod_module_t *find_locked(const char *name)
{
    if (!name)
        return nullptr;
    for (uint32_t i = 0; i < g_kmod_module_count; i++) {
        kmod_module_t *m = &g_kmod_modules[i];
        uint16_t st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
        if (st == KMOD_STATE_EMPTY || st == KMOD_STATE_REMOVED)
            continue;
        if (str_eq(m->name, name))
            return m;
    }
    return nullptr;
}

int32_t unpin_locked(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    if (m->pin_count == 0u)
        return KMOD_ERR_STATE;
    m->pin_count--;
    if (m->refcount > 0u)
        m->refcount--;
    return KMOD_OK;
}

int32_t inject_finish(kmod_module_t *m, uint32_t flags, const char *how)
{
    uint64_t lf = kmod_lock_acquire(&g_kmod_lock);
    int32_t rc = KMOD_OK;
    if (flags & KMOD_INJ_PIN)
        rc = pin_locked(m);
    uint32_t pins = m->pin_count;
    uint32_t refs = m->refcount;
    uint16_t state = m->state;
    kmod_lock_release(&g_kmod_lock, lf);
    if (rc != KMOD_OK) {
        g_kdll_stats.injects_fail++;
        kmod_log("[kdll] inject %s: pin failed rc=%d (state=%u)\n", m->name,
                 (int)rc, (unsigned)state);
        return rc;
    }
    g_kdll_stats.injects_ok++;
    if (!(flags & KMOD_INJ_SILENT)) {
        kmod_log("[kdll] inject %s@%s via %s: READY pins=%u refs=%u\n",
                 m->name, m->version, how, (unsigned)pins, (unsigned)refs);
    }
    return KMOD_OK;
}

int32_t inject_blob(const uint8_t *data, uint64_t size, bool owned,
                    uint32_t flags)
{
    if (!data || size == 0u)
        return KMOD_ERR_INVAL;

    kmod_manifest_t man;
    char err[96];
    err[0] = '\0';
    int32_t rc = KmodElfInspect(data, size, &man, err, sizeof(err));
    if (rc != KMOD_OK) {
        if (owned)
            kmod_free((void *)data, size);
        g_kdll_stats.injects_fail++;
        kmod_log("[kdll] inject blob rejected (%s): rc=%d\n", err, (int)rc);
        return rc;
    }

    int32_t pool = KmodPoolAdd(data, size, owned);
    if (pool < 0 && pool != KMOD_ERR_EXISTS) {
        if (owned)
            kmod_free((void *)data, size);
        g_kdll_stats.injects_fail++;
        return pool;
    }
    if (pool == KMOD_ERR_EXISTS && owned)
        kmod_free((void *)data, size); /* 池里已有同 (name,version) 镜像 */
    if (pool >= 0)
        g_kdll_stats.images++;

    const int32_t irc = KdllInject(man.name, flags);
    if (irc != KMOD_OK && pool >= 0) {
        /* 运行期注入失败：池条目和它的 blob 一起回收，否则每失败一次就
           永久占一个池槽（最多 8 MiB），后续同名版本也无法再注入。 */
        KmodPoolRelease(pool);
    }
    return irc;
}

} /* namespace */

/* ==================================================================== */
/* 初始化 / 常驻服务登记                                                 */
/* ==================================================================== */

extern "C" int32_t KdllRegisterResident(const char *name, const char *version,
                                        uint64_t caps,
                                        const char *description)
{
    if (!name || !name[0] || !version || !version[0])
        return KMOD_ERR_INVAL;
    if (KmodBuiltinNameValidate(name) != KMOD_OK)
        return KMOD_ERR_NAME;
    if (caps & ~KMOD_CAP_ALL_KNOWN)
        return KMOD_ERR_CAP;

    /* 已登记过（KmodInit 可能重复执行）则幂等返回。 */
    for (uint32_t i = 0; i < g_res_count; i++) {
        if (str_eq(g_res_manifest[i].name, name))
            return KMOD_OK;
    }
    if (g_res_count >= KDLL_MAX_RESIDENT)
        return KMOD_ERR_NOMEM;

    kmod_manifest_t *man = &g_res_manifest[g_res_count];
    uint8_t *p = (uint8_t *)man;
    for (uint64_t i = 0; i < sizeof(*man); i++)
        p[i] = 0;
    man->magic = KMOD_MANIFEST_MAGIC;
    man->abi_version = KMOD_ABI_VERSION;
    man->tier = KMOD_TIER_DYN;
    man->flags = KMOD_MF_RESIDENT | KMOD_MF_BOOT;
    str_copy(man->name, KMOD_NAME_MAX, name);
    str_copy(man->version, KMOD_VERSION_MAX, version);
    str_copy(man->description, KMOD_DESC_MAX, description ? description : "");
    man->required_caps = caps;
    man->init = nullptr;
    man->exit_hook = nullptr;

    int32_t idx = -1;
    int32_t rc = KmodModuleAllocResident(man, KMOD_MODF_CORE, &idx);
    if (rc != KMOD_OK && rc != KMOD_ERR_EXISTS) {
        kmod_log("[kdll] resident %s: alloc failed rc=%d\n", name, (int)rc);
        return rc;
    }
    g_res_count++;
    return KMOD_OK;
}

extern "C" int32_t KdllInit(void)
{
    g_kdll_stats = kdll_stats{};
    g_res_count = 0u;
    g_kdll_ready = true;

    /* Tier0 核心服务以 resident DynModule 形式出现在统一模块表里。它们没有
     * 可重定位代码；注入/解析/审计与普通模块一致。 */
    (void)KdllRegisterResident(KDLL_CORE_PMM, "1.0.0", 0u,
                               "Tier0 physical memory manager");
    (void)KdllRegisterResident(KDLL_CORE_VMM, "1.0.0", 0u,
                               "Tier0 virtual memory / page tables");
    (void)KdllRegisterResident(KDLL_CORE_HEAP, "1.0.0", 0u,
                               "Tier0 SLAB/SLUB heap");
    (void)KdllRegisterResident(KDLL_CORE_SYSCALL, "1.0.0", 0u,
                               "Tier0 syscall entry/dispatch");

    kmod_log("[kdll] dynlink ready: one module class, %u resident core "
             "service(s), contract=%s\n",
             (unsigned)g_res_count, KMOD_CONTRACT_KERNEL_DYNLINK);
    return KMOD_OK;
}

/* ==================================================================== */
/* DLL 式注入                                                           */
/* ==================================================================== */

extern "C" int32_t KdllInject(const char *name, uint32_t flags)
{
    if (!name || !name[0])
        return KMOD_ERR_INVAL;
    if (!g_kdll_ready)
        return KMOD_ERR_STATE;

    if ((flags & KMOD_INJ_FILE) || name[0] == '/')
        return KdllInjectFile(name, flags);

    /* 1) 已经加载：幂等，按需加 PIN。 */
    kmod_module_t *m = KmodModuleFind(name);
    if (m) {
        uint16_t st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
        if (st != KMOD_STATE_READY) {
            g_kdll_stats.injects_fail++;
            return KMOD_ERR_BUSY;
        }
        return inject_finish(m, flags, "already-ready");
    }

    /* 2) resident/builtin 描述符（代码已在镜像内，无重定位）。 */
    if (KmodBuiltinFind(name) != nullptr) {
        int32_t rc = KmodBuiltinLoad(name);
        if (rc != KMOD_OK && rc != KMOD_ERR_EXISTS) {
            g_kdll_stats.injects_fail++;
            kmod_log("[kdll] inject %s: builtin load rc=%d (%s)\n", name,
                     (int)rc, KmodElfErrorString(rc));
            return rc;
        }
        m = KmodModuleFind(name);
        if (!m) {
            g_kdll_stats.injects_fail++;
            return KMOD_ERR_NOTFOUND;
        }
        return inject_finish(m, flags, "resident-image");
    }

    /* 3) 镜像池里的 ET_REL（原 Tier 2 .ko），依赖由治理事务自动拉起。 */
    if (KmodPoolFind(name, nullptr) >= 0) {
        int32_t rc = KmodGovLoad(name, nullptr, KMOD_LOAD_NORMAL);
        if (rc != KMOD_OK && rc != KMOD_ERR_EXISTS) {
            g_kdll_stats.injects_fail++;
            kmod_log("[kdll] inject %s: load transaction rc=%d (%s)\n", name,
                     (int)rc, KmodElfErrorString(rc));
            return rc;
        }
        m = KmodModuleFind(name);
        if (!m) {
            g_kdll_stats.injects_fail++;
            return KMOD_ERR_NOTFOUND;
        }
        return inject_finish(m, flags, "relocatable-image");
    }

    g_kdll_stats.injects_fail++;
    if (!(flags & KMOD_INJ_SILENT))
        kmod_log("[kdll] inject %s: not resident, builtin, pooled or a file\n",
                 name);
    return KMOD_ERR_NOTFOUND;
}

extern "C" int32_t KdllInjectImage(const void *data, uint64_t size,
                                   uint32_t flags)
{
    if (!data || size == 0u || size > KMOD_MAX_ELF_SIZE)
        return KMOD_ERR_INVAL;
    uint8_t *copy = (uint8_t *)kmod_alloc(size);
    if (!copy)
        return KMOD_ERR_NOMEM;
    const uint8_t *src = (const uint8_t *)data;
    for (uint64_t i = 0; i < size; i++)
        copy[i] = src[i];
    return inject_blob(copy, size, true, flags);
}

extern "C" int32_t KdllInjectFile(const char *path, uint32_t flags)
{
    if (!path || !path[0])
        return KMOD_ERR_INVAL;
#ifdef __KERNEL_TEST_HOST__
    (void)flags;
    return KMOD_ERR_POLICY; /* 宿主测试没有内核 VFS */
#else
    uint8_t *buf = nullptr;
    uint64_t len = 0;
    int rc = kelf_read_file(path, &buf, &len);
    if (rc != 0 || !buf) {
        g_kdll_stats.injects_fail++;
        kmod_log("[kdll] inject file '%s': read rc=%d\n", path, (int)rc);
        return rc < 0 ? rc : KMOD_ERR_IMAGE;
    }
    g_kdll_stats.files++;
    if (!(flags & KMOD_INJ_SILENT))
        kmod_log("[kdll] inject file '%s': %llu bytes\n", path,
                 (unsigned long long)len);
    return inject_blob(buf, len, true, flags);
#endif
}

extern "C" int32_t KdllEject(const char *name)
{
    if (!name || !name[0])
        return KMOD_ERR_INVAL;
    bool want_unload = false;
    uint32_t pins = 0;
    uint64_t lf = kmod_lock_acquire(&g_kmod_lock);
    kmod_module_t *m = find_locked(name);
    if (!m) {
        kmod_lock_release(&g_kmod_lock, lf);
        g_kdll_stats.ejects_fail++;
        return KMOD_ERR_NOTFOUND;
    }
    int32_t rc = unpin_locked(m);
    if (rc == KMOD_OK) {
        pins = m->pin_count;
        bool resident = (m->internal_flags &
                         (KMOD_MODF_RESIDENT | KMOD_MODF_BUILTIN)) != 0u;
        want_unload = !resident && pins == 0u && m->refcount == 0u &&
                      m->n_consumers == 0u &&
                      (m->flags & KMOD_MF_UNLOADABLE) != 0u;
    }
    kmod_lock_release(&g_kmod_lock, lf);

    if (rc != KMOD_OK) {
        g_kdll_stats.ejects_fail++;
        kmod_log("[kdll] eject %s: no injection PIN held\n", name);
        return rc;
    }

    if (want_unload) {
        int32_t urc = KmodGovUnload(name, 0u);
        if (urc != KMOD_OK) {
            /* 卸载失败：把 PIN 还回去，FreeLibrary 语义不应意外丢引用。 */
            lf = kmod_lock_acquire(&g_kmod_lock);
            m = find_locked(name);
            if (m)
                (void)pin_locked(m);
            kmod_lock_release(&g_kmod_lock, lf);
            g_kdll_stats.ejects_fail++;
            kmod_log("[kdll] eject %s: unload rc=%d (%s), pin restored\n", name,
                     (int)urc, KmodElfErrorString(urc));
            return urc;
        }
    }
    g_kdll_stats.ejects_ok++;
    if (pins == 0u) {
        kmod_log("[kdll] eject %s: pin released%s\n", name,
                 want_unload ? ", module unloaded" : "");
    }
    return KMOD_OK;
}

extern "C" void *KdllResolve(const char *provider, const char *symbol)
{
    if (!provider || !symbol || !symbol[0])
        return nullptr;
    /* 热替换会在锁内原子替换 exports/n_exports；扫描必须与切换互斥，
     * 否则可能读到"旧指针 + 新计数"的组合。 */
    uint64_t lf = kmod_lock_acquire(&g_kmod_lock);
    kmod_module_t *m = find_locked(provider);
    if (!m) {
        kmod_lock_release(&g_kmod_lock, lf);
        g_kdll_stats.resolve_miss++;
        return nullptr;
    }
    uint16_t st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (st != KMOD_STATE_READY) {
        kmod_lock_release(&g_kmod_lock, lf);
        g_kdll_stats.resolve_miss++;
        return nullptr;
    }
    for (uint32_t i = 0; i < m->n_exports; i++) {
        const kmod_export_t *e = &m->exports[i];
        if (e->symbol && str_eq(e->symbol, symbol)) {
            void *addr = e->addr;
            kmod_lock_release(&g_kmod_lock, lf);
            g_kdll_stats.resolve_hits++;
            return addr;
        }
    }
    kmod_lock_release(&g_kmod_lock, lf);
    g_kdll_stats.resolve_miss++;
    return nullptr;
}

extern "C" uint32_t KdllPinCount(const char *name)
{
    uint64_t lf = kmod_lock_acquire(&g_kmod_lock);
    kmod_module_t *m = find_locked(name);
    uint32_t n = m ? m->pin_count : 0u;
    kmod_lock_release(&g_kmod_lock, lf);
    return n;
}

extern "C" uint32_t KdllResidentCount(void)
{
    return g_res_count;
}

extern "C" int32_t KdllDump(void)
{
    kmod_log("[kdll] ---- dynlink stats ----\n");
    kmod_log("[kdll] inject ok/fail=%llu/%llu eject ok/fail=%llu/%llu "
             "images=%llu files=%llu resolve hit/miss=%llu/%llu residents=%u\n",
             (unsigned long long)g_kdll_stats.injects_ok,
             (unsigned long long)g_kdll_stats.injects_fail,
             (unsigned long long)g_kdll_stats.ejects_ok,
             (unsigned long long)g_kdll_stats.ejects_fail,
             (unsigned long long)g_kdll_stats.images,
             (unsigned long long)g_kdll_stats.files,
             (unsigned long long)g_kdll_stats.resolve_hits,
             (unsigned long long)g_kdll_stats.resolve_miss,
             (unsigned)g_res_count);
    return KMOD_OK;
}
