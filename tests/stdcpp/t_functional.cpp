//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_functional.cpp — functional / tuple / utility
//=============================================================
#include "framework/test_framework.h"
#include <functional>
#include <tuple>
#include <utility>
#include <string>

namespace {

int add_one(int x) { return x + 1; }

struct Adder {
    int base;
    int operator()(int x) const { return base + x; }
};

int apply_twice(const std::function<int(int)>& f, int x) { return f(f(x)); }

} // namespace

TEST("functional/arithmetic and comparison") {
    CHECK(std::plus<int>()(2, 3) == 5);
    CHECK(std::minus<int>()(7, 3) == 4);
    CHECK(std::multiplies<int>()(3, 4) == 12);
    CHECK(std::divides<int>()(9, 3) == 3);
    CHECK(std::modulus<int>()(10, 3) == 1);
    CHECK(std::negate<int>()(5) == -5);

    CHECK(std::less<int>()(1, 2));
    CHECK(!std::less<int>()(2, 1));
    CHECK(std::greater<int>()(2, 1));
    CHECK(std::equal_to<int>()(3, 3));
    CHECK(std::not_equal_to<int>()(3, 4));
    CHECK(std::less_equal<int>()(3, 3));
    CHECK(std::greater_equal<int>()(4, 3));
}

TEST("functional/logical") {
    CHECK(std::logical_and<bool>()(true, true));
    CHECK(!std::logical_and<bool>()(true, false));
    CHECK(std::logical_or<bool>()(false, true));
    CHECK(!std::logical_not<bool>()(true));
}

TEST("functional/function wraps callables") {
    std::function<int(int)> f;
    CHECK(!f);                                  // 空 function

    f = add_one;
    CHECK_EQ(f(1), 2);

    int base = 10;
    f = [base](int x) { return base + x; };
    CHECK_EQ(f(5), 15);

    Adder a{100};
    f = a;
    CHECK_EQ(f(1), 101);

    CHECK(static_cast<bool>(f));
    CHECK_EQ(apply_twice(f, 1), 201);   // 100+(100+1)
    f = nullptr;
    CHECK(!f);
}

TEST("functional/function copy owns its own capture") {
    std::string big(40, 'x');                 // 超过常见小对象缓冲，走堆分配
    std::function<int(int)> f = [big](int x) { return (int)big.size() + x; };
    std::function<int(int)> g = f;            // 深拷贝：两者互不影响
    CHECK_EQ(f(0), 40);
    CHECK_EQ(g(5), 45);

    std::function<int(int)> h;
    h = g;
    CHECK_EQ(h(1), 41);
    h = nullptr;
    CHECK_EQ(g(2), 42);                       // g 不受 h 释放影响

    std::function<int(int)> empty;
    CHECK_THROWS(empty(1), std::bad_function_call);
}

TEST("functional/reference_wrapper") {
    int x = 1;
    std::reference_wrapper<int> r = std::ref(x);
    r.get() = 42;
    CHECK_EQ(x, 42);

    int y = 2;
    std::reference_wrapper<int> r2 = std::ref(y);
    r2 = r;
    r2.get() = 7;
    CHECK_EQ(x, 7);

    const int cx = 9;
    CHECK_EQ(std::cref(cx).get(), 9);
}

TEST("functional/hash consistency") {
    std::hash<int> hi;
    CHECK_EQ(hi(42), hi(42));
    CHECK_NE(hi(1), hi(2));

    std::hash<long> hl;
    CHECK_EQ(hl(123456789L), hl(123456789L));

    std::hash<std::string> hs;
    CHECK_EQ(hs(std::string("abc")), hs(std::string("abc")));
    CHECK_NE(hs(std::string("abc")), hs(std::string("abd")));
}

TEST("tuple/construction and get") {
    std::tuple<int, double, char> t(1, 2.5, 'x');
    CHECK_EQ(std::get<0>(t), 1);
    CHECK_EQ(std::get<1>(t), 2.5);
    CHECK_EQ(std::get<2>(t), 'x');

    CHECK_EQ((std::tuple_size<std::tuple<int, double, char> >::value), (std::size_t)3);

    std::tuple<int, int> a(1, 2);
    std::tuple<int, int> b(1, 3);
    CHECK(a < b);
    CHECK(a != b);
    CHECK(a == std::make_tuple(1, 2));
}

TEST("tuple/tie and apply") {
    int x = 0;
    double y = 0;
    std::tuple<int, double> src(7, 1.5);
    std::tie(x, y) = src;
    CHECK_EQ(x, 7);
    CHECK_EQ(y, 1.5);

    int sum = std::apply([](int a, int b) { return a + b; }, std::make_tuple(3, 4));
    CHECK_EQ(sum, 7);
}

TEST("utility/index_sequence and exchange") {
    int a = 1;
    int b = std::exchange(a, 2);
    CHECK_EQ(b, 1);
    CHECK_EQ(a, 2);

    std::pair<int, std::string> p(1, std::string("x"));
    CHECK_EQ(p.first, 1);
    CHECK_EQ(p.second, std::string("x"));

    std::pair<int, int> q = std::make_pair(3, 4);
    CHECK_EQ(q.second, 4);
}
