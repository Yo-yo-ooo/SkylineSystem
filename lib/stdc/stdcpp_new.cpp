//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// lib/stdc/stdcpp_new.cpp — 用户态 C++ 全局分配函数
//
// 背景：
//   stdcpp 的所有容器（vector/string/map/...）统一经
//   detail/alloc.h -> ::operator new 分配，而不是直接 malloc。
//   内核侧由 kernel/src/mem/new.cpp 提供这些符号；
//   用户态此前**没有任何** operator new 定义，导致任何 C++ 程序一旦
//   使用 stdcpp 容器就会在链接期报 undefined symbol: operator new(unsigned long)。
//   本文件补上这层桥接（malloc/free 由 lib/stdc/allocator 提供）。
//
// 注意：内核不链接 lib.a，两边不会重复定义。
//=============================================================
#include <stddef.h>
#include <stdlib.h>

namespace {

// 分配失败时的兜底：用户态没有异常运行时，直接终止进程比返回 nullptr
// （随后必然空指针解引用）更可控。
inline void* alloc_or_die(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) _Exit();      // lib/stdc/stdlib.h 里声明为 void _Exit(void)
    return p;
}

} // namespace

void* operator new(size_t n) { return alloc_or_die(n); }
void* operator new[](size_t n) { return alloc_or_die(n); }

void operator delete(void* p) noexcept { if (p) free(p); }
void operator delete[](void* p) noexcept { if (p) free(p); }

void operator delete(void* p, size_t) noexcept { if (p) free(p); }
void operator delete[](void* p, size_t) noexcept { if (p) free(p); }
