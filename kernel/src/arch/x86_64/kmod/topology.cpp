//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * topology.cpp —— kmod 纯函数层：依赖拓扑排序 / 环检测 / 版本 / 命名空间
 * ----------------------------------------------------------------------------
 * 本文件不依赖任何内核设施（不分配、不打印、不加锁），宿主测试直接编译同一份
 * 代码（tests/kmod/topology_test.cpp）。失败原因通过返回码表达：
 *   KMOD_ERR_INVAL   参数非法（空节点名/重名/超容量/out_order 容量不足）
 *   KMOD_ERR_CYCLE   依赖成环，cycle_path 写入 "a->b->a" 形式路径
 *   KMOD_ERR_VERSION 版本字符串非法 / 区间非法
 *   KMOD_ERR_NAME    命名空间非法
 *
 * 语义约定（与 kmod_internal.h 注释一致，另作三点补充）：
 *   1) deps 中的 "kernel" 以及不属于本次节点集合的名字视为"已满足的外部依赖"
 *      （例如已在运行时注册表中的 provider），不参与入度、也不报缺失错误。
 *      缺失依赖的判责在 governance 的 prepare 阶段（那里能查镜像池）。
 *   2) 同一节点 deps 中的重复 provider 只计一条边；自环按普通环处理。
 *   3) 节点名集合内重名无法构成唯一图，直接 KMOD_ERR_INVAL 拒绝。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

extern "C" {
}

namespace {

/* 本函数是通用图算法：容量只受栈上数组限制，与 KMOD_MAX_MODULES 无关。
 * 64 个模块的治理上限下 128 留有 2 倍余量；宿主测试可自由构造小图。 */
constexpr uint32_t TOPO_MAX_NODES = 128u;

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

inline bool is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

inline bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

inline bool is_name_char(char c)
{
    return is_lower(c) || is_digit(c) || c == '_';
}

/* 命名空间校验：segment('.'segment)+，每段 [a-z][a-z0-9_]* */
int32_t validate_namespace(const char *s)
{
    if (!s || s[0] == '\0')
        return KMOD_ERR_NAME;
    uint64_t len = str_len(s);
    if (len > (uint64_t)KMOD_NAME_MAX - 1u)
        return KMOD_ERR_NAME;

    uint32_t segments = 0;
    uint64_t i = 0;
    while (i < len) {
        /* 每段首字符必须是小写字母 */
        if (!is_lower(s[i]))
            return KMOD_ERR_NAME;
        i++;
        while (i < len && s[i] != '.') {
            if (!is_name_char(s[i]))
                return KMOD_ERR_NAME;
            i++;
        }
        segments++;
        if (i < len) {
            /* 跳过 '.'；不允许空段/尾点 */
            i++;
            if (i >= len)
                return KMOD_ERR_NAME;
        }
    }
    /* 至少两段：vendor.area（模块名/契约名都不允许单段裸名） */
    if (segments < 2u)
        return KMOD_ERR_NAME;
    return KMOD_OK;
}

bool starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
        return false;
    while (*prefix) {
        if (*s != *prefix)
            return false;
        s++;
        prefix++;
    }
    return true;
}

/* 解析单个 "x.y.z" 段；成功返回 true 并前移 *p。 */
bool parse_seg(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (!is_digit(*s))
        return false;
    uint64_t v = 0;
    while (is_digit(*s)) {
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > 65535ull) /* 头文件：每段 <= 65535 */
            return false;
        s++;
    }
    *p = s;
    *out = (uint32_t)v;
    return true;
}

int32_t version_parse_impl(const char *s, uint32_t *maj, uint32_t *min,
                           uint32_t *pat)
{
    if (!s || s[0] == '\0')
        return KMOD_ERR_VERSION;

    uint32_t v[3] = { 0u, 0u, 0u };
    const char *p = s;
    for (uint32_t i = 0; i < 3u; i++) {
        if (!parse_seg(&p, &v[i]))
            return KMOD_ERR_VERSION;
        if (*p == '\0')
            break;
        if (*p != '.')
            return KMOD_ERR_VERSION;
        p++;
        if (*p == '\0') /* 尾点 */
            return KMOD_ERR_VERSION;
        if (i == 2u) /* 第 4 段 */
            return KMOD_ERR_VERSION;
    }
    if (*p != '\0')
        return KMOD_ERR_VERSION;
    if (maj)
        *maj = v[0];
    if (min)
        *min = v[1];
    if (pat)
        *pat = v[2];
    return KMOD_OK;
}

/* 节点 u 是否依赖名为 dep 的节点（只看一次即可，拓扑循环里 O(n^3) 但 n<=128）。 */
bool node_depends_on(const kmod_topo_node_t *nodes, uint32_t u, const char *dep)
{
    const kmod_topo_node_t *node = &nodes[u];
    for (uint32_t k = 0; k < node->n_deps; k++) {
        if (str_eq(node->deps ? node->deps[k] : nullptr, dep))
            return true;
    }
    return false;
}

int32_t find_node(const kmod_topo_node_t *nodes, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (str_eq(nodes[i].name, name))
            return (int32_t)i;
    }
    return -1;
}

/* 在残余图（未被 Kahn 输出的节点）中找一个环，写入 "a->b->a"。
 * 依赖边方向：u 依赖 v 即 u->v。返回 true 表示写入成功（cycle_cap>0 时总以
 * NUL 结尾；容量不足截断但不溢出）。 */
bool build_cycle_path(const kmod_topo_node_t *nodes, uint32_t n,
                      const bool *placed, char *cycle_path, uint64_t cycle_cap)
{
    int32_t stack[TOPO_MAX_NODES];
    uint32_t cursor[TOPO_MAX_NODES]; /* 当前节点下一个待展开的 dep 下标 */
    uint8_t color[TOPO_MAX_NODES];   /* 0=未访问 1=在栈上 2=完成 */
    for (uint32_t i = 0; i < n; i++) {
        stack[i] = -1;
        cursor[i] = 0u;
        color[i] = 0u;
    }

    for (uint32_t root = 0; root < n; root++) {
        if (placed[root] || color[root] != 0u)
            continue;
        uint32_t top = 0;
        stack[0] = (int32_t)root;
        color[root] = 1u;
        cursor[root] = 0u;

        while (top != (uint32_t)-1) {
            uint32_t u = (uint32_t)stack[top];
            const kmod_topo_node_t *node = &nodes[u];
            bool advanced = false;
            while (cursor[u] < node->n_deps) {
                const char *dep = node->deps ? node->deps[cursor[u]] : nullptr;
                cursor[u]++;
                int32_t v = dep ? find_node(nodes, n, dep) : -1;
                if (v < 0 || placed[(uint32_t)v])
                    continue; /* 外部依赖/已输出，不成环 */
                uint32_t vv = (uint32_t)v;
                if (color[vv] == 0u) {
                    top++;
                    stack[top] = v;
                    color[vv] = 1u;
                    cursor[vv] = 0u;
                    advanced = true;
                    break;
                }
                if (color[vv] == 1u) {
                    /* vv 在栈上：从 vv 的位置到 u 构成环 vv->...->u->vv */
                    uint32_t start = 0;
                    for (uint32_t k = 0; k <= top; k++) {
                        if ((uint32_t)stack[k] == vv) {
                            start = k;
                            break;
                        }
                    }
                    if (cycle_path && cycle_cap > 0u) {
                        uint64_t w = 0;
                        bool ovf = false;
                        for (uint32_t k = start; k <= top; k++) {
                            const char *nm = nodes[(uint32_t)stack[k]].name;
                            if (nm) {
                                for (uint64_t c = 0; nm[c]; c++) {
                                    if (w + 1u >= cycle_cap) { ovf = true; break; }
                                    cycle_path[w++] = nm[c];
                                }
                            }
                            if (ovf || w + 2u >= cycle_cap)
                                { ovf = true; break; }
                            cycle_path[w++] = '-';
                            cycle_path[w++] = '>';
                        }
                        if (!ovf) {
                            const char *nm = nodes[vv].name;
                            if (nm) {
                                for (uint64_t c = 0; nm[c] && w + 1u < cycle_cap; c++)
                                    cycle_path[w++] = nm[c];
                            }
                        }
                        cycle_path[w] = '\0';
                    }
                    return true;
                }
            }
            if (advanced)
                continue;
            color[u] = 2u;
            if (top == 0)
                break;
            top--;
        }
    }
    if (cycle_path && cycle_cap > 0u)
        cycle_path[0] = '\0';
    return false;
}

} /* namespace */

extern "C" {

int32_t KmodTopoSort(const kmod_topo_node_t *nodes, uint32_t n_nodes,
                     uint32_t *out_order, uint32_t out_cap,
                     char *cycle_path, uint64_t cycle_cap)
{
    if (cycle_path && cycle_cap > 0u)
        cycle_path[0] = '\0';
    if (n_nodes == 0u)
        return 0; /* 空图合法 */
    if (!nodes || !out_order)
        return KMOD_ERR_INVAL;
    if (n_nodes > TOPO_MAX_NODES)
        return KMOD_ERR_INVAL;

    /* 1) 节点名合法性 + 重名检测 */
    for (uint32_t i = 0; i < n_nodes; i++) {
        const char *name = nodes[i].name;
        if (!name || name[0] == '\0')
            return KMOD_ERR_INVAL;
        if (nodes[i].n_deps > 0u && !nodes[i].deps)
            return KMOD_ERR_INVAL;
        for (uint32_t j = 0; j < i; j++) {
            if (str_eq(nodes[j].name, name))
                return KMOD_ERR_INVAL; /* 重名图不唯一 */
        }
    }

    /* 2) 入度：只统计"本次节点集合内、去重后"的 provider 边 */
    uint32_t indeg[TOPO_MAX_NODES];
    bool placed[TOPO_MAX_NODES];
    for (uint32_t i = 0; i < n_nodes; i++) {
        indeg[i] = 0u;
        placed[i] = false;
        const kmod_topo_node_t *node = &nodes[i];
        for (uint32_t k = 0; k < node->n_deps; k++) {
            const char *dep = node->deps[k];
            if (!dep || !dep[0])
                return KMOD_ERR_INVAL;
            if (str_eq(dep, "kernel"))
                continue; /* Tier0 恒已就绪 */
            int32_t p = find_node(nodes, n_nodes, dep);
            if (p < 0)
                continue; /* 外部/已加载 provider：视为已满足 */
            bool dup = false;
            for (uint32_t m = 0; m < k; m++) {
                if (str_eq(node->deps[m], dep)) { dup = true; break; }
            }
            if (!dup)
                indeg[i]++;
        }
    }

    /* 3) Kahn：每轮输出所有入度 0 节点（按下标稳定），再去掉它们对后继的边 */
    uint32_t order[TOPO_MAX_NODES];
    uint32_t count = 0u;
    while (count < n_nodes) {
        bool progressed = false;
        for (uint32_t u = 0; u < n_nodes; u++) {
            if (placed[u] || indeg[u] != 0u)
                continue;
            placed[u] = true;
            order[count++] = u;
            progressed = true;
            const char *uname = nodes[u].name;
            for (uint32_t v = 0; v < n_nodes; v++) {
                if (placed[v] || indeg[v] == 0u)
                    continue;
                if (node_depends_on(nodes, v, uname) && indeg[v] > 0u)
                    indeg[v]--;
            }
        }
        if (!progressed) {
            /* 4) 环：残余节点里找一条路径写入 cycle_path */
            build_cycle_path(nodes, n_nodes, placed, cycle_path, cycle_cap);
            return KMOD_ERR_CYCLE;
        }
    }

    /* 5) 输出：最多 out_cap 条；返回实际写入条数（头文件语义：
     *    out_order[0..min(n,cap)-1] 为依赖先序）。 */
    uint32_t written = count < out_cap ? count : out_cap;
    for (uint32_t i = 0; i < written; i++)
        out_order[i] = order[i];
    return (int32_t)written;
}

int32_t KmodVersionParse(const char *s, uint32_t *maj, uint32_t *min,
                         uint32_t *pat)
{
    return version_parse_impl(s, maj, min, pat);
}

int32_t KmodVersionCompare(const char *a, const char *b)
{
    uint32_t am = 0, ai = 0, ap = 0;
    uint32_t bm = 0, bi = 0, bp = 0;
    int32_t ra = version_parse_impl(a, &am, &ai, &ap);
    int32_t rb = version_parse_impl(b, &bm, &bi, &bp);
    /* 非法版本视为最小：让调用方能用 <0/0/>0 做稳定排序 */
    if (ra != KMOD_OK && rb != KMOD_OK)
        return 0;
    if (ra != KMOD_OK)
        return -1;
    if (rb != KMOD_OK)
        return 1;
    if (am != bm)
        return am < bm ? -1 : 1;
    if (ai != bi)
        return ai < bi ? -1 : 1;
    if (ap != bp)
        return ap < bp ? -1 : 1;
    return 0;
}

int32_t KmodVersionInRange(uint32_t ver, uint32_t vmin, uint32_t vmax)
{
    if (vmin != 0u && vmax != 0u && vmin > vmax)
        return KMOD_ERR_VERSION; /* 区间本身非法 */
    if (vmin != 0u && ver < vmin)
        return KMOD_ERR_VERSION;
    if (vmax != 0u && ver > vmax)
        return KMOD_ERR_VERSION;
    return KMOD_OK;
}

int32_t KmodNameValidate(const char *name)
{
    /* .ko 模块名：纯命名空间语法 + 保留前缀禁止（kernel./core. 只属于 Tier0
     * 内建组件/内核镜像，外部模块不得冒名）。 */
    if (starts_with(name, "kernel.") || starts_with(name, "core."))
        return KMOD_ERR_NAME;
    return validate_namespace(name);
}

/* 内建组件名校验：允许 kernel./core. 保留前缀，其余命名空间规则与 .ko 相同。
 * 仅供 Tier0 内建组件注册路径（builtin.cpp/KmodElfValidateBuiltinManifest）使用；
 * .ko 加载路径仍必须走 KmodNameValidate 的严格版本。 */
int32_t KmodBuiltinNameValidate(const char *name)
{
    return validate_namespace(name);
}

bool KmodNameIsReserved(const char *name)
{
    if (!name)
        return false;
    return starts_with(name, "kernel.") || starts_with(name, "core.");
}

int32_t KmodContractValidate(const char *contract)
{
    /* 契约名与模块名规则相同，额外允许 kernel. 保留前缀（仅 Tier0 内核镜像
     * 使用；Tier/来源校验由 KmodCheckLoadable 与加载器负责）。 */
    return validate_namespace(contract);
}

int32_t KmodSegNameValidate(const char *seg, uint32_t max_len)
{
    if (!seg || seg[0] == '\0')
        return KMOD_ERR_INVAL;
    uint64_t len = str_len(seg);
    if (max_len != 0u && len > (uint64_t)max_len)
        return KMOD_ERR_INVAL;
    /* 本文件接受的段名字符集：ELF 常见安全子集；拒绝 '/' 与不可打印字符。 */
    for (uint64_t i = 0; i < len; i++) {
        char c = seg[i];
        bool ok;
        if (i == 0)
            ok = (c == '.' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '_');
        else
            ok = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$' ||
                  c == '-');
        if (!ok)
            return KMOD_ERR_INVAL;
    }
    return KMOD_OK;
}

} /* extern "C" */
