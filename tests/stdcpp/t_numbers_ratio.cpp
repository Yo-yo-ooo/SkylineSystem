//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_numbers_ratio.cpp — <ratio> 与 <numbers>
//=============================================================
#include "framework/test_framework.h"
#include <ratio>
#include <cstdint>

TEST("ratio/normalization") {
    typedef std::ratio<2, 4> r1;
    CHECK_EQ(r1::num, 1);
    CHECK_EQ(r1::den, 2);

    typedef std::ratio<-6, 8> r2;          // 符号归到分子
    CHECK_EQ(r2::num, -3);
    CHECK_EQ(r2::den, 4);

    typedef std::ratio<3, -9> r3;          // 分母为负也要归一
    CHECK_EQ(r3::num, -1);
    CHECK_EQ(r3::den, 3);

    typedef std::ratio<0> zero;
    CHECK_EQ(zero::num, 0);
    CHECK_EQ(zero::den, 1);

    // type 必须是已约分形式
    CHECK((std::is_same<r1::type, std::ratio<1, 2> >::value));
}

TEST("ratio/arithmetic") {
    typedef std::ratio<1, 3> a;
    typedef std::ratio<1, 6> b;

    typedef std::ratio_add<a, b> sum;
    CHECK_EQ(sum::num, 1);
    CHECK_EQ(sum::den, 2);

    typedef std::ratio_subtract<a, b> diff;
    CHECK_EQ(diff::num, 1);
    CHECK_EQ(diff::den, 6);

    typedef std::ratio_multiply<a, b> prod;
    CHECK_EQ(prod::num, 1);
    CHECK_EQ(prod::den, 18);

    typedef std::ratio_divide<a, b> quot;
    CHECK_EQ(quot::num, 2);
    CHECK_EQ(quot::den, 1);

    // 带符号
    typedef std::ratio_subtract<std::ratio<1, 2>, std::ratio<3, 4> > neg;
    CHECK_EQ(neg::num, -1);
    CHECK_EQ(neg::den, 4);
}

TEST("ratio/comparison") {
    typedef std::ratio<1, 3> a;
    typedef std::ratio<1, 2> b;

    CHECK((std::ratio_equal<a, std::ratio<2, 6> >::value));
    CHECK((std::ratio_not_equal<a, b>::value));
    CHECK((std::ratio_less<a, b>::value));
    CHECK((std::ratio_less_equal<a, b>::value));
    CHECK((std::ratio_greater<b, a>::value));
    CHECK((std::ratio_greater_equal<b, a>::value));

    // 负值比较（符号处理容易写错）
    CHECK((std::ratio_less<std::ratio<-1, 2>, std::ratio<1, 3> >::value));
    CHECK((std::ratio_less<std::ratio<-3, 4>, std::ratio<-1, 2> >::value));
}

TEST("ratio/SI prefixes") {
    CHECK_EQ(std::kilo::num, 1000);
    CHECK_EQ(std::kilo::den, 1);
    CHECK_EQ(std::milli::num, 1);
    CHECK_EQ(std::milli::den, 1000);

    // 1 毫秒 × 1000 = 1 秒
    typedef std::ratio_multiply<std::milli, std::ratio<1000> > second;
    CHECK((std::ratio_equal<second, std::ratio<1, 1> >::value));

    // 微 = 千分之一毫
    typedef std::ratio_divide<std::milli, std::kilo> micro2;
    CHECK((std::ratio_equal<micro2, std::micro>::value));
}

//=============================================================
// <numbers>（C++20 起提供）
//=============================================================
#if __cplusplus >= 202002L
#include <numbers>

namespace {

bool close_enough(double a, double b, double eps) {
    double d = a - b;
    if (d < 0) d = -d;
    return d <= eps;
}

} // namespace

TEST("numbers/double constants") {
    CHECK(close_enough(std::numbers::pi, 3.14159265358979323846, 1e-15));
    CHECK(close_enough(std::numbers::e, 2.71828182845904523536, 1e-15));
    CHECK(close_enough(std::numbers::sqrt2, 1.41421356237309504880, 1e-15));
    CHECK(close_enough(std::numbers::ln2, 0.69314718055994530942, 1e-15));
    CHECK(close_enough(std::numbers::phi, 1.61803398874989484820, 1e-15));

    // 恒等式自检：pi * inv_pi == 1
    CHECK(close_enough(std::numbers::pi * std::numbers::inv_pi, 1.0, 1e-15));
    CHECK(close_enough(std::numbers::sqrt2 * std::numbers::sqrt2, 2.0, 1e-15));
    CHECK(close_enough(std::numbers::ln2 * std::numbers::log2e, 1.0, 1e-15));
}

TEST("numbers/float and long double variants") {
    CHECK(close_enough((double)std::numbers::pi_v<float>, 3.14159265, 1e-7));
    CHECK(close_enough((double)std::numbers::e_v<float>, 2.71828183, 1e-7));
    CHECK(close_enough((double)std::numbers::pi_v<long double>, 3.14159265358979323846, 1e-15));

    // 同一常量在 float 与 double 上不应是同一个类型
    CHECK((sizeof(std::numbers::pi_v<float>) == sizeof(float)));
}

TEST("numbers/inline variable has a single address") {
    const double* p1 = &std::numbers::pi;
    const double* p2 = &std::numbers::pi;
    CHECK(p1 == p2);
}
#endif // C++20
