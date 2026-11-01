//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_complex.cpp — <complex>
//
// 数值对照两类基准：
//   * 手算的精确值（(1+2i)(3+4i) = -5+10i 之类）
//   * 编译器内建的 sqrt/sin/cos（等价 libm，但不引入 host C++ 头，
//     避免与标准的 std:: 版本冲突）
//=============================================================
#include "framework/test_framework.h"
#include <complex>

namespace {

bool near(double a, double b, double eps = 1e-12) {
    double d = a - b;
    if (d < 0) d = -d;
    if (d <= eps) return true;
    double m = (b < 0 ? -b : b);
    return d <= eps * (m > 1.0 ? m : 1.0);
}

bool near_c(const std::complex<double>& z, double re, double im, double eps = 1e-12) {
    return near(z.real(), re, eps) && near(z.imag(), im, eps);
}

} // namespace

TEST("complex/construction and accessors") {
    std::complex<double> a;                       // 默认 (0,0)
    CHECK_EQ(a.real(), 0.0);
    CHECK_EQ(a.imag(), 0.0);

    std::complex<double> b(3.0, -4.0);
    CHECK_EQ(b.real(), 3.0);
    CHECK_EQ(b.imag(), -4.0);

    std::complex<double> c(5.0);                  // 隐式转换 -> (5,0)
    CHECK_EQ(c.imag(), 0.0);

    std::complex<float> f(1.5f, 2.5f);
    std::complex<double> d(f);                    // 转换构造
    CHECK_EQ(d.real(), 1.5);
    CHECK_EQ(d.imag(), 2.5);

    d.real(7.0);                                  // C++17 的设值接口
    d.imag(-1.0);
    CHECK_EQ(d.real(), 7.0);
    CHECK_EQ(d.imag(), -1.0);

    CHECK_EQ(std::real(b), 3.0);
    CHECK_EQ(std::imag(b), -4.0);
}

TEST("complex/arithmetic exact values") {
    std::complex<double> a(1, 2), b(3, 4);

    CHECK(near_c(a + b, 4, 6));
    CHECK(near_c(a - b, -2, -2));
    CHECK(near_c(a * b, -5, 10));                 // (1+2i)(3+4i) = 3+4i+6i+8i² = -5+10i
    CHECK(near_c(a / b, 0.44, 0.08));             // (1+2i)(3-4i)/25 = (11+2i)/25

    CHECK(near_c(a + 1.0, 2, 2));
    CHECK(near_c(1.0 + a, 2, 2));
    CHECK(near_c(a - 1.0, 0, 2));
    CHECK(near_c(1.0 - a, 0, -2));
    CHECK(near_c(a * 2.0, 2, 4));
    CHECK(near_c(2.0 * a, 2, 4));
    CHECK(near_c(a / 2.0, 0.5, 1));
    CHECK(near_c(2.0 / a, 0.4, -0.8));

    std::complex<double> c(a);
    c += b;  CHECK(near_c(c, 4, 6));
    c -= b;  CHECK(near_c(c, 1, 2));
    c *= b;  CHECK(near_c(c, -5, 10));
    c /= b;  CHECK(near_c(c, 1, 2));

    CHECK(a == std::complex<double>(1, 2));
    CHECK(a != b);
    CHECK(std::complex<double>(3, 0) == 3.0);
    CHECK(-a == std::complex<double>(-1, -2));
    CHECK(+a == a);
}

TEST("complex/abs norm arg conj") {
    std::complex<double> z(3, 4);
    CHECK_EQ(std::norm(z), 25.0);
    CHECK(near(std::abs(z), 5.0));
    CHECK(near(std::abs(std::complex<double>(0, 0)), 0.0));
    CHECK(near(std::abs(std::complex<double>(-3, -4)), 5.0));

    // 极大/极小值不能因为 re²+im² 溢出而得到 inf / 0
    CHECK(near(std::abs(std::complex<double>(1e200, 1e200)), 1.4142135623730951e200, 1e185));
    CHECK(near(std::abs(std::complex<double>(1e-200, 1e-200)), 1.4142135623730951e-200, 1e-215));

    CHECK(near(std::arg(std::complex<double>(1, 0)), 0.0));
    CHECK(near(std::arg(std::complex<double>(0, 1)), 3.14159265358979323846 / 2));
    CHECK(near(std::arg(std::complex<double>(-1, 0)), 3.14159265358979323846));

    std::complex<double> c = std::conj(z);
    CHECK(near_c(c, 3, -4));
    CHECK(near(std::norm(c), std::norm(z)));
}

TEST("complex/exp log sqrt pow") {
    const double pi = 3.14159265358979323846;

    // e^(iπ) = -1
    CHECK(near_c(std::exp(std::complex<double>(0, pi)), -1, 0, 1e-12));

    // log(-1) = iπ
    CHECK(near_c(std::log(std::complex<double>(-1, 0)), 0, pi, 1e-12));
    CHECK(near_c(std::log(std::complex<double>(1, 0)), 0, 0, 1e-12));

    // sqrt(-1) = i；sqrt(i) = (√2/2)(1+i)
    CHECK(near_c(std::sqrt(std::complex<double>(-1, 0)), 0, 1, 1e-12));
    const double s = 0.70710678118654752440;
    CHECK(near_c(std::sqrt(std::complex<double>(0, 1)), s, s, 1e-12));

    // sqrt 与 abs 的一致性：|sqrt(z)| = sqrt(|z|)
    for (int i = 1; i < 20; ++i) {
        std::complex<double> z(i * 0.7, -i * 1.3);
        CHECK(near(std::abs(std::sqrt(z)), __builtin_sqrt(std::abs(z)), 1e-12));
    }

    // pow：整数次幂
    std::complex<double> i(0, 1);
    CHECK(near_c(std::pow(i, 2), -1, 0, 1e-12));
    CHECK(near_c(std::pow(i, 4), 1, 0, 1e-12));
    CHECK(near_c(std::pow(i, 0), 1, 0, 1e-12));
    CHECK(near_c(std::pow(std::complex<double>(2, 0), -1), 0.5, 0, 1e-12));

    // pow(z, w) = exp(w log z)
    CHECK(near_c(std::pow(std::complex<double>(1, 0), std::complex<double>(5, 3)), 1, 0, 1e-12));

    // log10
    CHECK(near_c(std::log10(std::complex<double>(100, 0)), 2, 0, 1e-12));
}

TEST("complex/trig and hyperbolic") {
    const double pi = 3.14159265358979323846;

    CHECK(near_c(std::sin(std::complex<double>(0, 0)), 0, 0, 1e-15));
    CHECK(near_c(std::sin(std::complex<double>(pi / 2, 0)), 1, 0, 1e-12));
    CHECK(near_c(std::cos(std::complex<double>(0, 0)), 1, 0, 1e-15));
    CHECK(near_c(std::cos(std::complex<double>(pi, 0)), -1, 0, 1e-12));

    // sin(i) = i·sinh(1) = 1.1752011936438014i
    CHECK(near_c(std::sin(std::complex<double>(0, 1)), 0, 1.1752011936438014, 1e-12));
    // cos(i) = cosh(1) = 1.5430806348152437
    CHECK(near_c(std::cos(std::complex<double>(0, 1)), 1.5430806348152437, 0, 1e-12));

    // 与 libm 对照（实参数情况）
    for (int k = 0; k < 10; ++k) {
        double x = k * 0.37 - 1.5;
        CHECK(near(std::sin(std::complex<double>(x, 0)).real(), __builtin_sin(x), 1e-12));
        CHECK(near(std::cos(std::complex<double>(x, 0)).real(), __builtin_cos(x), 1e-12));
    }

    // sinh/cosh 恒等式 cosh² - sinh² = 1
    std::complex<double> z(0.3, -0.7);
    std::complex<double> ch = std::cosh(z), sh = std::sinh(z);
    CHECK(near((ch * ch - sh * sh).real(), 1.0, 1e-12));
    CHECK(near((ch * ch - sh * sh).imag(), 0.0, 1e-12));

    // asin(sin(z)) 在合适范围内应还原
    std::complex<double> w(0.2, 0.1);
    CHECK(near_c(std::asin(std::sin(w)), w.real(), w.imag(), 1e-12));
    CHECK(near_c(std::atan(std::tan(w)), w.real(), w.imag(), 1e-12));
}

using namespace std::literals::complex_literals;

TEST("complex/polar proj literals") {
    const double pi = 3.14159265358979323846;
    std::complex<double> p = std::polar(2.0, pi / 2);
    CHECK(near_c(p, 0, 2, 1e-12));

    std::complex<double> q = std::polar(3.0);     // 缺省 theta = 0
    CHECK(near_c(q, 3, 0, 1e-12));

    std::complex<double> inf(1.0 / 0.0, 2.0);
    std::complex<double> pr = std::proj(inf);
    CHECK(pr.real() > 1e308);                     // 仍是 inf

    std::complex<double> z = 1.0i;                // 字面量
    CHECK(near_c(z, 0, 1, 1e-15));

    std::complex<double> z2 = 3.0i;
    CHECK(near_c(z2, 0, 3, 1e-15));
}

TEST("complex/float and long double instantiations") {
    std::complex<float> a(1.0f, 2.0f), b(3.0f, -1.0f);
    std::complex<float> c = a * b;
    CHECK(near(c.real(), 5.0, 1e-6));
    CHECK(near(c.imag(), 5.0, 1e-6));
    CHECK(near(std::abs(a), __builtin_sqrt(5.0), 1e-6));

    std::complex<long double> la(1.0L, 2.0L);
    CHECK(near((double)std::abs(la), __builtin_sqrt(5.0), 1e-12));
    CHECK(near((double)std::sqrt(la).real(), 1.272019649514069, 1e-12));
}
