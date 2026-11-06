// tests/kmod/topology_test.cpp — kmod 拓扑/版本/命名纯函数宿主测试
// ----------------------------------------------------------------------------
// 覆盖 KmodTopoSort（随机 DAG 先序性质、环检测路径、自环、重名、外部依赖）、
// KmodVersionParse/Compare/InRange、KmodNameValidate/KmodContractValidate/
// KmodSegNameValidate。
//
// 随机种子固定 = 0x6b6d6f64（"kmod"），失败可重放。
// 语义约定（与 kernel/src/arch/x86_64/kmod/topology.cpp 文件头一致）：
//   * deps 里的 "kernel" 与不在本次节点集合内的名字视为"已满足的外部 provider"，
//     不参与入度、不报缺失错误（缺失依赖的判责在 governance 的 prepare 阶段）；
//   * 节点名集合内重名 → KMOD_ERR_INVAL；
//   * 环/自环 → KMOD_ERR_CYCLE 且 cycle_path 非空（"a->b->a" 形式）。
// ----------------------------------------------------------------------------
#include <kmod/kmod_internal.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_common.h"

namespace {

constexpr uint32_t kTopoCap = 256u;

// 一个随机 DAG：节点 i 命名为 n<i>，边只从低到高（j 依赖 i，i<j -> provider 先序）。
struct Dag {
    uint32_t n = 0;
    std::vector<std::string> names;
    std::vector<std::vector<const char *>> dep_names;
    std::vector<kmod_topo_node_t> nodes;
    // 记录生成边：node j 的 provider 下标集合
    std::vector<std::vector<uint32_t>> providers;
};

void build_dag(Dag *d, uint32_t n, KRng *rng, uint32_t edge_permille)
{
    d->n = n;
    d->names.clear();
    d->dep_names.clear();
    d->nodes.clear();
    d->providers.clear();
    d->names.resize(n);
    d->dep_names.resize(n);
    d->providers.resize(n);
    char buf[32];
    for (uint32_t i = 0; i < n; i++) {
        std::snprintf(buf, sizeof(buf), "n%u", i);
        d->names[i] = buf;
    }
    for (uint32_t j = 0; j < n; j++) {
        for (uint32_t i = 0; i < j; i++) {
            if (rng->below(1000) < edge_permille) {
                d->providers[j].push_back(i);
                d->dep_names[j].push_back(d->names[i].c_str());
            }
        }
    }
    d->nodes.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        d->nodes[i].name = d->names[i].c_str();
        d->nodes[i].deps = d->dep_names[i].empty() ? nullptr : d->dep_names[i].data();
        d->nodes[i].n_deps = (uint32_t)d->dep_names[i].size();
    }
}

int32_t sort_dag(const Dag &d, std::vector<uint32_t> *order, std::string *cyc)
{
    order->assign(kTopoCap, 0u);
    char path[512] = {};
    int32_t rc = KmodTopoSort(d.nodes.data(), d.n, order->data(),
                              (uint32_t)order->size(), path, sizeof(path));
    if (cyc)
        *cyc = path;
    return rc;
}

// 1) 随机 DAG：输出满足"每条边 provider 先序"，且每个节点恰好一次。
int32_t test_random_dag()
{
    KRng rng(KMOD_SEED_TOPO);
    for (uint32_t trial = 1; trial <= 24; trial++) {
        uint32_t n = 1u + rng.below(64u);
        Dag d;
        build_dag(&d, n, &rng, 120u);
        std::vector<uint32_t> order;
        std::string cyc;
        int32_t rc = sort_dag(d, &order, &cyc);
        KCHECK_MSG(rc == (int32_t)n,
                   "trial %u n=%u: topo returned %d, cycle='%s'", trial, n,
                   (int)rc, cyc.c_str());
        // 每个节点恰好出现一次
        std::vector<uint32_t> seen(n, 0);
        for (uint32_t k = 0; k < (uint32_t)rc; k++) {
            KCHECK_MSG(order[k] < n, "trial %u: out of range index %u", trial,
                       order[k]);
            seen[order[k]]++;
        }
        for (uint32_t i = 0; i < n; i++)
            KCHECK_MSG(seen[i] == 1u, "trial %u: node %u seen %u times",
                       trial, i, seen[i]);
        // 边先序
        std::vector<uint32_t> pos(n, 0);
        for (uint32_t k = 0; k < (uint32_t)rc; k++)
            pos[order[k]] = k;
        for (uint32_t j = 0; j < n; j++)
            for (uint32_t p : d.providers[j])
                KCHECK_MSG(pos[p] < pos[j],
                           "trial %u: provider n%u (#%u) after consumer n%u (#%u)",
                           trial, p, pos[p], j, pos[j]);
    }
    return 0;
}

// 2) 空图；3) 链式图 + out_cap 截断；4) deps 重复只算一条边
int32_t test_edge_cases()
{
    // 空图合法
    uint32_t sink[4] = {};
    KCHECK_EQ(KmodTopoSort(nullptr, 0, sink, 4, nullptr, 0), 0);

    // 链 a <- b <- c（c 依赖 b，b 依赖 a）
    kmod_topo_node_t nodes[3];
    const char *c_deps[] = {"b"};
    const char *b_deps[] = {"a"};
    nodes[0] = {"a", nullptr, 0};
    nodes[1] = {"b", b_deps, 1};
    nodes[2] = {"c", c_deps, 1};
    uint32_t order[3] = {0xff, 0xff, 0xff};
    char path[64] = {};
    int32_t rc = KmodTopoSort(nodes, 3, order, 3, path, sizeof(path));
    KCHECK_EQ(rc, 3);
    KCHECK_EQ(order[0], 0u);
    KCHECK_EQ(order[1], 1u);
    KCHECK_EQ(order[2], 2u);

    // out_cap 截断：只写 2 条并返回 2
    uint32_t cap2[2] = {0xff, 0xff};
    rc = KmodTopoSort(nodes, 3, cap2, 2, path, sizeof(path));
    KCHECK_EQ(rc, 2);
    KCHECK_EQ(cap2[0], 0u);
    KCHECK_EQ(cap2[1], 1u);

    // 重复 provider 只计一条边（仍可排序）
    const char *dup_deps[] = {"a", "a"};
    kmod_topo_node_t dup_nodes[2];
    dup_nodes[0] = {"a", nullptr, 0};
    dup_nodes[1] = {"b", dup_deps, 2};
    uint32_t o2[2] = {};
    rc = KmodTopoSort(dup_nodes, 2, o2, 2, path, sizeof(path));
    KCHECK_EQ(rc, 2);
    KCHECK_EQ(o2[0], 0u);
    KCHECK_EQ(o2[1], 1u);
    return 0;
}

// 5) 三节点环 a->b->c->a；6) 自环；7) 重名
int32_t test_cycles_and_duplicates()
{
    const char *deps_a[] = {"b"};
    const char *deps_b[] = {"c"};
    const char *deps_c[] = {"a"};
    kmod_topo_node_t cyc[3];
    cyc[0] = {"a", deps_a, 1};
    cyc[1] = {"b", deps_b, 1};
    cyc[2] = {"c", deps_c, 1};
    uint32_t order[8] = {};
    char path[256] = {};
    int32_t rc = KmodTopoSort(cyc, 3, order, 8, path, sizeof(path));
    KCHECK_EQ(rc, KMOD_ERR_CYCLE);
    KCHECK(path[0] != '\0');
    KCHECK(std::strstr(path, "->") != nullptr);
    // 路径必须至少覆盖一个节点名（形如 a->b->a / b->c->b / c->a->c）
    KCHECK(std::strstr(path, "a") || std::strstr(path, "b") ||
           std::strstr(path, "c"));
    KT_INFO("3-cycle path = %s", path);

    // 自环
    const char *self_deps[] = {"solo"};
    kmod_topo_node_t self[1];
    self[0] = {"solo", self_deps, 1};
    path[0] = '\0';
    rc = KmodTopoSort(self, 1, order, 8, path, sizeof(path));
    KCHECK_EQ(rc, KMOD_ERR_CYCLE);
    KCHECK(std::strstr(path, "solo") != nullptr);
    KT_INFO("self-cycle path = %s", path);

    // 重名节点：图不唯一 -> KMOD_ERR_INVAL
    kmod_topo_node_t dup[2];
    dup[0] = {"same.name", nullptr, 0};
    dup[1] = {"same.name", nullptr, 0};
    rc = KmodTopoSort(dup, 2, order, 8, path, sizeof(path));
    KCHECK_EQ(rc, KMOD_ERR_INVAL);

    // 空节点名 / 空 deps 指针
    kmod_topo_node_t bad[1];
    bad[0] = {"", nullptr, 0};
    KCHECK_EQ(KmodTopoSort(bad, 1, order, 8, path, sizeof(path)),
              KMOD_ERR_INVAL);
    bad[0] = {"a.b", nullptr, 1};
    KCHECK_EQ(KmodTopoSort(bad, 1, order, 8, path, sizeof(path)),
              KMOD_ERR_INVAL);
    return 0;
}

// 8) 外部依赖（"kernel" 与集合外名字）视为已满足
int32_t test_external_deps()
{
    const char *kernel_deps[] = {"kernel"};
    const char *ext_deps[] = {"vendor.not.in.graph"};
    kmod_topo_node_t nodes[2];
    nodes[0] = {"m1.x", kernel_deps, 1};
    nodes[1] = {"m2.y", ext_deps, 1};
    uint32_t order[2] = {};
    char path[64] = {};
    int32_t rc = KmodTopoSort(nodes, 2, order, 2, path, sizeof(path));
    KCHECK_MSG(rc == 2, "external deps should be treated as satisfied, rc=%d",
               (int)rc);
    KCHECK_EQ(order[0], 0u);
    KCHECK_EQ(order[1], 1u);
    return 0;
}

// 9) 版本解析 / 比较 / 区间
int32_t test_version()
{
    struct P {
        const char *s;
        bool ok;
        uint32_t maj, min, pat;
    } pos[] = {
        {"1.2.3", true, 1, 2, 3},
        {"1.2", true, 1, 2, 0},
        {"1", true, 1, 0, 0},
        {"0.0.0", true, 0, 0, 0},
        {"65535.65535.65535", true, 65535, 65535, 65535},
        {"01.2.3", true, 1, 2, 3}, // 前导零按十进制解析
    };
    for (const P &p : pos) {
        uint32_t a = 0, b = 0, c = 0;
        int32_t rc = KmodVersionParse(p.s, &a, &b, &c);
        KCHECK_MSG(rc == KMOD_OK, "parse('%s') rc=%d", p.s, (int)rc);
        KCHECK_EQ(a, p.maj);
        KCHECK_EQ(b, p.min);
        KCHECK_EQ(c, p.pat);
    }
    // nullptr 输出参数也必须可用（校验路径）
    KCHECK_EQ(KmodVersionParse("1.2.3", nullptr, nullptr, nullptr), KMOD_OK);

    const char *bad[] = {
        "",          "1.2.3.4",  "1..2",     ".1.2",   "1.2.",
        "a.b.c",     "1.2.x",    "-1.2.3",   " 1.2.3", "1.2.3 ",
        "65536.0.0", "1.65536.0", "999999999999999999999.0.0", "1.2.-3",
    };
    for (const char *s : bad) {
        int32_t rc = KmodVersionParse(s, nullptr, nullptr, nullptr);
        KCHECK_MSG(rc == KMOD_ERR_VERSION, "parse('%s') should fail, rc=%d", s,
                   (int)rc);
    }

    // 比较
    KCHECK(KmodVersionCompare("1.0.0", "1.0.1") < 0);
    KCHECK(KmodVersionCompare("1.2.0", "1.10.0") < 0);
    KCHECK(KmodVersionCompare("2.0.0", "1.99.99") > 0);
    KCHECK(KmodVersionCompare("1.0", "1.0.0") == 0);
    KCHECK(KmodVersionCompare("1.0.0", "1.0") == 0);
    KCHECK(KmodVersionCompare("bad", "1.0.0") < 0); // 非法视为最小
    KCHECK(KmodVersionCompare("1.0.0", "bad") > 0);
    KCHECK(KmodVersionCompare("bad", "bad") == 0);

    // 区间：[vmin,vmax]，0 = 不限
    KCHECK_EQ(KmodVersionInRange(5, 1, 10), KMOD_OK);
    KCHECK_EQ(KmodVersionInRange(1, 1, 10), KMOD_OK);
    KCHECK_EQ(KmodVersionInRange(10, 1, 10), KMOD_OK);
    KCHECK_EQ(KmodVersionInRange(0, 1, 10), KMOD_ERR_VERSION);
    KCHECK_EQ(KmodVersionInRange(11, 1, 10), KMOD_ERR_VERSION);
    KCHECK_EQ(KmodVersionInRange(3, 1, 0), KMOD_OK); // 无上界
    KCHECK_EQ(KmodVersionInRange(3, 0, 0), KMOD_OK); // 无区间
    KCHECK_EQ(KmodVersionInRange(5, 10, 1), KMOD_ERR_VERSION); // 区间倒置
    return 0;
}

// 10) 模块名/契约名/段名校验
int32_t test_names()
{
    const char *good_modules[] = {
        "vendor.demo.hello", "a.b", "vendor_x.area", "v1.x2",
        "vendor.test.a",     "acme.fs.v2",
    };
    for (const char *s : good_modules)
        KCHECK_MSG(KmodNameValidate(s) == KMOD_OK, "name '%s' should pass", s);

    const char *bad_modules[] = {
        "",            "a",           ".a.b",       "a.",
        "a..b",        "Vendor.demo", "vendor.Demo", "vendor.demo-1",
        "vendor.3x",   "vendor.demo!", "kernel.mem.v1", "core.x.y",
        "_a.b",        "a b.c",       "vendor.3x",
        "vendor.verylongsegmentname.anotherlongsegment.v1", // >47 字符
    };
    for (const char *s : bad_modules)
        KCHECK_MSG(KmodNameValidate(s) == KMOD_ERR_NAME,
                   "name '%s' should be rejected", s);

    // 契约名：规则同模块名，但允许 kernel./core. 保留前缀（Tier0 用）
    KCHECK_EQ(KmodContractValidate("kernel.mem.v1"), KMOD_OK);
    KCHECK_EQ(KmodContractValidate("kernel.syscall.v1"), KMOD_OK);
    KCHECK_EQ(KmodContractValidate("vendor.test.a.ops.v1"), KMOD_OK);
    KCHECK_EQ(KmodContractValidate("kernel"), KMOD_ERR_NAME);
    KCHECK_EQ(KmodContractValidate(""), KMOD_ERR_NAME);
    KCHECK_EQ(KmodContractValidate("Kernel.mem.v1"), KMOD_ERR_NAME);

    // 段名：".in"/".out"/".kmod_manifest"/".bss" 合法；空/越界字符/超长非法
    const char *good_segs[] = {".in", ".out",    ".kmod_manifest", ".bss",
                               ".text", "text",  "_odd-name"};
    for (const char *s : good_segs)
        KCHECK_MSG(KmodSegNameValidate(s, 63) == KMOD_OK, "seg '%s' should pass",
                   s);
    const char *bad_segs[] = {"",      "bad/name", "bad name", "bad\tname",
                              "a\\b",  "$odd-name", "-odd"};
    for (const char *s : bad_segs)
        KCHECK_MSG(KmodSegNameValidate(s, 63) == KMOD_ERR_INVAL,
                   "seg '%s' should be rejected", s);
    KCHECK_EQ(KmodSegNameValidate(nullptr, 63), KMOD_ERR_INVAL);
    KCHECK_EQ(KmodSegNameValidate(".in", 2), KMOD_ERR_INVAL); // 长度超限
    KCHECK_EQ(KmodSegNameValidate(".in", 3), KMOD_OK);
    return 0;
}

} // namespace

extern "C" int32_t test_topology_run(void)
{
    int32_t bad = 0;
    if (test_random_dag() != 0)
        bad++;
    if (test_edge_cases() != 0)
        bad++;
    if (test_cycles_and_duplicates() != 0)
        bad++;
    if (test_external_deps() != 0)
        bad++;
    if (test_version() != 0)
        bad++;
    if (test_names() != 0)
        bad++;
    return bad == 0 ? 0 : 1;
}
