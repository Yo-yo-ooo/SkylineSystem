//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_00_smoke.cpp — 基础设施自检
//   框架与被测库的最小闭环：类型工具、分配器观测、placement new。
//=============================================================
#include "framework/test_framework.h"

#include <cstddef>
#include <cstdint>
#include <climits>
#include <cfloat>
#include <type_traits>
#include <utility>
#include <new>

// 让指针逃逸，阻止 GCC 把 new 表达式优化到栈上（否则这条用例测不到分配器）
static void* volatile g_sink;

TEST("smoke/framework allocation tracking") {
    CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)0);
    int* p = new int(42);
    g_sink = p;
    CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)1);
    CHECK_EQ(*p, 42);
    delete p;
    g_sink = nullptr;
    CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)0);
    CHECK_LEAK_FREE();
}

TEST("smoke/cstddef basics") {
    CHECK_EQ(sizeof(std::size_t), sizeof(void*));
    static_assert(std::is_same<std::size_t, decltype(sizeof(int))>::value,
                  "size_t must match sizeof result");
    std::byte b{0x0F};
    b <<= 4;
    CHECK_EQ(std::to_integer<unsigned>(b), 0xF0u);
    CHECK_EQ(std::to_integer<unsigned>(~b), 0x0Fu);
    CHECK(static_cast<bool>(std::is_same<std::nullptr_t, decltype(nullptr)>::value));
}

TEST("smoke/cstdint & limits") {
    CHECK_EQ(sizeof(std::int8_t), (std::size_t)1);
    CHECK_EQ(sizeof(std::int64_t), (std::size_t)8);
    CHECK(INT_MAX > 0);
    CHECK(LLONG_MAX > INT_MAX);
    CHECK(DBL_EPSILON > 0.0);
}

TEST("smoke/type_traits core") {
    static_assert(std::is_integral<int>::value, "");
    static_assert(!std::is_integral<double>::value, "");
    static_assert(std::is_same<std::remove_reference<int&>::type, int>::value, "");
    static_assert(std::is_same<std::remove_const<const int>::type, int>::value, "");
    static_assert(std::is_same<std::conditional<true, int, double>::type, int>::value, "");
    static_assert(std::is_same<std::decay<int[3]>::type, int*>::value, "");
    static_assert(std::is_same<std::enable_if<true, int>::type, int>::value, "");
    static_assert(std::is_same<std::common_type<int, long>::type, long>::value, "");
    CHECK(true);
}

TEST("smoke/utility pair & move") {
    std::pair<int, int> p(1, 2);
    CHECK_EQ(p.first, 1);
    CHECK_EQ(p.second, 2);
    std::pair<int, int> q = std::make_pair(3, 4);
    CHECK_EQ(q.second, 4);
    int a = 1, b = 2;
    std::swap(a, b);
    CHECK_EQ(a, 2);
    CHECK_EQ(b, 1);

    struct MoveOnly {
        int v;
        explicit MoveOnly(int x) : v(x) {}
        MoveOnly(MoveOnly&& o) : v(o.v) { o.v = -1; }
        MoveOnly& operator=(MoveOnly&& o) { v = o.v; o.v = -1; return *this; }
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
    };
    MoveOnly m1(7);
    MoveOnly m2(std::move(m1));
    CHECK_EQ(m2.v, 7);
    CHECK_EQ(m1.v, -1);
}

TEST("smoke/placement new") {
    alignas(int) unsigned char buf[sizeof(int)];
    int* p = new (buf) int(99);
    CHECK_EQ(*p, 99);
    typedef int int_t;
    p->~int_t();
}
