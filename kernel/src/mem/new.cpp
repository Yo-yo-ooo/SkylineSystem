//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
//#include <mem/heap.h>
#include <stddef.h>
#include <mem/new2.h>
#include <klib/klib.h>
/* klib.h 会间接引入 mem/new.hpp, 其末尾的 #define new 跟踪宏会破坏
   operator new 的定义; 在此解除 */
#ifdef new
#undef new
#endif
const char* _X__file__ = "unknown";
const char* _X__func__ = "unknown";
size_t _X__line__ = 0;

void* operator new(size_t size) {
    void *ptr = _Ymalloc(size, _X__func__, _X__file__, _X__line__);
    _X__file__ = "unknown";
    _X__func__ = "unknown";
    _X__line__ = 0;
    return ptr;
}

/* 从 heap.h/new.hpp 移入的定义(原为头文件内联定义, clang 报 -Winline-new-delete) */
void operator delete(void* p) { kfree(p); }
void operator delete(void* ptr, unsigned long) { kfree(ptr); }
void operator delete[](void* ptr) noexcept { kfree(ptr); }

void* operator new[](unsigned long size) {
    void* ptr;
    if ((ptr = kmalloc(size))) {
        return ptr;
    }
    kerror("BAD MALLOC %p", ptr);
    for (;;) {
#ifdef __x86_64__
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
#endif
    }
}