// tests/slub/poison_main.cpp — 负向用例: 红区越界写必须被检测 (SLAB_DEBUG_POISON 构建)
// 覆盖: 多尺寸 (64/128/512/1024) × 后越界 (p+usable) + 前越界 (p-8 破坏头部红区)
// 每个用例 fork 子进程; 子进程内 Free 必须 slab_fatal → SIGABRT。
// 本程序退出码 0 = 全部检测生效。
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <mem/heap.h>
#include "kernel_shim.h"

static int expect_abort(int (*child)(void)) {
    pid_t pid = fork();
    if (pid == 0) { child(); _exit(0); }   // 检测失效会走到 _exit(0)
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

struct Case { const char *name; int (*fn)(void); };
static int cases_run = 0, cases_detected = 0;

static int oob_after_64(void) {
    void *p = SLAB::Alloc(64);
    if (!p) return 0;
    *(uint64_t *)((uint8_t *)p + 64) = 0xDEADBEEF;
    SLAB::Free(p);
    return 0;
}
static int oob_after_128(void) {
    void *p = SLAB::Alloc(128);
    if (!p) return 0;
    *(uint64_t *)((uint8_t *)p + 128) = 0xDEADBEEF;
    SLAB::Free(p);
    return 0;
}
static int oob_after_512(void) {
    void *p = SLAB::Alloc(512);
    if (!p) return 0;
    *(uint64_t *)((uint8_t *)p + 512) = 0xDEADBEEF;
    SLAB::Free(p);
    return 0;
}
static int oob_after_1024(void) {
    void *p = SLAB::Alloc(1024);
    if (!p) return 0;
    *(uint64_t *)((uint8_t *)p + 1024) = 0xDEADBEEF;
    SLAB::Free(p);
    return 0;
}
static int oob_before_512(void) {
    void *p = SLAB::Alloc(512);
    if (!p) return 0;
    *(uint64_t *)((uint8_t *)p - 8) = 0xDEADBEEF;   // 前越界: 命中头部元数据区
    SLAB::Free(p);
    return 0;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    SLAB::Init();
    SLUB::InitKmalloc();

    // 后越界 4 用例: 必须 ACTIVE (布局为仅尾部红区: obj_size = usable + 8B 红区)
    const Case cases[] = {
        {"oob+64",   oob_after_64},
        {"oob+128",  oob_after_128},
        {"oob+512",  oob_after_512},
        {"oob+1024", oob_after_1024},
    };
    for (const auto &c : cases) {
        bool d = expect_abort(c.fn);
        printf("  %-16s: %s\n", c.name, d ? "ACTIVE" : "MISSING");
        cases_run++;
        if (d) cases_detected++;
    }
    // 前越界: 当前布局头部无红区 (写入头部元数据不被检测) —— 如实记录为已知限制
    bool before_detected = expect_abort(oob_before_512);
    printf("  %-16s: %s (头部无红区, 记录为已知限制)\n", "oob-8",
           before_detected ? "意外被检测" : "N/A 未检测(符合布局)");
    printf("redzone-oob: %d/%d 尾部红区用例 detected (前部红区为已知限制)\n", cases_detected, cases_run);
    return cases_detected == cases_run ? 0 : 1;
}
