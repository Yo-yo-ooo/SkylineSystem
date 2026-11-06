//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * governance.cpp —— kmod 事务治理层（Tier0 固定机制的核心状态机）
 * ----------------------------------------------------------------------------
 * 职责（与 kmod_internal.h / docs 对应）：
 *   1. 模块运行时注册表（g_kmod_modules 的槽位分配/释放/查找；数组由 core 定义）；
 *   2. 加载事务：镜像池规划依赖闭包 -> prepare 全部镜像（重定位/清单/能力/
 *      策略校验，全部成功才进入 commit）-> 拓扑序 commit（alloc -> bind imports
 *      -> apply exports -> init）-> 任一步失败按逆序完整回滚（exit 已 init 者、
 *      revert exports、unbind、release），不留半加载残留；
 *   3. 卸载事务：refcount/consumers 门禁 -> quiesce+drain -> revert exports ->
 *      exit/fini -> unbind -> 释放槽位与镜像；
 *   4. 热替换事务：prepare 新版本镜像 -> provider + 直接消费者 quiesce/drain
 *      （每模块预算，超时 KMOD_ERR_BUDGET）-> 原子切换 img/exports/version/
 *      generation -> 重绑定消费者导入槽 -> 重新发布 seat/syscall -> 新 init；
 *      任一步失败恢复旧镜像/旧槽/旧座位/syscall/READY 状态；
 *   5. epoch 进入/退出（__atomic 双检）与 quiesce 排空看门狗。
 *
 * 并发/锁约定（重要）：
 *   - g_kmod_lock（core 定义）只保护注册表/镜像池登记表的**短临界区**；
 *     本文件所有公共入口自行取锁，调用方（kmod_core）不得持锁调用；
 *   - init/exit/quiesce/drain 回调一律在**不持全局锁**的情况下调用（模块内
 *     可以安全回调公共 API）；同理 seat/syscall 提交原语也在锁外调用；
 *   - 事务模型是"单写者"的：V1 假设同一时刻只有一个加载/热替换事务在跑
 *     （引导期 + selftest 串行；运行期并发由上层策略收敛），但所有注册表
 *     状态变更仍在锁内完成，读到的一致性不依赖该假设。
 *
 * 槽位策略：固定数组 + tombstone，不压缩（deps/consumers/binding.provider 都
 * 存的是稳定下标）。g_kmod_module_count 维护为"已用槽位高水位"，释放尾槽会
 * 回退；core 的 KmodModuleInfoAt 需跳过 state==EMPTY 的槽位。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#include <stdint.h>
#include <stddef.h>

namespace {

constexpr uint32_t MOD_MAX = KMOD_MAX_MODULES;
constexpr uint32_t IMP_MAX = KMOD_MAX_IMPORTS;
constexpr uint32_t DEP_MAX = KMOD_MAX_MODULE_DEPS;
constexpr uint32_t CONSUMER_MAX = KMOD_MAX_MODULE_CONSUMERS;
constexpr uint32_t EXPORT_MAX = KMOD_MAX_EXPORTS;

/* 事务提交阶段（回滚函数据此决定撤销到哪一步） */
enum {
    ST_NONE = 0,
    ST_ALLOC = 1,   /* 槽位+镜像已分配 */
    ST_BOUND = 2,   /* 导入已绑定 */
    ST_APPLIED = 3, /* seat/syscall 已发布 */
    ST_INITED = 4,  /* init 回调已成功 */
};

/* prepare 阶段缓存的临时镜像：commit 时由 KmodModuleAllocFromImage 采纳，
 * 避免二次加载；事务失败统一按 token 释放，不泄漏。 */
struct prep_slot {
    bool used;
    uint32_t token;
    int32_t pool_index;
    kmod_elf_image_t img;
};

prep_slot g_prep[MOD_MAX];
uint32_t g_tx_seq = 0u;
uint64_t g_load_seq = 0u;

/* ------------------------------------------------------------------ */
/* 基础工具                                                           */
/* ------------------------------------------------------------------ */

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

inline uint64_t str_len(const char *s)
{
    uint64_t n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
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

inline void mem_zero(void *p, uint64_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++)
        b[i] = 0;
}

inline uint16_t state_get(kmod_module_t *m)
{
    return __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
}

inline void state_set(kmod_module_t *m, uint16_t s)
{
    __atomic_store_n(&m->state, s, __ATOMIC_RELEASE);
}

inline uint32_t epoch_get(kmod_module_t *m)
{
    return __atomic_load_n((uint32_t *)&m->epoch_active, __ATOMIC_ACQUIRE);
}

inline uint64_t reg_lock(void)
{
    return kmod_lock_acquire(&g_kmod_lock);
}

inline void reg_unlock(uint64_t flags)
{
    kmod_lock_release(&g_kmod_lock, flags);
}

inline int32_t module_index_of(const kmod_module_t *m)
{
    return (int32_t)(m - g_kmod_modules);
}

/* ------------------------------------------------------------------ */
/* 注册表（全部 *_locked 约定：调用方已持 g_kmod_lock）                */
/* ------------------------------------------------------------------ */

kmod_module_t *module_find_locked(const char *name)
{
    if (!name)
        return nullptr;
    for (uint32_t i = 0; i < g_kmod_module_count && i < MOD_MAX; i++) {
        kmod_module_t *m = &g_kmod_modules[i];
        uint16_t st = state_get(m);
        if (st == KMOD_STATE_EMPTY || st == KMOD_STATE_REMOVED)
            continue;
        if (str_eq(m->name, name))
            return m;
    }
    return nullptr;
}

int32_t module_index_locked(const char *name)
{
    kmod_module_t *m = module_find_locked(name);
    if (!m)
        return KMOD_ERR_NOTFOUND;
    return module_index_of(m);
}

kmod_module_t *module_claim_locked(void)
{
    for (uint32_t i = 0; i < MOD_MAX; i++) {
        kmod_module_t *m = &g_kmod_modules[i];
        if (state_get(m) != KMOD_STATE_EMPTY)
            continue;
        mem_zero(m, sizeof(*m));
        state_set(m, KMOD_STATE_LOADING);
        if (i >= g_kmod_module_count)
            g_kmod_module_count = i + 1u;
        return m;
    }
    return nullptr;
}

/* 清空槽位；镜像所有权通过 out_img 交还调用方（锁外 KmodElfFreeImage）。
 * out_internal_flags 回传释放前的 internal_flags：BUILTIN 组件的
 * manifest/导入/导出数组是内核静态镜像，释放时不得 free（mem==NULL）。 */
bool module_release_locked(kmod_module_t *m, kmod_elf_image_t *out_img,
                           uint32_t *out_internal_flags)
{
    if (!m)
        return false;
    uint16_t st = state_get(m);
    if (st == KMOD_STATE_EMPTY)
        return false;
    if (m->n_consumers != 0u || m->n_bindings != 0u || m->refcount != 0u) {
        kmod_log("[kmod] release %s with residual refs (consumers=%u bindings=%u "
                 "refcount=%u)\n",
                 m->name, (unsigned)m->n_consumers, (unsigned)m->n_bindings,
                 (unsigned)m->refcount);
    }
    if (out_internal_flags)
        *out_internal_flags = m->internal_flags;
    if (out_img) {
        *out_img = m->img;
    } else {
        /* 调用方必须接管镜像；这里只清描述符，避免悬空 */
        m->img.mem = nullptr;
    }
    state_set(m, KMOD_STATE_REMOVED);
    mem_zero(m, sizeof(*m)); /* state 归零 = EMPTY，名字清空（Find 自然跳过） */
    while (g_kmod_module_count > 0u &&
           state_get(&g_kmod_modules[g_kmod_module_count - 1u]) == KMOD_STATE_EMPTY) {
        g_kmod_module_count--;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* prepare 缓存                                                       */
/* ------------------------------------------------------------------ */

int32_t prep_lookup_locked(int32_t pool_index)
{
    for (uint32_t i = 0; i < MOD_MAX; i++) {
        if (g_prep[i].used && g_prep[i].pool_index == pool_index)
            return (int32_t)i;
    }
    return -1;
}

/* 登记临时镜像（所有权移入缓存并清零调用方的 img）。返回槽位下标或负错误。 */
int32_t prep_register(uint32_t token, int32_t pool_index, kmod_elf_image_t *img)
{
    uint64_t lf = reg_lock();
    int32_t slot = -1;
    for (uint32_t i = 0; i < MOD_MAX; i++) {
        if (!g_prep[i].used) {
            slot = (int32_t)i;
            break;
        }
    }
    if (slot < 0) {
        reg_unlock(lf);
        return KMOD_ERR_NOMEM;
    }
    g_prep[slot].used = true;
    g_prep[slot].token = token;
    g_prep[slot].pool_index = pool_index;
    g_prep[slot].img = *img;
    mem_zero(img, sizeof(*img));
    reg_unlock(lf);
    return slot;
}

void prep_cleanup_token(uint32_t token)
{
    for (;;) {
        kmod_elf_image_t img;
        mem_zero(&img, sizeof(img));
        bool found = false;
        uint64_t lf = reg_lock();
        for (uint32_t i = 0; i < MOD_MAX; i++) {
            if (g_prep[i].used && g_prep[i].token == token) {
                img = g_prep[i].img;
                mem_zero(&g_prep[i].img, sizeof(g_prep[i].img));
                g_prep[i].used = false;
                found = true;
                break;
            }
        }
        reg_unlock(lf);
        if (!found)
            return;
        KmodElfFreeImage(&img); /* 锁外释放 */
    }
}

/* ------------------------------------------------------------------ */
/* 依赖边 / 消费者边                                                  */
/* ------------------------------------------------------------------ */

int32_t module_add_dep_locked(kmod_module_t *m, uint32_t provider_idx)
{
    if (provider_idx == (uint32_t)module_index_of(m))
        return KMOD_ERR_CYCLE; /* 自依赖按环处理 */
    for (uint32_t i = 0; i < m->n_deps; i++) {
        if ((uint32_t)m->deps[i] == provider_idx)
            return KMOD_OK;
    }
    if (m->n_deps >= DEP_MAX)
        return KMOD_ERR_DEPTH;
    m->deps[m->n_deps++] = (uint16_t)provider_idx;
    return KMOD_OK;
}

int32_t provider_add_consumer_locked(kmod_module_t *p, uint32_t consumer_idx)
{
    for (uint32_t i = 0; i < p->n_consumers; i++) {
        if ((uint32_t)p->consumers[i] == consumer_idx)
            return KMOD_OK;
    }
    if (p->n_consumers >= CONSUMER_MAX)
        return KMOD_ERR_DEPTH;
    p->consumers[p->n_consumers++] = (uint16_t)consumer_idx;
    return KMOD_OK;
}

void provider_remove_consumer_locked(kmod_module_t *p, uint32_t consumer_idx)
{
    for (uint32_t i = 0; i < p->n_consumers; i++) {
        if ((uint32_t)p->consumers[i] == consumer_idx) {
            for (uint32_t k = i + 1u; k < p->n_consumers; k++)
                p->consumers[k - 1u] = p->consumers[k];
            p->n_consumers--;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 符号解析                                                           */
/* ------------------------------------------------------------------ */

/* 在 provider 导出表中查找导入符号，并做契约/版本/尺寸的机械校验。 */
int32_t find_export_in(const kmod_export_t *exports, uint32_t n,
                       const kmod_import_t *imp, uint32_t *idx_out)
{
    if (!imp->symbol || imp->symbol[0] == '\0')
        return KMOD_ERR_FORMAT;
    for (uint32_t i = 0; i < n; i++) {
        const kmod_export_t *e = &exports[i];
        if (!e->symbol || !str_eq(e->symbol, imp->symbol))
            continue;
        /* 契约命名空间：导入声明了就必须与导出完全一致（空契约无法机械校验） */
        if (imp->contract && imp->contract[0]) {
            if (!e->contract || e->contract[0] == '\0' ||
                !str_eq(e->contract, imp->contract))
                return KMOD_ERR_CONTRACT;
        }
        int32_t rc = KmodVersionInRange(e->abi_version, imp->abi_min, imp->abi_max);
        if (rc != KMOD_OK)
            return rc;
        if (imp->expect_size != 0u && e->size != 0u && imp->expect_size != e->size)
            return KMOD_ERR_CONTRACT;
        if (idx_out)
            *idx_out = i;
        return KMOD_OK;
    }
    return KMOD_ERR_NOTFOUND;
}

/* provider="kernel" 的导入解析：与模块间导入同等校验
 *   - contract 非空 ⇒ 导出 contract 必须完全相等；
 *   - abi 必须落在 [abi_min, abi_max]（0 = 不设上界）；
 *   - expect_size 非 0 ⇒ 导出 size 必须相等且 kind 为 CONTRACT/DATA
 *     （FUNC 的 size 恒为 0，不会误通过）；用于 kernel.klib/sync/algo 等
 *     ops 结构体的布局强校验，失败时整个加载事务回滚。 */
int32_t find_kernel_export_in(const kmod_kernel_export_t *kexp, uint32_t n,
                              const kmod_import_t *imp, void **addr_out,
                              uint32_t *idx_out)
{
    if (!imp || !imp->symbol || !kexp)
        return KMOD_ERR_FORMAT;
    for (uint32_t i = 0; i < n; i++) {
        const kmod_kernel_export_t *e = &kexp[i];
        if (!e->name || !str_eq(e->name, imp->symbol))
            continue;
        if (imp->contract && imp->contract[0]) {
            if (!e->contract || e->contract[0] == '\0' ||
                !str_eq(e->contract, imp->contract))
                return KMOD_ERR_CONTRACT;
        }
        int32_t rc = KmodVersionInRange(e->abi_version, imp->abi_min,
                                        imp->abi_max);
        if (rc != KMOD_OK)
            return rc;
        if (imp->expect_size != 0u) {
            if (e->size != imp->expect_size ||
                (e->kind != KMOD_SYM_CONTRACT && e->kind != KMOD_SYM_DATA))
                return KMOD_ERR_CONTRACT;
        }
        if (addr_out)
            *addr_out = e->addr;
        if (idx_out)
            *idx_out = i;
        return KMOD_OK;
    }
    return KMOD_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/* 绑定                                                               */
/* ------------------------------------------------------------------ */

int32_t bind_one_locked(kmod_module_t *m, uint32_t midx, uint32_t import_index,
                        const kmod_import_t *imp,
                        const kmod_kernel_export_t *kexp, uint32_t nkexp)
{
    if (m->n_bindings >= IMP_MAX)
        return KMOD_ERR_NOMEM;
    kmod_binding_t *b = &m->bindings[m->n_bindings];
    mem_zero(b, sizeof(*b));
    b->import_index = import_index;
    b->provider = -1;
    bool optional = (imp->flags & KMOD_IMP_OPTIONAL) != 0u;
    bool weak = (imp->flags & KMOD_IMP_WEAK) != 0u;
    bool soft = optional || weak;
    bool dep_edge = (imp->flags & KMOD_IMP_DEP_EDGE) != 0u;
    b->optional = optional;
    b->dep_edge_only = dep_edge;

    if (!imp->provider || imp->provider[0] == '\0')
        return KMOD_ERR_FORMAT;

    if (str_eq(imp->provider, "kernel")) {
        if (dep_edge) {
            /* 指向内核的纯依赖边没有语义，容忍并记录审计 */
            m->n_bindings++;
            return KMOD_OK;
        }
        void *addr = nullptr;
        uint32_t kidx = 0;
        int32_t krc = find_kernel_export_in(kexp, nkexp, imp, &addr, &kidx);
        if (krc != KMOD_OK) {
            /* 可选/弱导入只容忍"找不到"；契约/版本/尺寸不匹配属于硬错误，
             * 由加载事务回滚（不能静默拿到一个布局不符的 ops 指针）。 */
            if (soft && krc == KMOD_ERR_NOTFOUND) {
                m->n_bindings++;
                return KMOD_OK;
            }
            return krc;
        }
        b->resolved = addr;
        b->provider_export_index = kidx;
        if (imp->slot)
            *(imp->slot) = addr;
        m->n_bindings++;
        return KMOD_OK;
    }

    kmod_module_t *p = module_find_locked(imp->provider);
    if (!p) {
        if (soft) {
            m->n_bindings++;
            return KMOD_OK; /* 槽保持 NULL（KMOD_IMPORT_READY 为假） */
        }
        return KMOD_ERR_UNRESOLVED;
    }
    uint16_t pst = state_get(p);
    if (pst != KMOD_STATE_READY)
        return KMOD_ERR_BUSY; /* provider 未完成加载（不应出现在拓扑序提交中） */
    int32_t pidx = module_index_of(p);
    if (pidx == (int32_t)midx)
        return KMOD_ERR_CYCLE;

    if (!dep_edge) {
        uint32_t eidx = 0;
        int32_t rc = find_export_in(p->exports, p->n_exports, imp, &eidx);
        if (rc == KMOD_ERR_NOTFOUND) {
            if (soft) {
                m->n_bindings++;
                return KMOD_OK;
            }
            return KMOD_ERR_UNRESOLVED;
        }
        if (rc != KMOD_OK)
            return rc;
        b->resolved = p->exports[eidx].addr;
        b->provider_export_index = eidx;
    }
    b->provider = pidx;
    int32_t rc = module_add_dep_locked(m, (uint32_t)pidx);
    if (rc != KMOD_OK)
        return rc;
    rc = provider_add_consumer_locked(p, midx);
    if (rc != KMOD_OK)
        return rc;
    __atomic_add_fetch(&p->refcount, 1u, __ATOMIC_ACQ_REL);
    if (!dep_edge && imp->slot)
        *(imp->slot) = b->resolved;
    m->n_bindings++;
    return KMOD_OK;
}

/* 解除本模块的导入边/消费者登记。
 * clear_slots=false 用于热替换：旧镜像的导入槽要保持有效，直到旧版本
 * exit/fini 回调执行完（exit 常常通过导入的 kprintf_ 等内核符号打印日志，
 * 提前清槽会让回调跳到 NULL）。旧镜像随后由事务释放，槽自然失效。 */
void unbind_edges_locked(kmod_module_t *m, bool clear_slots)
{
    if (!m)
        return;
    uint32_t midx = (uint32_t)module_index_of(m);
    for (uint32_t k = 0; k < m->n_bindings; k++) {
        kmod_binding_t *b = &m->bindings[k];
        if (b->provider >= 0 && b->provider < (int32_t)MOD_MAX) {
            kmod_module_t *p = &g_kmod_modules[b->provider];
            uint16_t pst = state_get(p);
            if (pst != KMOD_STATE_EMPTY && pst != KMOD_STATE_REMOVED) {
                provider_remove_consumer_locked(p, midx);
                if (p->refcount > 0u) {
                    uint32_t old = __atomic_sub_fetch(&p->refcount, 1u,
                                                      __ATOMIC_ACQ_REL);
                    if (old == 0u)
                        __atomic_store_n(&p->refcount, 0u, __ATOMIC_RELAXED);
                }
            }
        }
        if (clear_slots && !b->dep_edge_only && m->imports &&
            b->import_index < m->n_imports) {
            kmod_import_t *imp = &m->imports[b->import_index];
            if (imp->slot)
                *(imp->slot) = nullptr;
        }
    }
    m->n_bindings = 0u;
    m->n_deps = 0u;
    mem_zero(m->bindings, sizeof(m->bindings));
}

void unbind_all_locked(kmod_module_t *m)
{
    unbind_edges_locked(m, true);
}

/* ------------------------------------------------------------------ */
/* 生命周期回调（全部在锁外调用）                                      */
/* ------------------------------------------------------------------ */

void call_init_image(const kmod_elf_image_t *img)
{
    if (!img)
        return;
    for (uint32_t i = 0; i < img->n_init_array; i++) {
        void (*fn)(void) = img->init_array ? img->init_array[i] : nullptr;
        if (fn)
            fn();
    }
}

void call_exit_image(const kmod_elf_image_t *img)
{
    if (!img)
        return;
    if (img->manifest && img->manifest->exit_hook)
        img->manifest->exit_hook();
    for (uint32_t i = img->n_fini_array; i > 0u; i--) {
        void (*fn)(void) = img->fini_array ? img->fini_array[i - 1u] : nullptr;
        if (fn)
            fn();
    }
}

/* ------------------------------------------------------------------ */
/* 统计 / 错误映射                                                    */
/* ------------------------------------------------------------------ */

void note_reject(int32_t rc)
{
    switch (rc) {
    case KMOD_ERR_CAP:
        g_kmod_stats.reject_cap++;
        break;
    case KMOD_ERR_CYCLE:
        g_kmod_stats.reject_cycle++;
        break;
    case KMOD_ERR_VERSION:
        g_kmod_stats.reject_version++;
        break;
    case KMOD_ERR_FORMAT:
    case KMOD_ERR_IMAGE:
    case KMOD_ERR_CONTRACT:
        g_kmod_stats.reject_format++;
        break;
    case KMOD_ERR_RELOC:
        g_kmod_stats.reject_reloc++;
        break;
    case KMOD_ERR_NAME:
        g_kmod_stats.reject_name++;
        break;
    case KMOD_ERR_POLICY:
        g_kmod_stats.reject_policy++;
        break;
    case KMOD_ERR_BUSY:
        g_kmod_stats.reject_busy++;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* prepare：加载并校验镜像池里的一个 .ko                              */
/* ------------------------------------------------------------------ */

int32_t gov_prepare_image(int32_t pool_index, uint32_t token, uint32_t flags,
                          int32_t *prep_out)
{
    *prep_out = -1;
    if (pool_index < 0 || (uint32_t)pool_index >= g_kmod_image_count)
        return KMOD_ERR_NOTFOUND;
    kmod_image_t pi = g_kmod_images[pool_index]; /* 结构拷贝：data 指向稳定 blob */
    if (!pi.data || pi.size == 0u || pi.size > KMOD_MAX_ELF_SIZE)
        return KMOD_ERR_IMAGE;

    const kmod_kernel_export_t *kexp = nullptr;
    uint32_t nkexp = 0;
    KmodKernelExports(&kexp, &nkexp);

    kmod_elf_image_t img;
    mem_zero(&img, sizeof(img));
    char err[128];
    err[0] = '\0';
    int32_t rc = KmodElfLoadImage(pi.data, pi.size, kexp, nkexp, &img, err,
                                  sizeof(err));
    if (rc != KMOD_OK) {
        kmod_log("[kmod] prepare %s@%s: load rc=%d (%s) %s\n", pi.name, pi.version,
                 (int)rc, KmodElfErrorString(rc), err);
        return rc;
    }
    if (!img.manifest || !img.manifest->name[0]) {
        KmodElfFreeImage(&img);
        return KMOD_ERR_IMAGE;
    }
    rc = KmodCheckLoadable(img.manifest, flags);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] prepare %s@%s: loadable rc=%d (%s)\n", pi.name,
                 pi.version, (int)rc, KmodElfErrorString(rc));
        KmodElfFreeImage(&img);
        return rc;
    }
    rc = prep_register(token, pool_index, &img);
    if (rc < 0) {
        KmodElfFreeImage(&img);
        return rc;
    }
    *prep_out = rc;
    return KMOD_OK;
}

/* 丢弃一个 prepare 缓存项（同名不同版本去重时用）：锁外释放镜像 */
void prep_discard(int32_t slot)
{
    kmod_elf_image_t img;
    mem_zero(&img, sizeof(img));
    uint64_t lf = reg_lock();
    if (slot >= 0 && (uint32_t)slot < MOD_MAX && g_prep[slot].used) {
        img = g_prep[slot].img;
        mem_zero(&g_prep[slot].img, sizeof(g_prep[slot].img));
        g_prep[slot].used = false;
    }
    reg_unlock(lf);
    KmodElfFreeImage(&img);
}

/* 依赖闭包扩展：去重后把 pool_index 的镜像加入 staged 列表。 */
int32_t gov_stage_add(uint32_t token, uint32_t flags, int32_t pool_index,
                      int32_t *staged_pool, int32_t *staged_prep,
                      uint32_t *n_staged)
{
    for (uint32_t i = 0; i < *n_staged; i++) {
        if (staged_pool[i] == pool_index)
            return KMOD_OK;
    }
    if (*n_staged >= MOD_MAX)
        return KMOD_ERR_DEPTH;
    int32_t ps = -1;
    int32_t rc = gov_prepare_image(pool_index, token, flags, &ps);
    if (rc != KMOD_OK)
        return rc;
    /* 同名不同版本不允许同时进入同一事务（以先规划者为准，避免 commit 时 EXISTS） */
    const char *new_name = g_prep[ps].img.manifest ? g_prep[ps].img.manifest->name
                                                   : nullptr;
    if (new_name) {
        for (uint32_t i = 0; i < *n_staged; i++) {
            const char *nm = g_prep[staged_prep[i]].img.manifest
                                 ? g_prep[staged_prep[i]].img.manifest->name
                                 : nullptr;
            if (nm && str_eq(nm, new_name)) {
                prep_discard(ps);
                return KMOD_OK;
            }
        }
    }
    staged_pool[*n_staged] = pool_index;
    staged_prep[*n_staged] = ps;
    (*n_staged)++;
    return KMOD_OK;
}

} /* namespace */

/* ==================================================================== */
/* 公共入口                                                              */
/* ==================================================================== */

extern "C" {

int32_t KmodGovInit(void)
{
    uint64_t lf = reg_lock();
    for (uint32_t i = 0; i < MOD_MAX; i++) {
        g_prep[i].used = false;
        g_prep[i].token = 0u;
        g_prep[i].pool_index = -1;
        mem_zero(&g_prep[i].img, sizeof(g_prep[i].img));
    }
    g_tx_seq = 0u;
    g_load_seq = 0u;
    reg_unlock(lf);
    kmod_log("[kmod] governance init: registry=slots/%u, tx/prep tables ready\n",
             (unsigned)MOD_MAX);
    return KMOD_OK;
}

/* ------------------------------------------------------------------ */
/* 运行时模块表辅助                                                    */
/* ------------------------------------------------------------------ */

kmod_module_t *KmodModuleFind(const char *name)
{
    if (!name)
        return nullptr;
    uint64_t lf = reg_lock();
    kmod_module_t *m = module_find_locked(name);
    reg_unlock(lf);
    return m;
}

int32_t KmodModuleIndexOf(const char *name)
{
    if (!name)
        return KMOD_ERR_INVAL;
    uint64_t lf = reg_lock();
    int32_t idx = module_index_locked(name);
    reg_unlock(lf);
    return idx;
}

int32_t KmodModuleAllocFromImage(int32_t pool_index, uint32_t flags,
                                 int32_t *index_out)
{
    if (!index_out)
        return KMOD_ERR_INVAL;
    *index_out = -1;
    if (pool_index < 0 || (uint32_t)pool_index >= g_kmod_image_count)
        return KMOD_ERR_NOTFOUND;
    kmod_image_t pi = g_kmod_images[pool_index];
    if (!pi.data || pi.size == 0u)
        return KMOD_ERR_IMAGE;
    if (pi.parse_ret != KMOD_OK)
        return pi.parse_ret;

    /* 1) 采纳 prepare 缓存，否则即时加载 */
    kmod_elf_image_t img;
    mem_zero(&img, sizeof(img));
    bool loaded = false;
    uint64_t lf = reg_lock();
    int32_t slot = prep_lookup_locked(pool_index);
    if (slot >= 0) {
        img = g_prep[slot].img;
        mem_zero(&g_prep[slot].img, sizeof(g_prep[slot].img));
        g_prep[slot].used = false;
        loaded = true;
    }
    reg_unlock(lf);
    if (!loaded) {
        const kmod_kernel_export_t *kexp = nullptr;
        uint32_t nkexp = 0;
        KmodKernelExports(&kexp, &nkexp);
        char err[128];
        err[0] = '\0';
        int32_t lrc = KmodElfLoadImage(pi.data, pi.size, kexp, nkexp, &img, err,
                                       sizeof(err));
        if (lrc != KMOD_OK) {
            kmod_log("[kmod] alloc %s@%s: load rc=%d (%s)\n", pi.name, pi.version,
                     (int)lrc, KmodElfErrorString(lrc));
            return lrc;
        }
    }
    const kmod_manifest_t *man = img.manifest;
    if (!man) {
        KmodElfFreeImage(&img);
        return KMOD_ERR_IMAGE;
    }

    /* 2) 能力授权（策略 + 清单交集；保留/未定义位在这里也会被拒） */
    uint64_t granted = 0ull, denied = 0ull;
    int32_t rc = KmodCapGrant(man->name, man->required_caps, man->optional_caps,
                              &granted, &denied);
    if (rc != KMOD_OK) {
        KmodElfFreeImage(&img);
        return rc;
    }

    /* 3) 占用槽位并登记 */
    lf = reg_lock();
    if (module_find_locked(man->name)) {
        reg_unlock(lf);
        KmodElfFreeImage(&img);
        return KMOD_ERR_EXISTS;
    }
    kmod_module_t *m = module_claim_locked();
    if (!m) {
        reg_unlock(lf);
        KmodElfFreeImage(&img);
        return KMOD_ERR_NOMEM;
    }
    int32_t idx = module_index_of(m);
    str_copy(m->name, KMOD_NAME_MAX, man->name);
    str_copy(m->version, KMOD_VERSION_MAX, man->version);
    m->tier = man->tier;
    m->flags = man->flags;
    m->caps_required = man->required_caps;
    m->caps_optional = man->optional_caps;
    m->caps_granted = granted;
    m->caps_denied = denied;
    m->pool_index = pool_index;
    m->generation = 0u;
    m->load_seq = ++g_load_seq;
    m->img = img;
    m->exports = m->img.exports;
    m->n_exports = m->img.n_exports;
    m->imports = m->img.imports;
    m->n_imports = m->img.n_imports;
    m->refcount = 0u;
    m->n_bindings = 0u;
    m->n_deps = 0u;
    m->n_consumers = 0u;
    m->epoch_active = 0u;
    m->epoch_max_seen = 0u;
    m->gen_exports_hash = KmodExportsHash(m->exports, m->n_exports);
    uint64_t seq = m->load_seq;
    reg_unlock(lf);

    kmod_log("[kmod] alloc %s@%s -> slot #%d (seq=%llu tier=%u caps=0x%llx/0x%llx)\n",
             man->name, man->version, (int)idx, (unsigned long long)seq,
             (unsigned)man->tier, (unsigned long long)granted,
             (unsigned long long)denied);
    *index_out = idx;
    return KMOD_OK;
}

void KmodModuleRelease(kmod_module_t *m)
{
    if (!m)
        return;
    kmod_elf_image_t img;
    mem_zero(&img, sizeof(img));
    uint32_t iflags = 0u;
    uint64_t lf = reg_lock();
    bool released = module_release_locked(m, &img, &iflags);
    reg_unlock(lf);
    /* 内建组件的 manifest/.in/.out 在内核静态镜像里，绝不能 free */
    if (released && (iflags & KMOD_MODF_BUILTIN) == 0u)
        KmodElfFreeImage(&img);
}

int32_t KmodCheckLoadable(const kmod_manifest_t *m, uint32_t flags)
{
    if (!m)
        return KMOD_ERR_INVAL;
    if (m->magic != KMOD_MANIFEST_MAGIC)
        return KMOD_ERR_FORMAT;
    if (m->abi_version != KMOD_ABI_VERSION)
        return KMOD_ERR_VERSION;
    if (m->tier != KMOD_TIER_POLICY && m->tier != KMOD_TIER_IMPL)
        return KMOD_ERR_TIER;
    int32_t rc = KmodNameValidate(m->name);
    if (rc != KMOD_OK)
        return rc;
    rc = KmodVersionParse(m->version, nullptr, nullptr, nullptr);
    if (rc != KMOD_OK)
        return rc;
    const uint32_t known_flags = KMOD_MF_BOOT | KMOD_MF_HOTREPLACE |
                                 KMOD_MF_UNLOADABLE | KMOD_MF_DEBUG |
                                 KMOD_MF_NO_AUTODEP | KMOD_MF_EXPERIMENTAL;
    if (m->flags & ~known_flags)
        return KMOD_ERR_FORMAT;

    uint32_t mode = KmodCapMode();
    if (mode == KMOD_POLICY_DISABLED)
        return KMOD_ERR_POLICY;
    if ((m->flags & KMOD_MF_EXPERIMENTAL) && mode == KMOD_POLICY_STRICT &&
        !(flags & KMOD_LOAD_FORCE))
        return KMOD_ERR_POLICY;
    /* 参与热替换的模块必须显式声明并申请 HOT_REPLACE 能力（Tier0 治理断言） */
    if ((m->flags & KMOD_MF_HOTREPLACE) &&
        (m->required_caps & KMOD_CAP_HOT_REPLACE) == 0ull)
        return KMOD_ERR_CAP;

    uint64_t granted = 0ull, denied = 0ull;
    return KmodCapGrant(m->name, m->required_caps, m->optional_caps, &granted,
                        &denied);
}

/* ------------------------------------------------------------------ */
/* 提交阶段原语                                                        */
/* ------------------------------------------------------------------ */

int32_t KmodBindImports(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    const kmod_kernel_export_t *kexp = nullptr;
    uint32_t nkexp = 0;
    KmodKernelExports(&kexp, &nkexp);

    uint64_t lf = reg_lock();
    uint16_t st = state_get(m);
    /* LOADING/REPLACING：普通 .ko 事务；RESOLVING：内建组件提交（builtin.cpp） */
    if (st != KMOD_STATE_LOADING && st != KMOD_STATE_REPLACING &&
        st != KMOD_STATE_RESOLVING) {
        reg_unlock(lf);
        return KMOD_ERR_STATE;
    }
    if (m->n_bindings != 0u)
        unbind_all_locked(m); /* 防御：清掉残留绑定 */
    uint32_t n_imp = m->n_imports;
    if (n_imp > IMP_MAX || (n_imp > 0u && !m->imports)) {
        reg_unlock(lf);
        return KMOD_ERR_FORMAT;
    }
    uint32_t midx = (uint32_t)module_index_of(m);
    int32_t rc = KMOD_OK;
    for (uint32_t i = 0; i < n_imp; i++) {
        rc = bind_one_locked(m, midx, i, &m->imports[i], kexp, nkexp);
        if (rc != KMOD_OK)
            break;
    }
    if (rc != KMOD_OK) {
        kmod_log("[kmod] bind %s: import #%u failed rc=%d (%s)\n", m->name,
                 (unsigned)m->n_bindings, (int)rc, KmodElfErrorString(rc));
        unbind_all_locked(m); /* 回滚本次已建立的边/槽 */
    }
    reg_unlock(lf);
    return rc;
}

void KmodUnbindAll(kmod_module_t *m)
{
    if (!m)
        return;
    uint64_t lf = reg_lock();
    unbind_all_locked(m);
    reg_unlock(lf);
}

int32_t KmodApplyExports(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    uint64_t lf = reg_lock();
    if (m->n_exports > EXPORT_MAX ||
        (m->n_exports > 0u && !m->exports) ||
        m->exports != m->img.exports || m->n_exports != m->img.n_exports) {
        reg_unlock(lf);
        return KMOD_ERR_STATE;
    }
    reg_unlock(lf);

    int32_t rc = KmodSeatAttachModule(m);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] apply %s: seat attach rc=%d (%s)\n", m->name, (int)rc,
                 KmodElfErrorString(rc));
        return rc;
    }
    rc = KmodSyscallRegisterModule(m);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] apply %s: syscall register rc=%d (%s); seat rollback\n",
                 m->name, (int)rc, KmodElfErrorString(rc));
        KmodSeatDetachModule(m);
        return rc;
    }
    return KMOD_OK;
}

void KmodRevertExports(kmod_module_t *m)
{
    if (!m)
        return;
    KmodSyscallUnregisterModule(m);
    KmodSeatDetachModule(m);
}

int32_t KmodCallInit(kmod_module_t *m)
{
    if (!m)
        return KMOD_ERR_INVAL;
    kmod_elf_image_t img;
    uint32_t budget = 0u;
    uint64_t lf = reg_lock();
    img = m->img; /* 描述符拷贝；内存所有权仍归模块 */
    if (img.manifest)
        budget = img.manifest->init_budget_ms;
    reg_unlock(lf);
    if (!img.manifest)
        return KMOD_ERR_STATE;
    if (budget == 0u || budget > KMOD_INIT_BUDGET_MAX_MS)
        budget = KMOD_DEFAULT_INIT_BUDGET_MS;

    uint64_t t0 = kmod_uptime_ms();
    call_init_image(&img); /* init_array 先于 manifest->init */
    int32_t rc = KMOD_OK;
    if (img.manifest->init)
        rc = img.manifest->init();

    if (rc != KMOD_OK) {
        lf = reg_lock();
        state_set(m, KMOD_STATE_FAILED);
        reg_unlock(lf);
        kmod_log("[kmod] init %s failed rc=%d\n", m->name, (int)rc);
        return rc;
    }
    uint64_t elapsed = kmod_uptime_ms() - t0;
    if (kmod_uptime_ms() != 0u && elapsed > (uint64_t)budget) {
        g_kmod_stats.budget_exceeded++;
        kmod_log("[kmod] init %s over budget (%llu ms > %u ms), accepted\n",
                 m->name, (unsigned long long)elapsed, (unsigned)budget);
    }
    lf = reg_lock();
    state_set(m, KMOD_STATE_READY);
    reg_unlock(lf);
    return KMOD_OK;
}

void KmodCallExit(kmod_module_t *m)
{
    if (!m)
        return;
    kmod_elf_image_t img;
    uint64_t lf = reg_lock();
    img = m->img;
    reg_unlock(lf);
    call_exit_image(&img);
}

int32_t KmodQuiesceAndDrain(kmod_module_t *m, uint32_t budget_ms)
{
    if (!m)
        return KMOD_ERR_INVAL;
    void (*quiesce_fn)(void) = nullptr;
    int32_t (*drain_fn)(void) = nullptr;
    uint32_t budget = 0u;

    uint64_t lf = reg_lock();
    uint16_t st = state_get(m);
    if (st == KMOD_STATE_EMPTY || st == KMOD_STATE_REMOVED) {
        reg_unlock(lf);
        return KMOD_ERR_STATE;
    }
    state_set(m, KMOD_STATE_QUIESCING);
    if (m->img.manifest) {
        quiesce_fn = m->img.manifest->quiesce;
        drain_fn = m->img.manifest->drain;
        budget = m->img.manifest->quiesce_budget_ms;
    }
    reg_unlock(lf);

    if (budget_ms != 0u)
        budget = budget_ms;
    if (budget == 0u || budget > KMOD_QUIESCE_BUDGET_MAX_MS)
        budget = KMOD_DEFAULT_QUIESCE_BUDGET_MS;

    if (quiesce_fn)
        quiesce_fn();
    if (drain_fn) {
        int32_t first = drain_fn();
        if (first < 0)
            kmod_log("[kmod] quiesce %s: drain returned %d\n", m->name, (int)first);
    }

    uint64_t t0 = kmod_uptime_ms();
    uint64_t deadline = t0 + (uint64_t)budget;
    uint64_t rounds = 0u;
    uint64_t max_rounds = (uint64_t)budget + 1000ull;
    for (;;) {
        uint32_t active = epoch_get(m);
        int32_t remain = -1;
        if (drain_fn)
            remain = drain_fn();
        if (active == 0u && (!drain_fn || remain <= 0))
            break;
        uint64_t now = kmod_uptime_ms();
        if ((now != 0u && now >= deadline) || rounds >= max_rounds) {
            g_kmod_stats.budget_exceeded++;
            g_kmod_stats.epoch_waits++;
            lf = reg_lock();
            state_set(m, KMOD_STATE_READY); /* 超时：恢复 READY，放弃本事务 */
            reg_unlock(lf);
            kmod_log("[kmod] quiesce %s TIMEOUT (epoch=%u remain=%d budget=%u ms)\n",
                     m->name, (unsigned)epoch_get(m), (int)remain, (unsigned)budget);
            return KMOD_ERR_BUDGET;
        }
        g_kmod_stats.epoch_waits++;
        rounds++;
        kmod_wait_ms(1u);
    }
    return KMOD_OK;
}

int32_t KmodRebindConsumers(kmod_module_t *provider,
                            const kmod_export_t *old_exports, uint32_t n_old,
                            const kmod_export_t *new_exports, uint32_t n_new)
{
    if (!provider)
        return KMOD_ERR_INVAL;
    int32_t pidx = module_index_of(provider);
    if (pidx < 0)
        return KMOD_ERR_INVAL;

    uint64_t lf = reg_lock();
    /* 阶段 1：全量预校验（不改状态）；任一消费者不兼容 -> 整体失败 */
    for (uint32_t i = 0; i < g_kmod_module_count && i < MOD_MAX; i++) {
        kmod_module_t *c = &g_kmod_modules[i];
        uint16_t st = state_get(c);
        if (st == KMOD_STATE_EMPTY || st == KMOD_STATE_REMOVED)
            continue;
        for (uint32_t k = 0; k < c->n_bindings; k++) {
            kmod_binding_t *b = &c->bindings[k];
            if (b->provider != pidx || b->dep_edge_only)
                continue;
            if (b->import_index >= c->n_imports)
                continue;
            const kmod_import_t *imp = &c->imports[b->import_index];
            uint32_t eidx = 0;
            int32_t rc = find_export_in(new_exports, n_new, imp, &eidx);
            if (rc != KMOD_OK) {
                kmod_log("[kmod] rebind %s: consumer %s import #%u rc=%d (%s)\n",
                         provider->name, c->name, (unsigned)b->import_index,
                         (int)rc, KmodElfErrorString(rc));
                reg_unlock(lf);
                return rc;
            }
        }
    }
    /* 阶段 2：应用新地址（预校验后不会失败；失败时用 old_exports 全量恢复） */
    for (uint32_t i = 0; i < g_kmod_module_count && i < MOD_MAX; i++) {
        kmod_module_t *c = &g_kmod_modules[i];
        uint16_t st = state_get(c);
        if (st == KMOD_STATE_EMPTY || st == KMOD_STATE_REMOVED)
            continue;
        for (uint32_t k = 0; k < c->n_bindings; k++) {
            kmod_binding_t *b = &c->bindings[k];
            if (b->provider != pidx || b->dep_edge_only)
                continue;
            if (b->import_index >= c->n_imports)
                continue;
            kmod_import_t *imp = &c->imports[b->import_index];
            uint32_t eidx = 0;
            int32_t rc = find_export_in(new_exports, n_new, imp, &eidx);
            if (rc != KMOD_OK) {
                /* 理论上不会发生：按旧导出表恢复已改写的槽后返回 */
                for (uint32_t x = 0; x < g_kmod_module_count && x < MOD_MAX; x++) {
                    kmod_module_t *r = &g_kmod_modules[x];
                    if (state_get(r) == KMOD_STATE_EMPTY ||
                        state_get(r) == KMOD_STATE_REMOVED)
                        continue;
                    for (uint32_t y = 0; y < r->n_bindings; y++) {
                        kmod_binding_t *rb = &r->bindings[y];
                        if (rb->provider != pidx || rb->dep_edge_only ||
                            rb->import_index >= r->n_imports)
                            continue;
                        uint32_t oi = 0;
                        if (find_export_in(old_exports, n_old,
                                           &r->imports[rb->import_index], &oi) ==
                                KMOD_OK &&
                            r->imports[rb->import_index].slot) {
                            rb->resolved = old_exports[oi].addr;
                            rb->provider_export_index = oi;
                            *(r->imports[rb->import_index].slot) = old_exports[oi].addr;
                        }
                    }
                }
                reg_unlock(lf);
                return rc;
            }
            b->resolved = new_exports[eidx].addr;
            b->provider_export_index = eidx;
            if (imp->slot)
                *(imp->slot) = new_exports[eidx].addr;
        }
    }
    reg_unlock(lf);
    return KMOD_OK;
}

uint64_t KmodExportsHash(const kmod_export_t *exports, uint32_t n)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a 64 offset basis */
    for (uint32_t i = 0; i < n; i++) {
        const kmod_export_t *e = &exports[i];
        const char *s = e->symbol;
        if (s) {
            while (*s) {
                h ^= (uint64_t)(uint8_t)*s++;
                h *= 1099511628211ull;
            }
        }
        h ^= 0xFFull;
        h *= 1099511628211ull;
        s = e->contract;
        if (s) {
            while (*s) {
                h ^= (uint64_t)(uint8_t)*s++;
                h *= 1099511628211ull;
            }
        }
        uint64_t meta = ((uint64_t)e->size << 32) ^ ((uint64_t)e->abi_version << 16) ^
                        ((uint64_t)e->kind << 8) ^ ((uint64_t)e->flags << 4) ^
                        (uint64_t)e->aux;
        for (uint32_t b = 0; b < 8u; b++) {
            h ^= (meta >> (b * 8u)) & 0xFFull;
            h *= 1099511628211ull;
        }
    }
    return h == 0ull ? 1ull : h;
}

/* ------------------------------------------------------------------ */
/* epoch                                                               */
/* ------------------------------------------------------------------ */

int32_t KmodEpochEnterInternal(const char *name)
{
    if (!name)
        return KMOD_ERR_INVAL;
    kmod_module_t *m = KmodModuleFind(name);
    if (!m)
        return KMOD_ERR_NOTFOUND;
    /* __atomic 双检：READY -> +1 -> 再检 READY；第二轮失败则回滚计数 */
    uint16_t st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (st != KMOD_STATE_READY)
        return KMOD_ERR_BUSY;
    __atomic_add_fetch(&m->epoch_active, 1u, __ATOMIC_ACQ_REL);
    st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (st != KMOD_STATE_READY) {
        __atomic_sub_fetch(&m->epoch_active, 1u, __ATOMIC_ACQ_REL);
        return KMOD_ERR_BUSY;
    }
    uint32_t active = epoch_get(m);
    if ((uint64_t)active > m->epoch_max_seen)
        m->epoch_max_seen = (uint64_t)active; /* 观测峰值（弱一致即可） */
    return KMOD_OK;
}

void KmodEpochExitInternal(const char *name)
{
    if (!name)
        return;
    kmod_module_t *m = KmodModuleFind(name);
    if (!m)
        return;
    uint32_t prev = __atomic_sub_fetch(&m->epoch_active, 1u, __ATOMIC_ACQ_REL);
    if (prev == 0xFFFFFFFFu) {
        /* 非法配对：计数已回绕，钳回 0 并告警（不 fail hard，避免调用方泄漏） */
        __atomic_store_n(&m->epoch_active, 0u, __ATOMIC_RELEASE);
        kmod_log("[kmod] epoch exit %s: unbalanced (clamped to 0)\n", name);
    }
}

/* ------------------------------------------------------------------ */
/* 加载事务                                                            */
/* ------------------------------------------------------------------ */

int32_t KmodGovLoad(const char *name, const char *version, uint32_t flags)
{
    if (!name || name[0] == '\0') {
        g_kmod_stats.loads_fail++;
        return KMOD_ERR_INVAL;
    }
    if (KmodCapMode() == KMOD_POLICY_DISABLED) {
        g_kmod_stats.reject_policy++;
        g_kmod_stats.loads_fail++;
        kmod_log("[kmod] load %s: policy DISABLED\n", name);
        return KMOD_ERR_POLICY;
    }
    const char *want_ver = (version && version[0]) ? version : nullptr;
    int32_t target_pool = KmodPoolFind(name, want_ver);
    if (target_pool < 0) {
        g_kmod_stats.loads_fail++;
        note_reject(target_pool);
        kmod_log("[kmod] load %s: not in pool (rc=%d)\n", name, (int)target_pool);
        return target_pool;
    }

    {
        uint64_t lf = reg_lock();
        kmod_module_t *already = module_find_locked(name);
        reg_unlock(lf);
        if (already) {
            g_kmod_stats.loads_fail++;
            kmod_log("[kmod] load %s: already loaded\n", name);
            return KMOD_ERR_EXISTS;
        }
    }

    uint32_t token = ++g_tx_seq;
    if (token == 0u)
        token = ++g_tx_seq;

    int32_t staged_pool[MOD_MAX];
    int32_t staged_prep[MOD_MAX];
    uint32_t n_staged = 0u;
    char cycle_path[192];
    cycle_path[0] = '\0';
    const char **deps_buf = nullptr;
    uint64_t deps_bytes = 0u;
    int32_t rc = KMOD_OK;

    /* ---- prepare：BFS 依赖闭包，逐镜像重定位+清单/能力/策略校验 ---- */
    rc = gov_stage_add(token, flags, target_pool, staged_pool, staged_prep,
                       &n_staged);
    uint32_t cursor = 0u;
    while (rc == KMOD_OK && cursor < n_staged) {
        uint32_t k = cursor++;
        const kmod_elf_image_t *img = &g_prep[staged_prep[k]].img;
        const kmod_manifest_t *man = img->manifest;
        if (!man) {
            rc = KMOD_ERR_IMAGE;
            break;
        }
        for (uint32_t i = 0; i < img->n_imports; i++) {
            const kmod_import_t *imp = &img->imports[i];
            const char *provider = imp->provider;
            if (!provider || provider[0] == '\0') {
                rc = KMOD_ERR_FORMAT;
                break;
            }
            if (str_eq(provider, "kernel"))
                continue;
            if (imp->flags & KMOD_IMP_WEAK)
                continue; /* 弱依赖不强制拉起 */
            bool loaded = false;
            bool loading = false;
            uint64_t lf = reg_lock();
            kmod_module_t *prov = module_find_locked(provider);
            if (prov) {
                uint16_t pst = state_get(prov);
                loaded = (pst == KMOD_STATE_READY);
                loading = !loaded;
            }
            reg_unlock(lf);
            if (loading) {
                rc = KMOD_ERR_BUSY;
                break;
            }
            if (loaded)
                continue; /* 复用已加载 provider（commit 时绑定并加 refcount） */
            int32_t pp = KmodPoolFind(provider, nullptr);
            if (pp < 0) {
                if (imp->flags & KMOD_IMP_OPTIONAL)
                    continue; /* 可选缺失：槽保持 NULL */
                rc = (man->flags & KMOD_MF_NO_AUTODEP) ? KMOD_ERR_UNRESOLVED
                                                       : KMOD_ERR_UNRESOLVED;
                break;
            }
            rc = gov_stage_add(token, flags, pp, staged_pool, staged_prep,
                               &n_staged);
            if (rc != KMOD_OK)
                break;
        }
        if (rc != KMOD_OK)
            break;
    }

    /* ---- 拓扑排序（provider 先序；环 -> reject_cycle 且零残留） ---- */
    if (rc == KMOD_OK) {
        kmod_topo_node_t nodes[MOD_MAX];
        mem_zero(nodes, sizeof(nodes));
        uint32_t total_terms = 0u;
        for (uint32_t k = 0; k < n_staged; k++) {
            const kmod_elf_image_t *img = &g_prep[staged_prep[k]].img;
            nodes[k].name = img->manifest ? img->manifest->name : nullptr;
            if (!nodes[k].name) {
                rc = KMOD_ERR_IMAGE;
                break;
            }
            total_terms += img->n_imports;
        }
        if (rc == KMOD_OK && total_terms > 0u) {
            deps_bytes = (uint64_t)total_terms * (uint64_t)sizeof(const char *);
            deps_buf = (const char **)kmod_alloc(deps_bytes);
            if (!deps_buf) {
                rc = KMOD_ERR_NOMEM;
            } else {
                uint32_t off = 0u;
                for (uint32_t k = 0; k < n_staged; k++) {
                    const kmod_elf_image_t *img = &g_prep[staged_prep[k]].img;
                    nodes[k].deps = (const char *const *)&deps_buf[off];
                    nodes[k].n_deps = 0u;
                    for (uint32_t i = 0; i < img->n_imports; i++) {
                        const char *provider = img->imports[i].provider;
                        if (!provider || provider[0] == '\0' ||
                            str_eq(provider, "kernel"))
                            continue;
                        bool dup = false;
                        for (uint32_t d = 0; d < nodes[k].n_deps; d++) {
                            if (str_eq(nodes[k].deps[d], provider)) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            deps_buf[off++] = provider;
                            nodes[k].n_deps++;
                        }
                    }
                }
            }
        }
        if (rc == KMOD_OK) {
            uint32_t order[MOD_MAX];
            int32_t sorted = KmodTopoSort(nodes, n_staged, order, n_staged,
                                          cycle_path, sizeof(cycle_path));
            if (sorted != (int32_t)n_staged) {
                rc = (sorted < 0) ? sorted : KMOD_ERR_INVAL;
                if (rc == KMOD_ERR_CYCLE) {
                    kmod_log("[kmod] load %s: dependency cycle %s\n", name,
                             cycle_path);
                } else {
                    kmod_log("[kmod] load %s: topo rc=%d (%s)\n", name, (int)rc,
                             KmodElfErrorString(rc));
                }
            } else {
                /* ---- commit：按拓扑序 alloc -> bind -> apply -> init ---- */
                int32_t committed[MOD_MAX];
                uint8_t stages[MOD_MAX];
                uint32_t n_committed = 0u;
                rc = KMOD_OK;
                for (uint32_t oi = 0; oi < n_staged; oi++) {
                    uint32_t si = order[oi];
                    uint32_t lflags = flags;
                    if (si != 0u) /* BFS 起点 = 用户请求的模块 */
                        lflags |= KMOD_LOAD_DEPENDENCY;
                    int32_t mi = -1;
                    rc = KmodModuleAllocFromImage(staged_pool[si], lflags, &mi);
                    if (rc != KMOD_OK)
                        break;
                    committed[n_committed] = mi;
                    stages[n_committed] = ST_ALLOC;
                    n_committed++;
                    kmod_module_t *m = &g_kmod_modules[mi];
                    rc = KmodBindImports(m);
                    if (rc != KMOD_OK)
                        break;
                    stages[n_committed - 1u] = ST_BOUND;
                    rc = KmodApplyExports(m);
                    if (rc != KMOD_OK)
                        break;
                    stages[n_committed - 1u] = ST_APPLIED;
                    rc = KmodCallInit(m);
                    if (rc != KMOD_OK)
                        break;
                    stages[n_committed - 1u] = ST_INITED;
                }
                if (rc != KMOD_OK) {
                    kmod_log("[kmod] load %s: transaction failed rc=%d (%s), "
                             "rollback %u module(s)\n",
                             name, (int)rc, KmodElfErrorString(rc),
                             (unsigned)n_committed);
                    for (int32_t k = (int32_t)n_committed - 1; k >= 0; k--) {
                        kmod_module_t *m = &g_kmod_modules[committed[k]];
                        if (stages[k] >= ST_INITED) {
                            kmod_log("[kmod] rollback: exit %s\n", m->name);
                            KmodCallExit(m);
                        }
                        if (stages[k] >= ST_APPLIED) {
                            KmodRevertExports(m);
                        }
                        if (stages[k] >= ST_BOUND) {
                            KmodUnbindAll(m);
                        }
                        KmodModuleRelease(m);
                    }
                    g_kmod_stats.rollbacks++;
                } else {
                    /* 计数口径：成功加载的模块数（一事务自动拉起依赖时逐模块计），
                     * selftest/QEMU 门禁按 "loads_ok >= boot 模块数" 判定。 */
                    g_kmod_stats.loads_ok += (uint64_t)n_committed;
                    for (uint32_t k = 0; k < n_committed; k++) {
                        kmod_module_t *m = &g_kmod_modules[committed[k]];
                        kmod_log("[kmod] loaded %s@%s slot#%d gen=%u "
                                 "imports=%u exports=%u (tx=%u/%u)\n",
                                 m->name, m->version, (int)committed[k],
                                 (unsigned)m->generation, (unsigned)m->n_imports,
                                 (unsigned)m->n_exports, (unsigned)token,
                                 (unsigned)n_committed);
                    }
                }
            }
        }
    }

    if (deps_buf)
        kmod_free(deps_buf, deps_bytes);
    prep_cleanup_token(token);
    /* 统一失败计账：准备/拓扑/提交任一阶段失败都记一次 loads_fail + 对应拒绝桶
     * （早期 return 的 INVAL/POLICY/NOTFOUND/EXISTS 分支各自已计账）。 */
    if (rc != KMOD_OK) {
        g_kmod_stats.loads_fail++;
        note_reject(rc);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* 卸载事务                                                            */
/* ------------------------------------------------------------------ */

int32_t KmodGovUnload(const char *name, uint32_t flags)
{
    (void)flags;
    if (!name || name[0] == '\0') {
        g_kmod_stats.unloads_fail++;
        return KMOD_ERR_INVAL;
    }
    uint64_t lf = reg_lock();
    kmod_module_t *m = module_find_locked(name);
    if (!m) {
        reg_unlock(lf);
        g_kmod_stats.unloads_fail++;
        return KMOD_ERR_NOTFOUND;
    }
    uint16_t st = state_get(m);
    if (st != KMOD_STATE_READY) {
        reg_unlock(lf);
        g_kmod_stats.unloads_fail++;
        kmod_log("[kmod] unload %s: state=%s not READY\n", name,
                 KmodStateName(st));
        return (st == KMOD_STATE_LOADING || st == KMOD_STATE_QUIESCING ||
                st == KMOD_STATE_REPLACING || st == KMOD_STATE_UNLOADING)
                   ? KMOD_ERR_BUSY
                   : KMOD_ERR_STATE;
    }
    /* 内建组件（内核静态链接）不可卸载：热替换成功后 BUILTIN 已清除才走 .ko 规则 */
    if (m->internal_flags & KMOD_MODF_BUILTIN) {
        reg_unlock(lf);
        g_kmod_stats.reject_policy++;
        g_kmod_stats.unloads_fail++;
        kmod_log("[kmod] unload %s: REJECTED (builtin component, use hot "
                 "replace)\n",
                 name);
        return KMOD_ERR_POLICY;
    }
    if (m->n_consumers != 0u || m->refcount != 0u) {
        reg_unlock(lf);
        g_kmod_stats.reject_busy++;
        g_kmod_stats.unloads_fail++;
        kmod_log("[kmod] unload %s: BUSY (refcount=%u consumers=%u)\n", name,
                 (unsigned)m->refcount, (unsigned)m->n_consumers);
        return KMOD_ERR_BUSY;
    }
    if (!(m->flags & KMOD_MF_UNLOADABLE)) {
        reg_unlock(lf);
        g_kmod_stats.reject_policy++;
        g_kmod_stats.unloads_fail++;
        kmod_log("[kmod] unload %s: REJECTED by policy (manifest lacks "
                 "KMOD_MF_UNLOADABLE)\n",
                 name);
        return KMOD_ERR_POLICY;
    }
    state_set(m, KMOD_STATE_QUIESCING);
    reg_unlock(lf);

    int32_t rc = KmodQuiesceAndDrain(m, 0u);
    if (rc != KMOD_OK) {
        g_kmod_stats.unloads_fail++;
        kmod_log("[kmod] unload %s: quiesce/drain rc=%d (%s)\n", name, (int)rc,
                 KmodElfErrorString(rc));
        return rc;
    }

    /* 先摘掉 seat/syscall 发布（用当前 exports 语义），再 exit/unbind/释放 */
    KmodRevertExports(m);
    lf = reg_lock();
    state_set(m, KMOD_STATE_UNLOADING);
    reg_unlock(lf);
    KmodCallExit(m);

    kmod_elf_image_t img;
    mem_zero(&img, sizeof(img));
    uint32_t gen = 0u, refs = 0u, iflags = 0u;
    lf = reg_lock();
    gen = m->generation;
    refs = m->n_consumers;
    unbind_all_locked(m);
    bool released = module_release_locked(m, &img, &iflags);
    reg_unlock(lf);
    /* 能走到这里必然是 .ko 背书节点；BUILTIN 已在上面被 POLICY 拒绝 */
    if (released && (iflags & KMOD_MODF_BUILTIN) == 0u)
        KmodElfFreeImage(&img);

    g_kmod_stats.unloads_ok++;
    kmod_log("[kmod] unloaded %s (gen=%u consumers=%u) slot released\n", name,
             (unsigned)gen, (unsigned)refs);
    return KMOD_OK;
}

/* ------------------------------------------------------------------ */
/* 热替换事务                                                          */
/* ------------------------------------------------------------------ */

struct hot_ctx {
    kmod_module_t *m;
    int32_t midx;
    kmod_elf_image_t old_img;
    kmod_export_t *old_exports;
    uint32_t n_old;
    kmod_import_t *old_imports;
    uint32_t n_old_imports;
    char old_version[KMOD_VERSION_MAX];
    int32_t old_pool;
    uint32_t old_gen;
    uint32_t old_flags;
    uint32_t old_internal_flags;
    uint16_t old_tier;
    bool old_builtin;
    uint64_t old_caps_req;
    uint64_t old_caps_opt;
    uint64_t old_caps_granted;
    uint64_t old_caps_denied;
    uint64_t old_hash;
    kmod_elf_image_t new_img;
    bool new_owned;
    bool swapped;
    bool reverted_old;
    bool bound_new;
    bool rebound;
    bool applied_new;
    int32_t consumers[CONSUMER_MAX];
    uint32_t n_consumers;
    uint32_t n_quiesced;
};

int32_t hot_rollback(hot_ctx *c, int32_t rc_in)
{
    kmod_module_t *m = c->m;
    /* 1) 摘掉新版本的 seat/syscall 发布（当前 m->exports 指向新版本） */
    if (c->applied_new)
        KmodRevertExports(m);
    /* 2) 清掉新导入绑定（若已建立） */
    if (c->bound_new) {
        uint64_t lf = reg_lock();
        unbind_all_locked(m);
        reg_unlock(lf);
    }
    /* 3) 恢复旧镜像/导出/版本/能力/指纹（state 置 REPLACING 便于重新 bind） */
    uint64_t lf = reg_lock();
    m->img = c->old_img;
    m->exports = c->old_exports;
    m->n_exports = c->n_old;
    m->imports = c->old_imports;
    m->n_imports = c->n_old_imports;
    str_copy(m->version, KMOD_VERSION_MAX, c->old_version);
    m->pool_index = c->old_pool;
    m->generation = c->old_gen;
    m->flags = c->old_flags;
    m->internal_flags = c->old_internal_flags; /* 内建组件回滚必须恢复 BUILTIN */
    m->tier = c->old_tier;
    m->caps_required = c->old_caps_req;
    m->caps_optional = c->old_caps_opt;
    m->caps_granted = c->old_caps_granted;
    m->caps_denied = c->old_caps_denied;
    m->gen_exports_hash = c->old_hash;
    state_set(m, KMOD_STATE_REPLACING);
    reg_unlock(lf);
    /* 4) 消费者导入槽回退到旧导出表 */
    if (c->rebound) {
        int32_t rb = KmodRebindConsumers(m, c->new_img.exports,
                                         c->new_img.n_exports, c->old_exports,
                                         c->n_old);
        if (rb != KMOD_OK)
            kmod_log("[kmod] hotrollback %s: consumer rebind rc=%d\n", m->name,
                     (int)rb);
    }
    /* 5) 恢复旧版本的导入绑定（旧绑定在切换前已 unbind） */
    if (c->reverted_old) {
        int32_t rb = KmodBindImports(m);
        if (rb != KMOD_OK)
            kmod_log("[kmod] hotrollback %s: rebind old imports rc=%d\n", m->name,
                     (int)rb);
        /* 6) 重新发布旧 seat/syscall（此时 m->exports 已是旧版本） */
        int32_t ra = KmodApplyExports(m);
        if (ra != KMOD_OK)
            kmod_log("[kmod] hotrollback %s: reapply old exports rc=%d\n", m->name,
                     (int)ra);
    }
    /* 7) 恢复 READY（provider + 被 quiesce 的消费者） */
    lf = reg_lock();
    state_set(m, KMOD_STATE_READY);
    for (uint32_t i = 0; i < c->n_quiesced; i++) {
        kmod_module_t *cm = &g_kmod_modules[c->consumers[i]];
        if (state_get(cm) == KMOD_STATE_QUIESCING)
            state_set(cm, KMOD_STATE_READY);
    }
    reg_unlock(lf);
    /* 8) 释放新镜像 */
    if (c->new_owned) {
        KmodElfFreeImage(&c->new_img);
        c->new_owned = false;
    }
    g_kmod_stats.hotswaps_fail++;
    g_kmod_stats.rollbacks++;
    kmod_log("[kmod] hot replace %s: FAILED rc=%d (%s), rolled back to %s\n",
             m->name, (int)rc_in, KmodElfErrorString(rc_in), m->version);
    return rc_in;
}

int32_t KmodGovHotReplace(const char *name, const char *new_version,
                          uint32_t flags)
{
    if (!name || name[0] == '\0') {
        g_kmod_stats.hotswaps_fail++;
        return KMOD_ERR_INVAL;
    }
    if (KmodCapMode() == KMOD_POLICY_DISABLED) {
        g_kmod_stats.reject_policy++;
        g_kmod_stats.hotswaps_fail++;
        return KMOD_ERR_POLICY;
    }

    hot_ctx c;
    mem_zero(&c, sizeof(c));
    char want_ver[KMOD_VERSION_MAX];
    want_ver[0] = '\0';
    if (new_version)
        str_copy(want_ver, KMOD_VERSION_MAX, new_version);

    uint64_t lf = reg_lock();
    kmod_module_t *m = module_find_locked(name);
    if (!m) {
        reg_unlock(lf);
        g_kmod_stats.hotswaps_fail++;
        return KMOD_ERR_NOTFOUND;
    }
    uint16_t st = state_get(m);
    if (st != KMOD_STATE_READY) {
        reg_unlock(lf);
        g_kmod_stats.hotswaps_fail++;
        return (st == KMOD_STATE_LOADING || st == KMOD_STATE_QUIESCING ||
                st == KMOD_STATE_REPLACING || st == KMOD_STATE_UNLOADING)
                   ? KMOD_ERR_BUSY
                   : KMOD_ERR_STATE;
    }
    c.m = m;
    c.midx = module_index_of(m);
    c.old_img = m->img;
    c.old_exports = m->exports;
    c.n_old = m->n_exports;
    c.old_imports = m->imports;
    c.n_old_imports = m->n_imports;
    str_copy(c.old_version, KMOD_VERSION_MAX, m->version);
    c.old_pool = m->pool_index;
    c.old_gen = m->generation;
    c.old_caps_req = m->caps_required;
    c.old_caps_opt = m->caps_optional;
    c.old_caps_granted = m->caps_granted;
    c.old_caps_denied = m->caps_denied;
    c.old_hash = m->gen_exports_hash;
    c.old_flags = m->flags;
    c.old_internal_flags = m->internal_flags;
    c.old_tier = m->tier;
    c.old_builtin = (m->internal_flags & KMOD_MODF_BUILTIN) != 0u;
    c.n_consumers = m->n_consumers;
    for (uint32_t i = 0; i < c.n_consumers && i < CONSUMER_MAX; i++)
        c.consumers[i] = (int32_t)m->consumers[i];
    /* Tier0 治理断言：.ko 节点需显式 HOTREPLACE flag + HOT_REPLACE 能力；
     * 内建组件是 Tier0 构建产物，允许由 .ko 热替换（替换后转为 .ko 背书）。 */
    bool hot_flag = (m->flags & KMOD_MF_HOTREPLACE) != 0u;
    bool hot_cap = (m->caps_granted & KMOD_CAP_HOT_REPLACE) != 0u;
    if (!c.old_builtin && (!hot_flag || !hot_cap)) {
        reg_unlock(lf);
        g_kmod_stats.hotswaps_fail++;
        g_kmod_stats.reject_cap++;
        kmod_log("[kmod] hot replace %s: REJECTED (flags=0x%x caps=0x%llx; "
                 "need MF_HOTREPLACE + CAP_HOT_REPLACE)\n",
                 name, (unsigned)m->flags,
                 (unsigned long long)m->caps_granted);
        return KMOD_ERR_CAP;
    }
    if (c.old_builtin)
        kmod_log("[kmod] hot replace %s: builtin component -> .ko takeover\n",
                 name);
    reg_unlock(lf);

    /* 1) 选镜像（版本为空 = 池内最高版本；与当前版本相同视为 no-op） */
    int32_t pool = KmodPoolFind(name, want_ver[0] ? want_ver : nullptr);
    if (pool < 0) {
        g_kmod_stats.hotswaps_fail++;
        note_reject(pool);
        kmod_log("[kmod] hot replace %s: target version '%s' not in pool rc=%d\n",
                 name, want_ver[0] ? want_ver : "(highest)", (int)pool);
        return pool;
    }
    if (pool == c.old_pool) {
        g_kmod_stats.hotswaps_ok++;
        kmod_log("[kmod] hot replace %s: already at '%s' (no-op)\n", name,
                 c.old_version);
        return KMOD_OK;
    }

    /* 2) prepare 新镜像：重定位 + 清单身份/能力/策略校验 */
    {
        kmod_image_t pi = g_kmod_images[pool];
        if (!pi.data || pi.size == 0u) {
            g_kmod_stats.hotswaps_fail++;
            return KMOD_ERR_IMAGE;
        }
        const kmod_kernel_export_t *kexp = nullptr;
        uint32_t nkexp = 0;
        KmodKernelExports(&kexp, &nkexp);
        char err[128];
        err[0] = '\0';
        int32_t rc = KmodElfLoadImage(pi.data, pi.size, kexp, nkexp, &c.new_img,
                                      err, sizeof(err));
        if (rc != KMOD_OK) {
            g_kmod_stats.hotswaps_fail++;
            note_reject(rc);
            kmod_log("[kmod] hot replace %s: new image load rc=%d (%s)\n", name,
                     (int)rc, KmodElfErrorString(rc));
            return rc;
        }
        c.new_owned = true;
        const kmod_manifest_t *nm = c.new_img.manifest;
        if (!nm || !str_eq(nm->name, name)) {
            int32_t brc = KMOD_ERR_NAME;
            g_kmod_stats.hotswaps_fail++;
            note_reject(brc);
            KmodElfFreeImage(&c.new_img);
            c.new_owned = false;
            return brc;
        }
        if (want_ver[0] && !str_eq(nm->version, want_ver)) {
            g_kmod_stats.hotswaps_fail++;
            note_reject(KMOD_ERR_VERSION);
            KmodElfFreeImage(&c.new_img);
            c.new_owned = false;
            return KMOD_ERR_VERSION;
        }
        rc = KmodCheckLoadable(nm, flags);
        if (rc != KMOD_OK) {
            g_kmod_stats.hotswaps_fail++;
            note_reject(rc);
            KmodElfFreeImage(&c.new_img);
            c.new_owned = false;
            return rc;
        }
    }

    /* 3) quiesce provider + 直接消费者（每模块 budget 内 epoch 排空） */
    {
        uint32_t q = 0u;
        for (; q < c.n_consumers; q++) {
            kmod_module_t *cm = &g_kmod_modules[c.consumers[q]];
            lf = reg_lock();
            uint16_t cst = state_get(cm);
            reg_unlock(lf);
            if (cst != KMOD_STATE_READY)
                continue; /* 已经不在服务：无需 quiesce */
            int32_t qrc = KmodQuiesceAndDrain(cm, 0u);
            if (qrc != KMOD_OK) {
                int32_t brc = qrc;
                g_kmod_stats.hotswaps_fail++;
                g_kmod_stats.rollbacks++;
                for (uint32_t x = 0; x < q; x++) {
                    kmod_module_t *rm = &g_kmod_modules[c.consumers[x]];
                    lf = reg_lock();
                    if (state_get(rm) == KMOD_STATE_QUIESCING)
                        state_set(rm, KMOD_STATE_READY);
                    reg_unlock(lf);
                }
                KmodElfFreeImage(&c.new_img);
                c.new_owned = false;
                kmod_log("[kmod] hot replace %s: consumer %s drain rc=%d\n", name,
                         cm->name, (int)brc);
                return brc;
            }
            c.n_quiesced = q + 1u;
        }
        int32_t qrc = KmodQuiesceAndDrain(m, 0u);
        if (qrc != KMOD_OK) {
            g_kmod_stats.hotswaps_fail++;
            g_kmod_stats.rollbacks++;
            for (uint32_t x = 0; x < c.n_quiesced; x++) {
                kmod_module_t *rm = &g_kmod_modules[c.consumers[x]];
                lf = reg_lock();
                if (state_get(rm) == KMOD_STATE_QUIESCING)
                    state_set(rm, KMOD_STATE_READY);
                reg_unlock(lf);
            }
            KmodElfFreeImage(&c.new_img);
            c.new_owned = false;
            return qrc;
        }
    }

    /* 4) state=REPLACING；摘旧 seat/syscall；解除旧导入边（保留旧镜像导入槽：
     *    成功路径要在切换后调用旧版本 exit/fini，回调可能经导入槽调用内核符号） */
    lf = reg_lock();
    state_set(m, KMOD_STATE_REPLACING);
    reg_unlock(lf);
    KmodRevertExports(m); /* m->exports 此时仍指向旧版本 */
    c.reverted_old = true;
    {
        lf = reg_lock();
        unbind_edges_locked(m, false);
        reg_unlock(lf);
    }

    /* 5) 原子切换镜像/导出/版本/generation/能力 */
    {
        uint64_t granted = 0ull, denied = 0ull;
        const kmod_manifest_t *nm = c.new_img.manifest;
        int32_t grc = KmodCapGrant(nm->name, nm->required_caps,
                                   nm->optional_caps, &granted, &denied);
        lf = reg_lock();
        m->img = c.new_img;
        m->exports = m->img.exports;
        m->n_exports = m->img.n_exports;
        m->imports = m->img.imports;
        m->n_imports = m->img.n_imports;
        str_copy(m->version, KMOD_VERSION_MAX, nm->version);
        m->pool_index = pool;
        m->generation = c.old_gen + 1u;
        m->flags = nm->flags;
        m->tier = nm->tier;
        m->internal_flags = 0u; /* 节点从此由 .ko 镜像背书（清 BUILTIN） */
        m->caps_required = nm->required_caps;
        m->caps_optional = nm->optional_caps;
        if (grc == KMOD_OK) {
            m->caps_granted = granted;
            m->caps_denied = denied;
        }
        m->gen_exports_hash = KmodExportsHash(m->exports, m->n_exports);
        reg_unlock(lf);
        c.swapped = true;
    }

    /* 6) 绑定新导入 -> 重绑定消费者 -> 重新发布 seat/syscall -> 新 init */
    {
        int32_t rc = KmodBindImports(m);
        if (rc != KMOD_OK) {
            note_reject(rc);
            return hot_rollback(&c, rc);
        }
        c.bound_new = true;
        rc = KmodRebindConsumers(m, c.old_exports, c.n_old, m->exports,
                                 m->n_exports);
        if (rc != KMOD_OK) {
            note_reject(rc);
            return hot_rollback(&c, rc);
        }
        c.rebound = true;
        rc = KmodApplyExports(m);
        if (rc != KMOD_OK) {
            note_reject(rc);
            return hot_rollback(&c, rc);
        }
        c.applied_new = true;
        rc = KmodCallInit(m);
        if (rc != KMOD_OK) {
            note_reject(rc);
            return hot_rollback(&c, rc);
        }
    }

    /* 7) 成功：provider READY、消费者 READY、旧版本 exit/fini 后释放 */
    {
        lf = reg_lock();
        state_set(m, KMOD_STATE_READY);
        for (uint32_t i = 0; i < c.n_quiesced; i++) {
            kmod_module_t *cm = &g_kmod_modules[c.consumers[i]];
            if (state_get(cm) == KMOD_STATE_QUIESCING)
                state_set(cm, KMOD_STATE_READY);
        }
        uint32_t gen = m->generation;
        uint64_t hash = m->gen_exports_hash;
        reg_unlock(lf);

        call_exit_image(&c.old_img); /* 旧版本 exit_hook -> fini_array 逆序 */
        if (!c.old_builtin) {
            KmodElfFreeImage(&c.old_img); /* 旧 .ko 镜像由 governance 释放 */
        } else {
            /* 旧节点是内核静态链接组件：exit 已调用，static manifest/段不释放 */
            mem_zero(&c.old_img, sizeof(c.old_img));
        }
        g_kmod_stats.hotswaps_ok++;
        kmod_log("[kmod] hot replace %s: %s -> %s OK gen=%u exports_hash=0x%llx\n",
                 name, c.old_version, m->version, (unsigned)gen,
                 (unsigned long long)hash);
    }
    return KMOD_OK;
}

} /* extern "C" */
