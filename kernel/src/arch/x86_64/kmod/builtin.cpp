//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * builtin.cpp —— 内建组件管理器（Tier 0 固定机制；整个内核组件化的入口）
 * ----------------------------------------------------------------------------
 * 内核镜像里用 KCOMPONENT_BEGIN/END 宏声明的组件，其 manifest/.in/.out 在
 * 链接期落入 .kbuiltin.* 段；本文件在启动时：
 *   1. 扫描 .kbuiltin.desc 收集所有内建组件描述符（严格校验边界哨兵/清单）；
 *   2. 把每个组件的导入 provider 抽成模块级依赖边；
 *   3. KmodTopoSort 得到 provider 先序（A->B 环 -> 拒绝并打印环路径）；
 *   4. 逐组件：KmodModuleAllocBuiltin -> KmodBindImports -> KmodApplyExports
 *      -> KmodCallInit -> READY；任一步失败逆序回滚已提交组件；
 *   5. 之后 .ko 引导模块继续走同一张图（KmodBootLoad），可导入内建组件导出，
 *      内建组件也可被 .ko 热替换（governance 的 BUILTIN 分支负责回滚恢复）。
 * 这样 x86_64_init 里不再是一条硬编码初始化长链，而是由组件拓扑图驱动。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

/* 链接脚本提供（kernel/linker-x86_64.ld 的 .data 输出段内） */
extern "C" const kmod_builtin_raw_t __kbuiltin_desc_start[];
extern "C" const kmod_builtin_raw_t __kbuiltin_desc_end[];

namespace {

struct RawView {
    const kmod_builtin_raw_t *raw;
    const kmod_import_t *imports;
    uint32_t n_imports;
    const kmod_export_t *exports;
    uint32_t n_exports;
    const char *name;
};

/* 校验并解析一段内建组件条目，返回条目字节跨度；失败返回 0 */
bool parse_entries(const uint8_t *begin, const uint8_t *end, uint64_t entry_size,
                   uint32_t max_entries, const uint8_t **entries_out,
                   uint32_t *count_out)
{
    if (!begin || !end || begin >= end)
        return false;
    uint64_t first;
    for (uint32_t i = 0; i < 8; i++)
        ((uint8_t *)&first)[i] = begin[i];
    if (first != KMOD_BUILTIN_BEGIN_MAGIC)
        return false;
    /* end 指向 END 哨兵（8 字节）；entries 位于 (begin+8, end) */
    uint64_t end_magic;
    for (uint32_t i = 0; i < 8; i++)
        ((uint8_t *)&end_magic)[i] = end[i];
    if (end_magic != KMOD_BUILTIN_END_MAGIC)
        return false;
    uint64_t bytes = (uint64_t)(end - begin) - 8u;
    if (bytes % entry_size != 0)
        return false;
    uint64_t count = bytes / entry_size;
    if (count > max_entries)
        return false;
    *entries_out = begin + 8;
    *count_out = (uint32_t)count;
    return true;
}

bool raw_view(const kmod_builtin_raw_t *raw, RawView *out)
{
    if (!raw || raw->magic != KMOD_BUILTIN_MAGIC || !raw->manifest)
        return false;
    if (!raw->in_begin || !raw->in_end || !raw->out_begin || !raw->out_end)
        return false;
    const kmod_manifest_t *m = raw->manifest;
    if (m->magic != KMOD_MANIFEST_MAGIC || m->abi_version != KMOD_ABI_VERSION)
        return false;
    if (KmodBuiltinNameValidate(m->name) != KMOD_OK)
        return false;
    out->raw = raw;
    out->name = m->name;
    out->imports = nullptr;
    out->n_imports = 0;
    out->exports = nullptr;
    out->n_exports = 0;
    const uint8_t *entries = nullptr;
    uint32_t count = 0;
    if (!parse_entries(raw->in_begin, raw->in_end, sizeof(kmod_import_t),
                       KMOD_MAX_IMPORTS, &entries, &count))
        return false;
    out->imports = (const kmod_import_t *)entries;
    out->n_imports = count;
    entries = nullptr;
    count = 0;
    if (!parse_entries(raw->out_begin, raw->out_end, sizeof(kmod_export_t),
                       KMOD_MAX_EXPORTS, &entries, &count))
        return false;
    out->exports = (const kmod_export_t *)entries;
    out->n_exports = count;
    return true;
}

bool name_eq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

uint32_t collect_raws(const kmod_builtin_raw_t **out, uint32_t cap)
{
    uint32_t n = 0;
    const kmod_builtin_raw_t *p = __kbuiltin_desc_start;
    while (p < __kbuiltin_desc_end) {
        if (n >= cap)
            break;
        out[n++] = p;
        p++;
    }
    return n;
}

int32_t find_raw_index(const char *name, const RawView *views, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (name_eq(views[i].name, name))
            return (int32_t)i;
    }
    return KMOD_ERR_NOTFOUND;
}

/* 提交一个已解析的内建组件：分配节点 -> bind -> apply exports -> init -> READY */
int32_t commit_one(const RawView *view, int32_t *module_index_out)
{
    int32_t mi = -1;
    int32_t rc = KmodModuleAllocBuiltin(view->raw, &mi);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] builtin %s: alloc failed rc=%d (%s)\n", view->name,
                 (int)rc, KmodElfErrorString(rc));
        return rc;
    }
    kmod_module_t *m = &g_kmod_modules[mi];
    /* 状态语义：KmodBindImports 只接受 LOADING/REPLACING；bind 成功后再进入
       RESOLVING（ApplyExports/CallInit 不依赖该状态，CallInit 成功置 READY）。 */

    rc = KmodBindImports(m);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] builtin %s: bind failed rc=%d (%s)\n", view->name,
                 (int)rc, KmodElfErrorString(rc));
        KmodUnbindAll(m);
        KmodModuleRelease(m);
        return rc;
    }
    m->state = KMOD_STATE_RESOLVING;
    rc = KmodApplyExports(m);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] builtin %s: export registration failed rc=%d (%s)\n",
                 view->name, (int)rc, KmodElfErrorString(rc));
        KmodRevertExports(m);
        KmodUnbindAll(m);
        KmodModuleRelease(m);
        return rc;
    }
    kmod_log("[kmod] commit %s: exports applied, calling init...\n", view->name);
    rc = KmodCallInit(m);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] builtin %s: init failed rc=%d (%s)\n", view->name,
                 (int)rc, KmodElfErrorString(rc));
        KmodRevertExports(m);
        KmodUnbindAll(m);
        KmodModuleRelease(m);
        return rc;
    }
    kmod_log("[kmod] commit %s: init ok\n", view->name);
    m->state = KMOD_STATE_READY;
    g_kmod_stats.loads_ok++;
    if (module_index_out)
        *module_index_out = mi;
    kmod_log("[kmod] builtin ready: %s@%s tier=%u caps=0x%llx "
             "imports=%u exports=%u epoch=%u\n",
             m->name, m->version, (unsigned)m->tier,
             (unsigned long long)m->caps_granted, m->n_imports, m->n_exports,
             __atomic_load_n(&m->epoch_active, __ATOMIC_RELAXED));
    return KMOD_OK;
}

/* 收集组件声明的内建依赖（provider != "kernel" 且在内建集合内） */
uint32_t gather_deps(const RawView &view, const RawView *views, uint32_t n,
                     const char **deps_out, uint32_t deps_cap)
{
    uint32_t n_deps = 0;
    for (uint32_t i = 0; i < view.n_imports; i++) {
        const kmod_import_t *imp = &view.imports[i];
        const char *provider = imp->provider;
        if (!provider || !provider[0])
            continue;
        if (name_eq(provider, "kernel"))
            continue;
        bool dup = false;
        for (uint32_t d = 0; d < n_deps; d++) {
            if (name_eq(deps_out[d], provider)) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (n_deps >= deps_cap)
            continue;
        deps_out[n_deps++] = provider;
    }
    (void)views;
    (void)n;
    return n_deps;
}

} /* namespace */

/* ==================================================================== */
/* 公共入口                                                              */
/* ==================================================================== */

/* 内建组件的加载序号（governance 的 g_load_seq 在匿名 namespace 内不可外部
 * 引用；内建组件在启动期单线程提交，这里用独立单调计数器即可用于审计排序）。
 * TODO(接口): 与 governance 的 .ko 序号合并成全局全序，需要内部 API 导出。 */
static uint64_t g_builtin_seq = 0;

extern "C" int32_t KmodModuleAllocBuiltin(const kmod_builtin_raw_t *raw,
                                          int32_t *index_out)
{
    if (!raw || !index_out)
        return KMOD_ERR_INVAL;
    *index_out = -1;
    RawView view;
    if (!raw_view(raw, &view))
        return KMOD_ERR_FORMAT;
    const kmod_manifest_t *man = raw->manifest;
    char err[80];
    err[0] = '\0';
    int32_t rc = KmodElfValidateBuiltinManifest(man, err, sizeof(err));
    if (rc != KMOD_OK) {
        kmod_log("[kmod] builtin %s: manifest invalid rc=%d (%s)\n", man->name,
                 (int)rc, err);
        return rc;
    }
    /* Tier0 保留能力位绝不允许出现在内建组件（更不用说 .ko）清单里 */
    if (man->required_caps & KMOD_CAP_RESERVED_CORE)
        return KMOD_ERR_CAP;
    if (KmodModuleFind(man->name) != nullptr)
        return KMOD_ERR_EXISTS;

    uint64_t lf = kmod_lock_acquire(&g_kmod_lock);
    int32_t idx = -1;
    for (uint32_t i = 0; i < KMOD_MAX_MODULES; i++) {
        if (g_kmod_modules[i].state == KMOD_STATE_EMPTY) {
            idx = (int32_t)i;
            break;
        }
    }
    if (idx < 0) {
        kmod_lock_release(&g_kmod_lock, lf);
        return KMOD_ERR_NOMEM;
    }
    kmod_module_t *m = &g_kmod_modules[idx];
    uint8_t *mp = (uint8_t *)m;
    for (uint64_t i = 0; i < sizeof(*m); i++)
        mp[i] = 0;
    for (uint32_t i = 0; i + 1 < KMOD_NAME_MAX && man->name[i]; i++)
        m->name[i] = man->name[i];
    for (uint32_t i = 0; i + 1 < KMOD_VERSION_MAX && man->version[i]; i++)
        m->version[i] = man->version[i];
    m->tier = man->tier;
    m->flags = man->flags;
    m->internal_flags = KMOD_MODF_BUILTIN;
    m->caps_required = man->required_caps;
    m->caps_optional = man->optional_caps;
    /* 内建组件是 Tier0 构建产物：能力按清单授予（不做 STRICT 白名单校验），
       保留位/未定义位在下面的掩码里剔除并拒绝未知位。 */
    if (man->required_caps & ~KMOD_CAP_ALL_KNOWN) {
        g_kmod_modules[idx].internal_flags = 0;
        kmod_lock_release(&g_kmod_lock, lf);
        return KMOD_ERR_CAP;
    }
    m->caps_granted = (man->required_caps | man->optional_caps) & KMOD_CAP_ALL_KNOWN;
    m->caps_denied = 0;
    m->pool_index = -1;
    m->generation = 0;
    m->load_seq = ++g_builtin_seq;
    m->img.mem = nullptr;
    m->img.size = 0;
    m->img.manifest = (kmod_manifest_t *)man;
    m->img.exports = (kmod_export_t *)view.exports;
    m->img.n_exports = view.n_exports;
    m->img.imports = (kmod_import_t *)view.imports;
    m->img.n_imports = view.n_imports;
    m->img.init_array = nullptr;
    m->img.n_init_array = 0;
    m->img.fini_array = nullptr;
    m->img.n_fini_array = 0;
    m->exports = (kmod_export_t *)view.exports;
    m->n_exports = view.n_exports;
    m->imports = (kmod_import_t *)view.imports;
    m->n_imports = view.n_imports;
    m->state = KMOD_STATE_LOADING;
    m->gen_exports_hash = KmodExportsHash(m->exports, m->n_exports);
    if (g_kmod_module_count < (uint32_t)(idx + 1))
        g_kmod_module_count = (uint32_t)(idx + 1);
    kmod_lock_release(&g_kmod_lock, lf);

    kmod_log("[kmod] alloc builtin %s@%s -> slot #%d (seq=%llu tier=%u "
             "caps=0x%llx imports=%u exports=%u)\n",
             m->name, m->version, (int)idx, (unsigned long long)m->load_seq,
             (unsigned)m->tier, (unsigned long long)m->caps_granted,
             view.n_imports, view.n_exports);
    *index_out = idx;
    return KMOD_OK;
}

extern "C" int32_t KmodBuiltinInit(void)
{
    uint32_t n = KmodBuiltinCount();
    /* 全量预校验：坏描述符在启动早期就暴露，而不是在 init 半途 */
    const kmod_builtin_raw_t *raws[KMOD_MAX_BUILTINS];
    uint32_t got = collect_raws(raws, KMOD_MAX_BUILTINS);
    for (uint32_t i = 0; i < got; i++) {
        RawView v;
        if (!raw_view(raws[i], &v)) {
            kmod_log("[kmod] builtin: bad descriptor #%u "
                     "(magic/manifest/section bounds)\n", i);
            return KMOD_ERR_FORMAT;
        }
    }
    kmod_log("[kmod] builtin components: %u declared\n", n);
    return KMOD_OK;
}

extern "C" uint32_t KmodBuiltinCount(void)
{
    uint64_t bytes = (uint64_t)((const uint8_t *)__kbuiltin_desc_end -
                                (const uint8_t *)__kbuiltin_desc_start);
    uint64_t n = bytes / sizeof(kmod_builtin_raw_t);
    return n > KMOD_MAX_BUILTINS ? KMOD_MAX_BUILTINS : (uint32_t)n;
}

extern "C" const kmod_builtin_raw_t *KmodBuiltinAt(uint32_t index)
{
    if (index >= KmodBuiltinCount())
        return nullptr;
    return &__kbuiltin_desc_start[index];
}

extern "C" const kmod_builtin_raw_t *KmodBuiltinFind(const char *name)
{
    if (!name)
        return nullptr;
    uint32_t n = KmodBuiltinCount();
    for (uint32_t i = 0; i < n; i++) {
        const kmod_builtin_raw_t *raw = &__kbuiltin_desc_start[i];
        if (raw->magic != KMOD_BUILTIN_MAGIC || !raw->manifest)
            continue;
        if (name_eq(raw->manifest->name, name))
            return raw;
    }
    return nullptr;
}

extern "C" uint32_t KmodBuiltinImportCount(const kmod_builtin_raw_t *raw)
{
    RawView v;
    return raw_view(raw, &v) ? v.n_imports : 0;
}

extern "C" uint32_t KmodBuiltinExportCount(const kmod_builtin_raw_t *raw)
{
    RawView v;
    return raw_view(raw, &v) ? v.n_exports : 0;
}

extern "C" const kmod_import_t *KmodBuiltinImports(const kmod_builtin_raw_t *raw)
{
    RawView v;
    return raw_view(raw, &v) ? v.imports : nullptr;
}

extern "C" const kmod_export_t *KmodBuiltinExports(const kmod_builtin_raw_t *raw)
{
    RawView v;
    return raw_view(raw, &v) ? v.exports : nullptr;
}

/* 拓扑化加载：boot 一次到位；KmodBuiltinLoad 复用同一规划逻辑 */
/* 规划工作区放 BSS 而不是内核栈：KMOD_MAX_BUILTINS × KMOD_MAX_IMPORTS 的
 * provider 名表约 32 KiB，叠加 RawView/nodes 等工作区会超过引导栈（Limine
 * 启动栈较小）。组件管理器只在启动期单线程执行（KmodBootComponents /
 * KmodBuiltinLoad），静态工作区可安全复用；并发加载路径由治理锁串行化。 */
static int32_t builtin_load_set(const char *only_name)
{
    static RawView views[KMOD_MAX_BUILTINS];
    uint32_t n = 0;
    uint32_t raw_n = KmodBuiltinCount();
    for (uint32_t i = 0; i < raw_n && n < KMOD_MAX_BUILTINS; i++) {
        RawView v;
        if (!raw_view(KmodBuiltinAt(i), &v)) {
            kmod_log("[kmod] builtin: descriptor #%u invalid\n", i);
            return KMOD_ERR_FORMAT;
        }
        views[n++] = v;
    }
    if (n == 0) {
        kmod_log("[kmod] builtin: none declared (Tier0-only boot)\n");
        return KMOD_OK;
    }

    /* only_name != NULL：只加载该组件及其内建依赖闭包 */
    static bool want[KMOD_MAX_BUILTINS];
    for (uint32_t i = 0; i < n; i++)
        want[i] = (only_name == nullptr);
    if (only_name) {
        int32_t target = find_raw_index(only_name, views, n);
        if (target < 0) {
            kmod_log("[kmod] builtin: %s not declared\n", only_name);
            return KMOD_ERR_NOTFOUND;
        }
        /* 简单 BFS 闭包（组件数量 <= 64，O(n^2) 足够） */
        want[target] = true;
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t i = 0; i < n; i++) {
                if (!want[i])
                    continue;
                const char *deps[KMOD_MAX_IMPORTS];
                uint32_t nd = gather_deps(views[i], views, n, deps, KMOD_MAX_IMPORTS);
                for (uint32_t d = 0; d < nd; d++) {
                    int32_t pi = find_raw_index(deps[d], views, n);
                    if (pi < 0) {
                        kmod_log("[kmod] builtin: %s depends on unknown builtin %s\n",
                                 views[i].name, deps[d]);
                        return KMOD_ERR_UNRESOLVED;
                    }
                    if (!want[pi]) {
                        want[pi] = true;
                        changed = true;
                    }
                }
            }
        }
    }

    /* 拓扑排序：把 want 集合内的 provider 依赖表达成节点邻接 */
    static kmod_topo_node_t nodes[KMOD_MAX_BUILTINS];
    static const char *dep_store[KMOD_MAX_BUILTINS][KMOD_MAX_IMPORTS];
    static uint32_t sel[KMOD_MAX_BUILTINS];
    static const char *names[KMOD_MAX_BUILTINS];
    uint32_t ns = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!want[i])
            continue;
        sel[ns] = i;
        names[ns] = views[i].name;
        ns++;
    }
    for (uint32_t s = 0; s < ns; s++) {
        uint32_t src = sel[s];
        const char *deps[KMOD_MAX_IMPORTS];
        uint32_t nd = gather_deps(views[src], views, n, deps, KMOD_MAX_IMPORTS);
        uint32_t kept = 0;
        for (uint32_t d = 0; d < nd; d++) {
            int32_t pi = find_raw_index(deps[d], views, n);
            if (pi >= 0 && want[pi]) {
                dep_store[s][kept++] = views[pi].name;
            } else if (pi < 0) {
                /* 外部 provider：必须已经加载（例如更早的 .ko/内建） */
                if (KmodModuleFind(deps[d]) == nullptr) {
                    kmod_log("[kmod] builtin: %s requires external module %s "
                             "(not loaded yet)\n", views[src].name, deps[d]);
                    return KMOD_ERR_UNRESOLVED;
                }
            }
        }
        nodes[s].name = names[s];
        nodes[s].deps = dep_store[s];
        nodes[s].n_deps = kept;
    }

    static uint32_t order[KMOD_MAX_BUILTINS];
    char cycle[192];
    int32_t rc = KmodTopoSort(nodes, ns, order, KMOD_MAX_BUILTINS, cycle, sizeof(cycle));
    if (rc < 0) {
        kmod_log("[kmod] builtin: dependency cycle: %s\n",
                 cycle[0] ? cycle : "(unknown)");
        g_kmod_stats.reject_cycle++;
        return rc;
    }
    uint32_t ordered = (uint32_t)rc;

    /* 逐组件提交；失败逆序回滚本次事务已提交的组件 */
    uint32_t committed = 0;
    static int32_t committed_mi[KMOD_MAX_BUILTINS];
    for (uint32_t k = 0; k < ordered; k++) {
        uint32_t s = order[k];
        uint32_t src = sel[s];
        const char *name = views[src].name;
        kmod_module_t *already = KmodModuleFind(name);
        if (already != nullptr) {
            kmod_log("[kmod] builtin: %s already loaded (state=%s)\n", name,
                     KmodStateName(already->state));
            continue;
        }
        int32_t mi = -1;
        rc = commit_one(&views[src], &mi);
        if (rc != KMOD_OK) {
            g_kmod_stats.loads_fail++;
            kmod_log("[kmod] builtin: commit FAILED at %s rc=%d -> rollback %u\n",
                     name, (int)rc, committed);
            for (uint32_t r = committed; r > 0; r--) {
                kmod_module_t *m = &g_kmod_modules[committed_mi[r - 1]];
                if (m->state != KMOD_STATE_EMPTY) {
                    m->state = KMOD_STATE_UNLOADING;
                    KmodCallExit(m);
                    KmodRevertExports(m);
                    KmodUnbindAll(m);
                    m->state = KMOD_STATE_FAILED;
                    KmodModuleRelease(m);
                }
            }
            g_kmod_stats.rollbacks++;
            return rc;
        }
        committed_mi[committed++] = mi;
    }
    kmod_log("[kmod] builtin graph: %u component(s) ready (topo order)\n",
             committed);
    return KMOD_OK;
}

extern "C" int32_t KmodBuiltinBootLoad(void)
{
    int32_t rc = KmodBuiltinInit();
    if (rc != KMOD_OK)
        return rc;
    return builtin_load_set(nullptr);
}

extern "C" int32_t KmodBuiltinLoad(const char *name)
{
    if (!name || !name[0])
        return KMOD_ERR_INVAL;
    if (KmodModuleFind(name) != nullptr)
        return KMOD_ERR_EXISTS;
    int32_t rc = KmodBuiltinInit();
    if (rc != KMOD_OK)
        return rc;
    return builtin_load_set(name);
}
