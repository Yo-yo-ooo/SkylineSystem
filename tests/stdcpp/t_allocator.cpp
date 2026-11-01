//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_allocator.cpp — 有状态分配器与传播语义
//
// std::allocator 是无状态的（always_equal、不传播），只能覆盖"平凡路径"。
// 这里用带 id 和计数器的分配器，验证标准的三条传播规则：
//   POCCA 拷贝赋值 → 分配器跟着对方走，旧缓冲区必须用旧分配器释放
//   POCMA 移动赋值 → 为 false 且分配器不等时，禁止窃取缓冲区，退化为逐元素搬
//   POCS  swap     → 为 true 时连分配器一起换
// 这些路径写错时通常表现为"用 A 分配、用 B 释放"，在真实分配器上会崩。
//=============================================================
#include "framework/test_framework.h"
#include <vector>
#include <string>
#include <type_traits>
#include <utility>

namespace {

struct AllocCounters {
    long allocs;
    long deallocs;
    AllocCounters() : allocs(0), deallocs(0) {}
    long live() const { return allocs - deallocs; }
};

template<class T>
struct CountingAlloc {
    typedef T                 value_type;
    typedef T*                pointer;
    typedef const T*          const_pointer;
    typedef T&                reference;
    typedef const T&          const_reference;
    typedef std::size_t       size_type;
    typedef std::ptrdiff_t    difference_type;

    typedef std::true_type  propagate_on_container_copy_assignment;
    typedef std::false_type propagate_on_container_move_assignment;
    typedef std::true_type  propagate_on_container_swap;
    typedef std::false_type is_always_equal;

    template<class U> struct rebind { typedef CountingAlloc<U> other; };

    int            id;
    AllocCounters* c;

    CountingAlloc(int i, AllocCounters* cc) : id(i), c(cc) {}
    template<class U>
    CountingAlloc(const CountingAlloc<U>& o) : id(o.id), c(o.c) {}

    T* allocate(size_type n) {
        ++c->allocs;
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, size_type) {
        ++c->deallocs;
        ::operator delete(p);
    }
};

template<class T, class U>
bool operator==(const CountingAlloc<T>& a, const CountingAlloc<U>& b) {
    return a.id == b.id && a.c == b.c;
}
template<class T, class U>
bool operator!=(const CountingAlloc<T>& a, const CountingAlloc<U>& b) {
    return !(a == b);
}

typedef std::vector<int, CountingAlloc<int> > ivec;
typedef std::basic_string<char, std::char_traits<char>, CountingAlloc<char> > istr;

} // namespace

TEST("allocator/vector uses the supplied allocator") {
    AllocCounters c;
    CountingAlloc<int> al(1, &c);
    {
        ivec v(al);
        for (int i = 0; i < 64; ++i) v.push_back(i);
        CHECK(c.allocs >= 1);
        CHECK_EQ(v.get_allocator().id, 1);
        CHECK_EQ(v.size(), (std::size_t)64);
    }
    CHECK_EQ(c.live(), 0L);          // 全部归还
}

TEST("allocator/POCCA copy assignment adopts the source allocator") {
    AllocCounters ca, cb;
    CountingAlloc<int> ala(1, &ca), alb(2, &cb);

    ivec a(ala);
    for (int i = 0; i < 32; ++i) a.push_back(i);
    ivec b(alb);
    b.push_back(100);
    b.push_back(200);

    const long ca_before = ca.deallocs;
    const long cb_alloc_before = cb.allocs;

    a = b;                            // POCCA = true → a 改用 alb

    CHECK_EQ(a.get_allocator().id, 2);
    CHECK_EQ(a.size(), (std::size_t)2);
    CHECK_EQ(a[0], 100);
    CHECK_EQ(a[1], 200);
    CHECK(ca.deallocs > ca_before);            // 旧缓冲区用旧分配器释放
    CHECK(cb.allocs > cb_alloc_before);        // 新缓冲区用新分配器分配
    CHECK_EQ(cb.live(), 2L);          // a 的新缓冲区 + b 自己的缓冲区

    a.clear();
    a.shrink_to_fit();
    CHECK_EQ(cb.live(), 1L);          // a 释放后只剩 b 自己的缓冲区
}

TEST("allocator/POCMA false + unequal allocators degrades to element move") {
    AllocCounters ca, cb;
    CountingAlloc<int> ala(1, &ca), alb(2, &cb);

    ivec a(ala);
    ivec b(alb);
    for (int i = 0; i < 8; ++i) b.push_back(i * 3);

    a = std::move(b);

    // 分配器不传播：a 仍是 id=1，缓冲区不能被窃取，只能逐元素搬
    CHECK_EQ(a.get_allocator().id, 1);
    CHECK_EQ(a.size(), (std::size_t)8);
    CHECK_EQ(a[3], 9);
    CHECK(b.empty());
    CHECK_EQ(ca.live(), 1L);          // a 的缓冲区由 ca 持有
    CHECK_EQ(cb.live(), 1L);          // b 只被 clear()，容量仍由 cb 持有
}

TEST("allocator/POCS swap exchanges allocators with buffers") {
    AllocCounters ca, cb;
    CountingAlloc<int> ala(1, &ca), alb(2, &cb);

    ivec a(ala), b(alb);
    a.push_back(1);
    b.push_back(2);
    b.push_back(3);

    a.swap(b);
    CHECK_EQ(a.get_allocator().id, 2);
    CHECK_EQ(b.get_allocator().id, 1);
    CHECK_EQ(a.size(), (std::size_t)2);
    CHECK_EQ(b.size(), (std::size_t)1);
    CHECK_EQ(b[0], 1);
    CHECK_EQ(a[1], 3);
}

TEST("allocator/string uses allocator only when leaving SSO") {
    AllocCounters c;
    CountingAlloc<char> al(7, &c);
    {
        istr s(al);
        s = "short";                          // 5 字符，走 SSO
        CHECK_EQ(s, istr("short", al));
        CHECK_EQ(c.allocs, 0L);               // 短串不得分配

        for (int i = 0; i < 100; ++i) s.push_back('x');
        CHECK(c.allocs >= 1);
        CHECK_EQ(s.get_allocator().id, 7);
        CHECK_EQ(s.size(), (std::size_t)105);
    }
    CHECK_EQ(c.live(), 0L);
}

TEST("allocator/string POCCA copy assignment") {
    AllocCounters ca, cb;
    CountingAlloc<char> ala(1, &ca), alb(2, &cb);

    istr a("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", ala);   // 30 字符，堆分配
    istr b("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", alb);

    const long ca_before = ca.deallocs;
    a = b;
    CHECK_EQ(a.get_allocator().id, 2);
    CHECK_EQ(a.size(), b.size());
    CHECK(ca.deallocs > ca_before);

    a = istr("tiny", alb);                           // 回到 SSO
    CHECK_EQ(a, istr("tiny", alb));
}

TEST("allocator/string swap with SSO and heap mix") {
    AllocCounters c;
    CountingAlloc<char> al(3, &c);

    istr short_s("abc", al);                          // SSO
    istr long_s("0123456789abcdefghijklmnopqrstuvwxyz", al);

    short_s.swap(long_s);                             // SSO <-> heap
    CHECK_EQ(short_s.size(), (std::size_t)36);
    CHECK_EQ(long_s.size(), (std::size_t)3);
    CHECK_EQ(long_s, istr("abc", al));
    CHECK_EQ(short_s[35], 'z');
}
