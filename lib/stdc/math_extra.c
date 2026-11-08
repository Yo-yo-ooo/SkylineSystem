//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * lib/stdc/math_extra.c — 内嵌 Lua 5.4 需要、math.c 里缺少的 C99 math 函数。
 *
 * 覆盖: asin / log2 / log10 / sinh / cosh / tanh / frexp / ldexp / modf。
 *
 * 设计要点:
 *   - 定义域错误: errno = EDOM, 返回 NaN;
 *     极点/上溢错误: errno = ERANGE, 返回 ±HUGE_VAL (下溢返回 ±0),
 *     与 C 标准 / glibc 的行为一致。
 *   - asin 用 atan2(x, sqrt((1-x)*(1+x))): 乘积分式避免 1-x*x 在 |x|→1
 *     时的相消误差; sqrt 走 SSE2 sqrtsd, atan2 走 x87 FPATAN, 精度 ~1 ulp。
 *   - log2/log10 共用自带的对数核: 位分解出 m∈[sqrt(1/2),sqrt(2)) 后
 *     用 atanh 级数 (到 t^26, |t|<=0.1716), 精度 ~1e-16 相对误差。
 *   - sinh/cosh/tanh 用自带 exp/expm1: k=round(x/ln2) 归约 + ln2 hi/lo
 *     双精度常数 + 13 阶 Taylor, 比 math.c 现有 12 阶 exp 更准, 且
 *     tanh/sinh 在小参数处不做 e^x-e^-x 的抵消减法。
 */

#include <math.h>
#include <errno.h>
#include <stdint.h>

#define SKY_PI       3.141592653589793238462643383279502884
#define SKY_LN2      0.693147180559945309417232121458176568
#define SKY_LN10     2.302585092994045684017991454684364208
#define SKY_INV_LN2  1.442695040888963407359924681001892137
#define SKY_INV_LN10 0.434294481903251827651128918916605082
#define SKY_E        2.718281828459045235360287471352662498

/* 1/2^54: 次正规数规格化用 */
#define SKY_TWO54    18014398509481984.0

static int sky_isnan(double x) { return __builtin_isnan(x); }
static int sky_isinf(double x) { return __builtin_isinf(x); }

/* 2^n (仅用于 -1022 <= n <= 1023, 结果一定是正规数) */
static double sky_pow2i(int n) {
    union { double f; uint64_t u; } v;
    v.u = (uint64_t)(1023 + n) << 52;
    return v.f;
}

/* ------------------------------------------------------------------ */
/* 对数核: 返回 ln(x), 调用者保证 x 是有限正数                          */
/* ------------------------------------------------------------------ */
static double sky_log_kernel(double x) {
    union { double f; uint64_t u; } v;
    int e;
    double m, t, t2, p;

    v.f = x;
    e = (int)((v.u >> 52) & 0x7FFu);
    if (e == 0) {                       /* 次正规数: 先放大 2^54 再取指数 */
        v.f = x * SKY_TWO54;
        e = (int)((v.u >> 52) & 0x7FFu) - 54;
    }
    e -= 1023;                          /* 无偏指数: x = m * 2^e, m∈[1,2) */
    v.u = (v.u & 0x000FFFFFFFFFFFFFULL) | (1023ULL << 52);
    m = v.f;
    if (m > 1.4142135623730951) {       /* 收缩到 [sqrt(1/2), sqrt(2)) */
        m *= 0.5;
        e += 1;
    }

    /* ln(m) = 2 * (t + t^3/3 + t^5/5 + ... + t^27/27), t = (m-1)/(m+1)
       |t| <= 0.1716 -> t^27 ~ 2e-21, 截断误差远低于 binary64 舍入 */
    t = (m - 1.0) / (m + 1.0);
    t2 = t * t;
    p = 1.0 / 27.0;
    p = p * t2 + 1.0 / 25.0;
    p = p * t2 + 1.0 / 23.0;
    p = p * t2 + 1.0 / 21.0;
    p = p * t2 + 1.0 / 19.0;
    p = p * t2 + 1.0 / 17.0;
    p = p * t2 + 1.0 / 15.0;
    p = p * t2 + 1.0 / 13.0;
    p = p * t2 + 1.0 / 11.0;
    p = p * t2 + 1.0 / 9.0;
    p = p * t2 + 1.0 / 7.0;
    p = p * t2 + 1.0 / 5.0;
    p = p * t2 + 1.0 / 3.0;
    p = p * t2 + 1.0;

    return 2.0 * t * p + (double)e * SKY_LN2;
}

/* ------------------------------------------------------------------ */
/* exp: k=round(x/ln2) 归约 + Taylor 13 阶                              */
/* ------------------------------------------------------------------ */
static double sky_exp_kernel(double x) {
    int k = (int)(x * SKY_INV_LN2 + (x >= 0.0 ? 0.5 : -0.5));
    double r = x - (double)k * 6.93147180369123816490e-01   /* ln2 hi */
                 - (double)k * 1.90821492927058770002e-10;  /* ln2 lo */
    double p;

    /* exp(r) = sum r^n/n!, |r| <= ln2/2 -> 13 阶足够 (截断 < 1e-17) */
    p = 1.0 / 6227020800.0;        /* 1/13! */
    p = p * r + 1.0 / 479001600.0; /* 1/12! */
    p = p * r + 1.0 / 39916800.0;  /* 1/11! */
    p = p * r + 1.0 / 3628800.0;   /* 1/10! */
    p = p * r + 1.0 / 362880.0;    /* 1/9!  */
    p = p * r + 1.0 / 40320.0;     /* 1/8!  */
    p = p * r + 1.0 / 5040.0;      /* 1/7!  */
    p = p * r + 1.0 / 720.0;       /* 1/6!  */
    p = p * r + 1.0 / 120.0;       /* 1/5!  */
    p = p * r + 1.0 / 24.0;        /* 1/4!  */
    p = p * r + 1.0 / 6.0;         /* 1/3!  */
    p = p * r + 0.5;               /* 1/2!  */
    p = p * r + 1.0;               /* 1/1!  */
    p = p * r + 1.0;               /* 1/0!  */
    return p * sky_pow2i(k);
}

/* exp(x), 处理上溢/下溢边界; x 为有限或 inf 时结果与 IEEE 语义一致 */
static double sky_exp(double x) {
    if (sky_isnan(x)) return x;
    if (x > 709.782712893383973096) return HUGE_VAL;     /* 上溢 */
    if (x < -745.13321910194110842) return 0.0;          /* 下溢到 0 */
    if (x > 700.0) {                                     /* 先把 k 压回可表示范围 */
        return sky_exp_kernel(x - 128.0 * SKY_LN2) * sky_pow2i(128);
    }
    if (x < -700.0) {
        return sky_exp_kernel(x + 128.0 * SKY_LN2) * sky_pow2i(-128);
    }
    return sky_exp_kernel(x);
}

/* exp(x) - 1: 小参数用 Taylor, 避免 1 附近相消 (tanh/sinh 小参数用) */
static double sky_expm1(double x) {
    double p;
    if (sky_isnan(x)) return x;
    if (x > 0.5 || x < -0.5) return sky_exp(x) - 1.0;

    /* expm1(x) = x * (1 + x/2 + x^2/6 + ... + x^12/13!), |x| <= 0.5 */
    p = 1.0 / 6227020800.0;
    p = p * x + 1.0 / 479001600.0;
    p = p * x + 1.0 / 39916800.0;
    p = p * x + 1.0 / 3628800.0;
    p = p * x + 1.0 / 362880.0;
    p = p * x + 1.0 / 40320.0;
    p = p * x + 1.0 / 5040.0;
    p = p * x + 1.0 / 720.0;
    p = p * x + 1.0 / 120.0;
    p = p * x + 1.0 / 24.0;
    p = p * x + 1.0 / 6.0;
    p = p * x + 0.5;
    p = p * x + 1.0;
    return x * p;
}

/* ------------------------------------------------------------------ */
/* asin                                                                 */
/* ------------------------------------------------------------------ */
double asin(double x) {
    if (sky_isnan(x)) return x;
    if (x < -1.0 || x > 1.0) {
        errno = EDOM;
        return NAN;
    }
    if (x == 1.0) return SKY_PI / 2.0;
    if (x == -1.0) return -SKY_PI / 2.0;
    if (x == 0.0) return x;             /* 保留 ±0 符号 */
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

/* ------------------------------------------------------------------ */
/* log2 / log10                                                         */
/* ------------------------------------------------------------------ */
double log2(double x) {
    if (sky_isnan(x)) return x;
    if (x > 0.0) {
        if (x == HUGE_VAL) return x;
        return sky_log_kernel(x) * SKY_INV_LN2;
    }
    if (x == 0.0) {                     /* 极点错误 */
        errno = ERANGE;
        return -HUGE_VAL;
    }
    errno = EDOM;                       /* 定义域错误 */
    return NAN;
}

double log10(double x) {
    if (sky_isnan(x)) return x;
    if (x > 0.0) {
        if (x == HUGE_VAL) return x;
        return sky_log_kernel(x) * SKY_INV_LN10;
    }
    if (x == 0.0) {
        errno = ERANGE;
        return -HUGE_VAL;
    }
    errno = EDOM;
    return NAN;
}

/* ------------------------------------------------------------------ */
/* sinh / cosh / tanh                                                   */
/* ------------------------------------------------------------------ */
double sinh(double x) {
    double ax, r;
    if (sky_isnan(x) || sky_isinf(x)) return x;

    ax = fabs(x);
    if (ax > 700.0) {
        /* sinh(x) = e^x/2 - e^-x/2; ax > 700 时后项可忽略,
           e^x/2 = exp(x - ln2) (直到 ax ~ 710.476 才上溢) */
        r = sky_exp(ax - SKY_LN2);
        if (sky_isinf(r)) {
            errno = ERANGE;
            return (x > 0.0) ? HUGE_VAL : -HUGE_VAL;
        }
        return (x > 0.0) ? r : -r;
    }

    /* 小/中等参数: (expm1(x) - expm1(-x)) / 2, 两项同号相减变相加, 无相消 */
    r = 0.5 * (sky_expm1(x) - sky_expm1(-x));
    return r;
}

double cosh(double x) {
    double ax, r;
    if (sky_isnan(x)) return x;
    if (sky_isinf(x)) return HUGE_VAL;

    ax = fabs(x);
    if (ax > 700.0) {
        r = sky_exp(ax - SKY_LN2);
        if (sky_isinf(r)) {
            errno = ERANGE;
            return HUGE_VAL;
        }
        return r;
    }

    /* cosh(x) = 1 + (expm1(x) + expm1(-x)) / 2 */
    return 1.0 + 0.5 * (sky_expm1(x) + sky_expm1(-x));
}

double tanh(double x) {
    double e2;
    if (sky_isnan(x)) return x;
    if (x > 20.0) return 1.0;           /* 含 +inf */
    if (x < -20.0) return -1.0;         /* 含 -inf */
    if (x == 0.0) return x;             /* 保留 ±0 符号 */

    /* tanh(x) = (e^2x - 1) / (e^2x + 1); expm1 保证 x->0 时精度 */
    e2 = sky_expm1(2.0 * x);
    return e2 / (e2 + 2.0);
}

/* ------------------------------------------------------------------ */
/* frexp / ldexp / modf  (精确按 C 标准: 位级操作)                       */
/* ------------------------------------------------------------------ */
double frexp(double x, int *exp) {
    union { double f; uint64_t u; } v;
    int e;

    if (x == 0.0 || sky_isnan(x) || sky_isinf(x)) {
        if (exp) *exp = 0;
        return x;
    }

    v.f = x;
    e = (int)((v.u >> 52) & 0x7FFu);
    if (e == 0) {                       /* 次正规数: 乘 2^54 拉回正规范围 */
        v.f = x * SKY_TWO54;
        e = (int)((v.u >> 52) & 0x7FFu) - 54;
    }
    /* x = m * 2^(e-1023), m∈[1,2) -> 返回 0.5*m, 指数 e-1022 */
    if (exp) *exp = e - 1022;
    v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | (1022ULL << 52);
    return v.f;
}

double ldexp(double x, int exp) {
    if (x == 0.0 || sky_isnan(x) || sky_isinf(x)) return x;

    /* 分步缩放: 每步最多 2^1023 / 2^-1022, 避免中间结果越界;
       对极端 exp 循环最多几次就会因上溢/下溢收敛 */
    while (exp > 1023) {
        x *= sky_pow2i(1023);
        exp -= 1023;
        if (sky_isinf(x)) {
            errno = ERANGE;
            return x;
        }
    }
    while (exp < -1022) {
        x *= sky_pow2i(-1022);
        exp += 1022;
        if (x == 0.0) {
            errno = ERANGE;
            return x;
        }
    }

    x *= sky_pow2i(exp);
    if (sky_isinf(x)) {
        errno = ERANGE;
    } else if (x == 0.0) {              /* 下溢到 0 */
        errno = ERANGE;
    }
    return x;
}

double modf(double x, double *iptr) {
    double t;

    if (sky_isnan(x) || sky_isinf(x)) {
        *iptr = x;
        return sky_isinf(x) ? (x > 0.0 ? 0.0 : -0.0) : x;
    }
    if (fabs(x) < 1.0) {
        *iptr = (x < 0.0 || (x == 0.0 && 1.0 / x < 0.0)) ? -0.0 : 0.0;
        return x;
    }
    if (fabs(x) >= 4503599627370496.0) { /* 2^52: 双精度已无小数部分 */
        *iptr = x;
        return (x > 0.0) ? 0.0 : -0.0;
    }

    t = (double)(int64_t)x;             /* 向零截断, |x| < 2^52 时精确 */
    *iptr = t;
    return x - t;                       /* 二者同量级, 减法精确 */
}
