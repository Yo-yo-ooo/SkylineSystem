//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#ifndef _LIBMATH_H_
#define _LIBMATH_H_

#ifdef __cplusplus
extern "C" {
#endif

#define INFINITY __builtin_inf()
#define NAN      __builtin_nan("")

/* ISO C 溢出/错误返回值。HUGE_VALF 必须定义 —— Lua luaconf.h 用
   `defined(HUGE_VAL) && !defined(HUGE_VALF)` 判断是否退化为 C89 配置。 */
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()

int isnan(double x);

float ceilf(float x);
float  sqrtf(float x);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  fabsf(float x);
float  atanf(float x);
float  atan2f(float y, float x);
float  floorf(float x);
float  fmodf(float a, float b);
float  acosf(float x);
float  logf(float x);
float  expf(float x);
float  powf(float x, float y);

double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double fabs(double x);
double atan(double x);
double atan2(double y, double x);
double floor(double x);
double fmod(double a, double b);
double acos(double x);
double log(double x);
double exp(double x);
double pow(double x, double y);
double ceil(double x);

/* ---- 内嵌 Lua 5.4 需要的补充函数 (lib/stdc/math_extra.c) ---- */
double asin(double x);
double log2(double x);
double log10(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);

#ifdef __x86_64__
static inline long double floorl(long double x){
    return (long double)floor((double)x);
}
#endif

#ifdef __cplusplus
}
#endif

#endif