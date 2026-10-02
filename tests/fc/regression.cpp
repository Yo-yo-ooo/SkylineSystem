// tests/fc/regression.cpp — 三个已修缺陷的最小确定性回归测试
// 1) test_art_true_prefix_key: ART 真前缀键越界读
// 2) test_promote_inline_capacity_reuse: promote 内联容量复用溢出
// 3) test_insert_fail_no_dangling: 插入失败后无悬垂引用/双重释放(故障注入)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include <fs/fc.h>
#include "kernel_shim.h"
#undef __init
#undef max
#undef min
#undef swap

static uint64_t failures = 0;
static void check(bool c, const char *what) { if (!c) { failures++; printf("  FAIL: %s\n", what); } }

// promote 接管 data 所有权并用 kfree 释放 —— 数据必须是 kmalloc 缓冲区
static void *kcopy(const void *src, size_t n) {
    void *p = kmalloc(n + 1);
    if (!p) return nullptr;
    memcpy(p, src, n);
    return p;
}

// ---------- 1. ART 真前缀键 ----------
// 关键: 键缓冲区尾部填非零垃圾 —— 修复前(无终结符守卫)插入/搜索必错位
static int test_art_true_prefix_key() {
    printf("== test_art_true_prefix_key ==\n");
    int fail = 0;
    art_tree t;
    art_tree_init(&t);

    // 插入 "file105" (7B) 后再插入 "file10" (6B), 且 file10 缓冲第 7 字节 = 'X'
    char buf10[8] = {'f','i','l','e','1','0','X',0};
    art_insert(&t, (const uint8_t*)"file105", 7, (void*)0x105);
    art_insert(&t, (const uint8_t*)buf10, 6, (void*)0x10);

    if (art_search(&t, (const uint8_t*)"file10", 6) != (void*)0x10) { printf("  FAIL: file10 不可搜索\n"); fail = 1; }
    if (art_search(&t, (const uint8_t*)"file105", 7) != (void*)0x105) { printf("  FAIL: file105 被破坏\n"); fail = 1; }

    // 反向顺序
    art_tree t2;
    art_tree_init(&t2);
    art_insert(&t2, (const uint8_t*)buf10, 6, (void*)0x20);
    art_insert(&t2, (const uint8_t*)"file105", 7, (void*)0x205);
    if (art_search(&t2, (const uint8_t*)"file10", 6) != (void*)0x20) { printf("  FAIL(反序): file10 不可搜索\n"); fail = 1; }
    if (art_search(&t2, (const uint8_t*)"file105", 7) != (void*)0x205) { printf("  FAIL(反序): file105 被破坏\n"); fail = 1; }

    // 删除中缀后两端仍可达
    if (art_delete(&t, (const uint8_t*)"file10", 6) != (void*)0x10) { printf("  FAIL: 删除 file10 返回错误\n"); fail = 1; }
    if (art_search(&t, (const uint8_t*)"file105", 7) != (void*)0x105) { printf("  FAIL: 删除后 file105 丢失\n"); fail = 1; }
    if (art_search(&t, (const uint8_t*)"file10", 6) != nullptr) { printf("  FAIL: 删除后 file10 仍可见\n"); fail = 1; }

    check(fail == 0, "ART 真前缀键(垃圾尾字节)全通过");
    return fail;
}

// ---------- 2. promote 内联容量复用溢出 ----------
static int32_t wb_ok(const uint8_t *k, uint32_t kl, void *d, size_t dl) {
    (void)k; (void)kl; (void)d; (void)dl;
    return 0;
}
static int test_promote_inline_capacity_reuse() {
    printf("== test_promote_inline_capacity_reuse ==\n");
    int fail = 0;
    file_cache_cpu_t c;
    file_cache_cpu_init(&c, 0, wb_ok);

    // 哨兵: 紧邻的另一个条目, 校验其 canary
    char sentinel_key[16] = "sentinel";
    char sentinel_data[64];
    memset(sentinel_data, 0x5C, sizeof sentinel_data);
    void *s_kbuf = kcopy(sentinel_data, 64);
    if (!s_kbuf || file_cache_promote(&c, (const uint8_t*)sentinel_key, 8, s_kbuf, 64, false, 0, 1) != 0)
        { printf("  FAIL: 哨兵 promote 失败\n"); fail = 1; if (s_kbuf) kfree(s_kbuf); }

    // 同一 key: 先 8B 内联(对象容量 8B), 再 64B 内联(超出 → 必须走新条目路径)
    char key[16] = "victim";
    char d8[8]; memset(d8, 0x11, 8);
    void *k8 = kcopy(d8, 8);
    if (!k8 || file_cache_promote(&c, (const uint8_t*)key, 6, k8, 8, false, 0, 1) != 0)
        { printf("  FAIL: 8B promote 失败\n"); fail = 1; if (k8) kfree(k8); }
    char d64[64]; memset(d64, 0x22, 64);
    void *k64 = kcopy(d64, 64);
    if (!k64 || file_cache_promote(&c, (const uint8_t*)key, 6, k64, 64, false, 0, 1) != 0)
        { printf("  FAIL: 64B promote 失败\n"); fail = 1; if (k64) kfree(k64); }

    // 读回校验
    size_t out = 0;
    file_cache_entry_t *e = nullptr;
    void *got = file_cache_get(&c, (const uint8_t*)key, 6, 64, &out, &e);
    if (!got || out != 64 || memcmp(got, d64, 64) != 0) { printf("  FAIL: 读回内容不符\n"); fail = 1; }
    if (e) file_cache_put(&c, e);

    // 哨兵 canary 必须完好 (修复前 53 字节溢出会砸进哨兵条目)
    size_t s_out = 0;
    file_cache_entry_t *se = nullptr;
    void *sg = file_cache_get(&c, (const uint8_t*)sentinel_key, 8, 64, &s_out, &se);
    if (!sg || s_out != 64 || memcmp(sg, sentinel_data, 64) != 0)
        { printf("  FAIL: 哨兵条目被溢出破坏 (修复失效)\n"); fail = 1; }
    if (se) file_cache_put(&c, se);

    file_cache_cpu_destroy(&c);
    check(fail == 0, "promote 内联容量复用不溢出");
    return fail;
}

// ---------- 3. 插入失败后无悬垂 (故障注入) ----------
extern "C" void fc_set_insert_fail_once(void);
static int test_insert_fail_no_dangling() {
    printf("== test_insert_fail_no_dangling ==\n");
    int fail = 0;
    file_cache_cpu_t c;
    file_cache_cpu_init(&c, 0, wb_ok);

    for (int i = 0; i < 200; i++) {
        char key[32];
        snprintf(key, sizeof key, "k%d", i);
        char data[48];
        memset(data, (char)i, sizeof data);
        void *kbuf = kcopy(data, 48);
        if (!kbuf) { printf("  FAIL: kcopy OOM\n"); fail = 1; break; }
        if (i == 100) fc_set_insert_fail_once();   // 强制一次"插入后校验失败"
        int32_t rc = file_cache_promote(&c, (const uint8_t*)key, (uint32_t)strlen(key), kbuf, sizeof data, false, 0, (uint64_t)i);
        if (rc != 0) { printf("  FAIL: promote(%s) rc=%d\n", key, rc); fail = 1; kfree(kbuf); break; }
    }
    // 全树一致性: 每个值可反查且字段合法 (悬垂/双释会在此暴露)
    for (int i = 0; i < 200; i++) {
        char key[32];
        snprintf(key, sizeof key, "k%d", i);
        size_t out = 0;
        file_cache_entry_t *e = nullptr;
        void *d = file_cache_get(&c, (const uint8_t*)key, (uint32_t)strlen(key), 48, &out, &e);
        if (!d || out != 48 || ((char*)d)[0] != (char)i) { printf("  FAIL: get(%s) 失败或内容不符\n", key); fail = 1; break; }
        if (e) file_cache_put(&c, e);
    }
    file_cache_cpu_destroy(&c);
    check(fail == 0, "插入失败后 200 键无悬垂无双重释放");
    return fail;
}

// ---------- 4. 磁盘错误注入: writeback 失败不丢脏 (故障注入支柱"磁盘错误") ----------
static int wb_fail_remaining = 0;
static int32_t wb_flaky(const uint8_t *k, uint32_t kl, void *d, size_t dl) {
    (void)k; (void)kl; (void)d; (void)dl;
    if (wb_fail_remaining > 0) { wb_fail_remaining--; return -5; }   // EIO
    return 0;
}
static int test_disk_error_writeback() {
    printf("== test_disk_error_writeback ==\n");
    int fail = 0;
    file_cache_cpu_t c;
    wb_fail_remaining = 100;               // 全失败窗口
    file_cache_cpu_init(&c, 0, wb_flaky);

    const char *key = "diskerr";
    void *kbuf = kcopy("payload", 7);
    if (!kbuf) { printf("  FAIL: kcopy OOM\n"); fail = 1; return fail; }
    int32_t rc = file_cache_promote(&c, (const uint8_t*)key, 7, kbuf, 7, true, 7, 42);
    if (rc != 0) { printf("  FAIL: promote rc=%d\n", rc); fail = 1; kfree(kbuf); return fail; }

    /* 连续 fsync: 前 5 次计入重试, 达上限后条目进入 WRITEBACK_FAILED,
       不再被选入冲刷 (fc_idle 只选 retries < 5)。
       注意: file_cache_get 拒绝脏条目 (fc.cpp), 故经 ART 索引直查。 */
    for (int i = 0; i < 10; i++) file_cache_fsync(&c, 42);

    file_cache_entry_t *e = (file_cache_entry_t *)art_search(&c.index, (const uint8_t*)key, 7);
    if (!e) { printf("  FAIL: 条目丢失\n"); fail = 1; }
    else {
        check(e->is_dirty, "写回失败后脏位必须保持 (不丢脏)");
        check(e->writeback_retries == 5, "重试计数达上限 5");
        check(e->state == FC_STATE_WRITEBACK_FAILED, "状态 = WRITEBACK_FAILED");
        check(c.total_writeback_failures == 5, "全局失败计数 = 5 (上限后不再尝试)");
    }

    /* 磁盘恢复: 手动重置重试 → fsync → 成功清脏 */
    if (e) {
        e->writeback_retries = 0;
        e->state = FC_STATE_CACHED;
    }
    wb_fail_remaining = 0;                 // 磁盘恢复
    file_cache_fsync(&c, 42);
    e = (file_cache_entry_t *)art_search(&c.index, (const uint8_t*)key, 7);
    if (e) {
        check(!e->is_dirty, "恢复后 fsync 清脏");
        check(e->state == FC_STATE_CACHED, "恢复后状态回 CACHED");
    } else { printf("  FAIL: 恢复后条目丢失\n"); fail = 1; }

    file_cache_cpu_destroy(&c);
    check(fail == 0, "磁盘错误注入: 失败不丢脏 + 重试上限 + 恢复清脏");
    return fail;
}

// ---------- 5. 写回内容校验 (P4-90): fsync 落盘的字节必须与缓存一致 ----------
static uint8_t g_disk[256];
static uint32_t g_disk_len = 0;
static int32_t wb_capture(const uint8_t *k, uint32_t kl, void *d, size_t dl) {
    (void)k; (void)kl;
    g_disk_len = (dl < sizeof(g_disk)) ? (uint32_t)dl : (uint32_t)sizeof(g_disk);
    if (g_disk_len) memcpy(g_disk, d, g_disk_len);
    return 0;
}
static int test_flush_writeback_content() {
    printf("== test_flush_writeback_content ==\n");
    int fail = 0;
    file_cache_cpu_t c;
    file_cache_cpu_init(&c, 0, wb_capture);

    const char *key = "content";
    uint8_t payload[64];
    for (int i = 0; i < 64; i++) payload[i] = (uint8_t)(i * 3 + 1);
    void *kbuf = kcopy(payload, 64);
    if (!kbuf) { printf("  FAIL: kcopy OOM\n"); return 1; }
    int32_t rc = file_cache_promote(&c, (const uint8_t*)key, 7, kbuf, 64, true, 64, 7);
    if (rc != 0) { printf("  FAIL: promote rc=%d\n", rc); kfree(kbuf); return 1; }

    g_disk_len = 0;
    file_cache_fsync(&c, 7);

    check(g_disk_len == 64, "写回长度 = 64");
    check(memcmp(g_disk, payload, 64) == 0, "写回内容逐字节一致");
    {
        file_cache_entry_t *e = (file_cache_entry_t *)art_search(&c.index, (const uint8_t*)key, 7);
        if (e) check(!e->is_dirty, "成功写回后清脏");
        else { printf("  FAIL: 条目丢失\n"); fail = 1; }
    }

    file_cache_cpu_destroy(&c);
    check(fail == 0, "写回内容校验全通过");
    return fail;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    SLAB::Init();
    SLUB::InitKmalloc();
    test_art_true_prefix_key();
    test_promote_inline_capacity_reuse();
    test_insert_fail_no_dangling();
    test_disk_error_writeback();
    test_flush_writeback_content();
    printf("FC REGRESSION RESULT: failures=%llu\n", (unsigned long long)failures);
    return failures == 0 ? 0 : 1;
}
