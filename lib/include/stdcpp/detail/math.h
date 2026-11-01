//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// detail/math.h — <complex>/<random> 需要的数学原语
//
// 分两类：
//   1. 自包含实现（不产生外部符号）：开方、绝对值。
//      内核没有 libm，但 std::abs(complex) 这类函数必须能用，所以开方用
//      牛顿迭代 + 位技巧取初值，全程只有四则运算。
//   2. 转发到编译器内建（会落到 libm 符号）：sin/cos/atan2/exp/log/pow 等。
//      用户态 lib/stdc/math.c 已提供这些符号；内核若调用需要自备实现，
//      不调用则不会产生未定义符号（模板只在用到时实例化）。
//=============================================================
#ifndef STDCPP_DETAIL_MATH_H
#define STDCPP_DETAIL_MATH_H

#include <cstddef>
#include <cstdint>
#include "config.h"

namespace std {
namespace __detail {

//=============================================================
// 自包含开方：牛顿迭代
//
// 初值用 IEEE-754 位模式估算（指数右移一位），再迭代若干次。
// 位技巧本身依赖 IEEE 布局，用 __STDC_IEC_559__ 判定；不支持时退化为
// 简单的折半初值（仍然正确，只是多几次迭代）。
//=============================================================
inline float sqrt_nr(float x) {
    if (!(x > 0.0f)) return x;          // 0、负数、NaN 原样返回
#if defined(__STDC_IEC_559__) || defined(__GCC_IEC_559)
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u = (v.u >> 1) + 0x1FC00000u;     // 约等于 2^(e/2)
    float y = v.f;
#else
    float y = x > 1.0f ? x : 1.0f;
#endif
    // 初始误差约 5%，每次迭代精度翻倍：5 次足够 float 的 24 位
    for (int i = 0; i < 5; ++i) y = 0.5f * (y + x / y);
    return y;
}

inline double sqrt_nr(double x) {
    if (!(x > 0.0)) return x;
#if defined(__STDC_IEC_559__) || defined(__GCC_IEC_559)
    union { double d; uint64_t u; } v;
    v.d = x;
    v.u = (v.u >> 1) + (1023ULL << 51);
    double y = v.d;
#else
    double y = x > 1.0 ? x : 1.0;
#endif
    for (int i = 0; i < 6; ++i) y = 0.5 * (y + x / y);
    return y;
}

inline long double sqrt_nr(long double x) {
    if (!(x > 0.0L)) return x;
    long double y = x > 1.0L ? x : 1.0L;
    // long double 的位布局随架构不同（x87 80 位 / IEEE binary128 / 等于 double），
    // 这里不做位技巧，用 2 的幂缩放取一个不太差的初值
    int e = 0;
    while (y > 4.0L) { y *= 0.5L; ++e; }
    while (y < 0.25L) { y *= 2.0L; --e; }
    y = (e & 1) ? y * 1.4142135623730950488L : y;
    for (int i = 0; i < 8; ++i) y = 0.5L * (y + x / y);
    return y;
}

//=============================================================
// 三角函数 / 指数对数：转发编译器内建（落到 libm）
//=============================================================
inline float       m_sin(float x)       { return __builtin_sinf(x); }
inline double      m_sin(double x)      { return __builtin_sin(x); }
inline long double m_sin(long double x) { return __builtin_sinl(x); }

inline float       m_cos(float x)       { return __builtin_cosf(x); }
inline double      m_cos(double x)      { return __builtin_cos(x); }
inline long double m_cos(long double x) { return __builtin_cosl(x); }

inline float       m_tan(float x)       { return __builtin_tanf(x); }
inline double      m_tan(double x)      { return __builtin_tan(x); }
inline long double m_tan(long double x) { return __builtin_tanl(x); }

inline float       m_atan2(float y, float x)       { return __builtin_atan2f(y, x); }
inline double      m_atan2(double y, double x)     { return __builtin_atan2(y, x); }
inline long double m_atan2(long double y, long double x) { return __builtin_atan2l(y, x); }

inline float       m_exp(float x)       { return __builtin_expf(x); }
inline double      m_exp(double x)      { return __builtin_exp(x); }
inline long double m_exp(long double x) { return __builtin_expl(x); }

inline float       m_log(float x)       { return __builtin_logf(x); }
inline double      m_log(double x)      { return __builtin_log(x); }
inline long double m_log(long double x) { return __builtin_logl(x); }

//=============================================================
// 分类与位操作
//=============================================================
inline bool is_nan_f(float x)  { return x != x; }
inline bool is_nan_f(double x) { return x != x; }
inline bool is_nan_f(long double x) { return x != x; }

inline float  m_fabs(float x)  { return __builtin_fabsf(x); }
inline double m_fabs(double x) { return __builtin_fabs(x); }
inline long double m_fabs(long double x) { return __builtin_fabsl(x); }

// 有限性判定：inf 做 inf-inf 得 NaN；先排除本身就是 NaN 的情况
template<class T>
inline bool is_inf_f(T x) {
    if (x != x) return false;                 // NaN
    const T z = x - x;
    return z != z;                            // inf - inf -> NaN
}

template<class T>
inline bool is_finite_f(T x) {
    return !(x != x) && !is_inf_f(x);
}

} // namespace __detail
} // namespace std

#endif // STDCPP_DETAIL_MATH_H
