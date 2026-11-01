//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// detail/alloc.h — 裸字节分配桥
//
// 为什么需要这一层：
//   旧实现各容器直接 `malloc/free`，而 SkylineSystem 内核根本没有 malloc
//   （intra-kernel 只有 kmalloc，对外经 operator new 暴露）。直接调 malloc
//   会导致内核侧链接失败，且绕过 operator new 会丢掉内核的 OOM 诊断。
//   统一改为走 replaceable ::operator new / ::operator delete：
//     * 内核:  mem/new.cpp
//     * 用户态: lib/stdc/stdcpp_new.cpp
//
// 过度对齐（alignof(T) > max_align_t）用“手工多分配 + 前置原始指针”实现，
// 不依赖 C++17 的 aligned new：内核并未提供 operator new(size_t, align_val_t)。
//=============================================================
#ifndef STDCPP_DETAIL_ALLOC_H
#define STDCPP_DETAIL_ALLOC_H

#include <cstddef>
// 各容器都要用 placement new 构造元素；<new> 只依赖 detail/config.h，
// 与 alloc.h 无循环包含。放在这里可以避免每个容器各自忘记 include。
#include <new>
#include "config.h"
#include "throw.h"

// replaceable 全局分配函数只做声明；定义由平台提供（内核 new.cpp /
// 用户态 stdcpp_new.cpp）。重复声明在 C++ 中是合法的。
void* operator new(std::size_t);
void* operator new[](std::size_t);
void operator delete(void*) noexcept;
void operator delete[](void*) noexcept;
void operator delete(void*, std::size_t) noexcept;
void operator delete[](void*, std::size_t) noexcept;

namespace std {
namespace __detail {

//-------------------------------------------------------------
// 基本字节分配
//-------------------------------------------------------------
inline void* alloc_bytes(size_t n) {
    // 标准要求 operator new 失败时抛 bad_alloc；内核实现在失败时直接停机。
    // 这里不额外检查 nullptr，保持与 operator new 语义一致。
    return ::operator new(n ? n : 1);
}

inline void dealloc_bytes(void* p) noexcept {
    if (p) ::operator delete(p);
}

//-------------------------------------------------------------
// 过度对齐分配
//-------------------------------------------------------------
inline void* alloc_bytes_aligned(size_t n, size_t align) {
    if (align <= alignof(std::max_align_t)) return alloc_bytes(n);
    // 布局: [原始指针][padding][对齐后的用户区]
    size_t total = n + align + sizeof(void*);
    void* raw = alloc_bytes(total);
    size_t addr = reinterpret_cast<size_t>(raw) + sizeof(void*);
    size_t aligned = (addr + align - 1) & ~(align - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

inline void dealloc_bytes_aligned(void* p, size_t align) noexcept {
    if (!p) return;
    if (align <= alignof(std::max_align_t)) { dealloc_bytes(p); return; }
    dealloc_bytes(reinterpret_cast<void**>(p)[-1]);
}

//-------------------------------------------------------------
// 分配并构造 / 析构并释放（容器批量路径用）
//-------------------------------------------------------------
template<class T>
inline T* alloc_array(size_t n) {
    if (n == 0) return nullptr;
    return static_cast<T*>(alloc_bytes_aligned(n * sizeof(T), alignof(T)));
}

template<class T>
inline void dealloc_array(T* p, size_t n) noexcept {
    if (!p) return;
    (void)n;
    dealloc_bytes_aligned(p, alignof(T));
}

// 分配并清零（替代 calloc：内核没有 calloc，且这里要能带上对齐信息）
template<class T>
inline T* alloc_array_zero(size_t n) {
    if (n == 0) return nullptr;
    T* p = alloc_array<T>(n);
    __builtin_memset(p, 0, n * sizeof(T));
    return p;
}

} // namespace __detail
} // namespace std

#endif // STDCPP_DETAIL_ALLOC_H
