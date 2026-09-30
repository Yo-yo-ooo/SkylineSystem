// tests/art_replay.cpp — 重放 fc 混沌中 cache0 索引树的真实变更序列,
// 每步校验"遍历可见 ⇔ 搜索可达"一致性, 找到第一个破坏点。
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include "kernel_shim.h"
#undef __init
#undef max
#undef min
#undef swap
extern "C" void art_deep_dump(art_tree *t);

struct Op { uint64_t n; bool ins; std::string key; };

static int g_fail = 0;
static art_tree *g_tree;
static int32_t check_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    (void)data;
    void *back = art_search(g_tree, key, (int32_t)key_len);
    if (back != value) {
        printf("INCONSISTENT: key='%.*s' iter=%p search=%p\n", (int)key_len, (const char*)key, value, back);
        g_fail = 1;
    }
    return 0;
}

int main() {
    SLAB::Init();
    SLUB::InitKmalloc();
    art_tree t;
    art_tree_init(&t);
    g_tree = &t;

    FILE *f = fopen("/tmp/art_ops.txt", "r");
    if (!f) { printf("no ops file\n"); return 2; }
    char line[256];
    std::vector<Op> ops;
    while (fgets(line, sizeof line, f)) {
        // 去掉行尾换行
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        Op o;
        char *p = line;
        o.n = strtoull(p, &p, 10);
        if (strncmp(p, "INS", 3) == 0) { o.ins = true; p += 3; }
        else if (strncmp(p, "DEL", 3) == 0) { o.ins = false; p += 3; }
        else continue;
        p = strstr(p, "'");
        if (!p) continue;
        p++;
        char *q = strchr(p, '\'');
        o.key.assign(p, q ? (size_t)(q - p) : strlen(p));
        ops.push_back(o);
    }
    fclose(f);
    printf("loaded %zu ops\n", ops.size());

    for (size_t i = 0; i < ops.size(); i++) {
        Op &o = ops[i];
        if (i == 76) { printf("== REPLAY DUMP BEFORE LINE 77 ==\n"); art_deep_dump(&t); }
        if (o.ins) {
            void *old = art_insert(&t, (const uint8_t*)o.key.data(), (int32_t)o.key.size(), (void*)(uintptr_t)(i + 1));
            void *back = art_search(&t, (const uint8_t*)o.key.data(), (int32_t)o.key.size());
            if (back != (void*)(uintptr_t)(i + 1)) {
                printf("FIRST BREAK after op %zu (%llu INS '%s'): search=%p expected=%p old=%p\n",
                       i, (unsigned long long)o.n, o.key.c_str(), back, (void*)(uintptr_t)(i + 1), old);
                return 1;
            }
        } else {
            art_delete(&t, (const uint8_t*)o.key.data(), (int32_t)o.key.size());
        }

        g_fail = 0;
        art_iter(&t, check_cb, NULL);
        if (g_fail) {
            printf("FIRST BREAK after op %zu (%llu %s '%s')\n",
                   i, (unsigned long long)o.n, o.ins ? "INS" : "DEL", o.key.c_str());
            return 1;
        }
    }
    printf("REPLAY OK: all %zu ops consistent\n", ops.size());
    return 0;
}
