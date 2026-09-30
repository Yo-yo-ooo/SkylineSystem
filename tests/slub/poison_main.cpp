// tests/slub/poison_main.cpp — 负向用例: 红区越界写必须被检测 (SLAB_DEBUG_POISON 构建)
// SLAB::Alloc(512): 对象含 8B 红区, usable=512; 写入 p+512 破坏红区 magic,
// Free 时必须 slab_fatal → SIGABRT。本程序自身退出码 0 = 检测生效。
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <mem/heap.h>
#include "kernel_shim.h"

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    SLAB::Init();
    SLUB::InitKmalloc();

    pid_t pid = fork();
    if (pid == 0) {
        void *p = SLAB::Alloc(512);
        if (!p) _exit(2);
        memset(p, 0x42, 512);
        // 越界写: 破坏红区 magic (usable=512, 红区在 p+512)
        *(uint64_t *)((uint8_t *)p + 512) = 0x12345678;
        SLAB::Free(p);
        _exit(0);   // 若检测失效会走到这里
    }
    int st = 0;
    waitpid(pid, &st, 0);
    bool detected = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
    printf("redzone-oob-detect: %s (越界写被 Free 检测并 SIGABRT)\n", detected ? "ACTIVE" : "MISSING");
    return detected ? 0 : 1;
}
