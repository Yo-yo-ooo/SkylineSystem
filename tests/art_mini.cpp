// tests/art_mini.cpp — libart 前缀冲突隔离测试
#include <cstdio>
#include <cstring>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include "kernel_shim.h"

int main() {
    SLAB::Init();
    SLUB::InitKmalloc();
    art_tree t;
    art_tree_init(&t);
    int failures = 0;

    // 场景1: "file1" 是 "file10" 的前缀
    const char *k1 = "file1";
    const char *k10 = "file10";
    art_insert(&t, (const uint8_t*)k1, 5, (void*)0x1111);
    art_insert(&t, (const uint8_t*)k10, 6, (void*)0x2222);
    void *v = art_search(&t, (const uint8_t*)k10, 6);
    printf("insert 'file1' then 'file10': search file10 -> %p (expect 0x2222)\n", v);
    if (v != (void*)0x2222) failures++;
    v = art_search(&t, (const uint8_t*)k1, 5);
    printf("search file1 -> %p (expect 0x1111)\n", v);
    if (v != (void*)0x1111) failures++;

    // 场景2: 反向插入
    art_tree t2;
    art_tree_init(&t2);
    art_insert(&t2, (const uint8_t*)k10, 6, (void*)0x2222);
    art_insert(&t2, (const uint8_t*)k1, 5, (void*)0x1111);
    v = art_search(&t2, (const uint8_t*)k10, 6);
    printf("insert 'file10' then 'file1': search file10 -> %p (expect 0x2222)\n", v);
    if (v != (void*)0x2222) failures++;
    v = art_search(&t2, (const uint8_t*)k1, 5);
    printf("search file1 -> %p (expect 0x1111)\n", v);
    if (v != (void*)0x1111) failures++;

    // 场景3: 覆盖同键
    art_insert(&t2, (const uint8_t*)k10, 6, (void*)0x3333);
    v = art_search(&t2, (const uint8_t*)k10, 6);
    printf("overwrite file10 -> %p (expect 0x3333)\n", v);
    if (v != (void*)0x3333) failures++;

    // 场景4: 删除后残留
    void *d = art_delete(&t, (const uint8_t*)k1, 5);
    printf("delete file1 -> %p (expect 0x1111)\n", d);
    v = art_search(&t, (const uint8_t*)k1, 5);
    printf("search file1 after delete -> %p (expect nil)\n", v);
    if (v) failures++;
    v = art_search(&t, (const uint8_t*)k10, 6);
    printf("search file10 after deleting file1 -> %p (expect 0x2222)\n", v);
    if (v != (void*)0x2222) failures++;

    // 场景5: 三层前缀链 file1 -> file10 -> file100
    art_tree t3;
    art_tree_init(&t3);
    art_insert(&t3, (const uint8_t*)"file100", 7, (void*)0xAAAA);
    art_insert(&t3, (const uint8_t*)"file1", 5, (void*)0xBBBB);
    art_insert(&t3, (const uint8_t*)"file10", 6, (void*)0xCCCC);
    printf("3-level: search file10 -> %p (expect 0xCCCC)\n", art_search(&t3, (const uint8_t*)"file10", 6));
    if (art_search(&t3, (const uint8_t*)"file10", 6) != (void*)0xCCCC) failures++;
    printf("3-level: search file1 -> %p (expect 0xBBBB)\n", art_search(&t3, (const uint8_t*)"file1", 5));
    printf("3-level: search file100 -> %p (expect 0xAAAA)\n", art_search(&t3, (const uint8_t*)"file100", 7));
    // 删除中缀后前后缀仍可达
    void *d5 = art_delete(&t3, (const uint8_t*)"file10", 6);
    printf("3-level: delete file10 -> %p (expect 0xCCCC)\n", d5);
    printf("3-level: after delete search file100 -> %p (expect 0xAAAA)\n", art_search(&t3, (const uint8_t*)"file100", 7));
    if (art_search(&t3, (const uint8_t*)"file100", 7) != (void*)0xAAAA) failures++;

    // 场景6: node16 前缀分裂 —— file10 是 file105 的前缀 (真实崩溃场景)
    art_tree t4;
    art_tree_init(&t4);
    art_insert(&t4, (const uint8_t*)"file105", 7, (void*)0x1);
    art_insert(&t4, (const uint8_t*)"file111", 7, (void*)0x2);
    art_insert(&t4, (const uint8_t*)"file115", 7, (void*)0x3);
    art_insert(&t4, (const uint8_t*)"file118", 7, (void*)0x4);
    art_insert(&t4, (const uint8_t*)"file119", 7, (void*)0x5);
    art_insert(&t4, (const uint8_t*)"file12", 6, (void*)0x6);
    art_insert(&t4, (const uint8_t*)"file124", 7, (void*)0x7);
    art_insert(&t4, (const uint8_t*)"file10", 6, (void*)0x8);
    void *v6 = art_search(&t4, (const uint8_t*)"file10", 6);
    printf("node16-split: search file10 -> %p (expect 0x8)\n", v6);
    if (v6 != (void*)0x8) failures++;
    v6 = art_search(&t4, (const uint8_t*)"file105", 7);
    printf("node16-split: search file105 -> %p (expect 0x1)\n", v6);
    if (v6 != (void*)0x1) failures++;

    printf("art_mini RESULT: failures=%d\n", failures);
    return failures ? 1 : 0;
}
