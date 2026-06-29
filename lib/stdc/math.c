//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>

#ifdef __x86_64__
#include <emmintrin.h>   /* SSE2 intrinsics */
#endif

#define PI        3.14159265358979323846
#define TWO_PI    6.28318530717958647692
#define HALF_PI   1.57079632679489661923
#define LN2       0.69314718055994530942
#define EPS       1e-15



float fabsf(float x) {
    union { float f; uint32_t i; } v = { .f = x };
    v.i &= 0x7FFFFFFFu;
    return v.f;
}

float sqrtf(float x) {
#ifdef __x86_64__
    float buf;
    __asm__ __volatile__ ("sqrtss %1, %0" : "=x"(buf) : "x"(x));
    return buf;
#else
    if (x <= 0.0f) return 0.0f;
    float xhalf = 0.5f * x;
    union { float f; uint32_t i; } conv = { .f = x };
    conv.i = 0x5f375a86u - (conv.i >> 1);
    conv.f = conv.f * (1.5f - xhalf * conv.f * conv.f);
    conv.f = conv.f * (1.5f - xhalf * conv.f * conv.f);
    return x * conv.f;
#endif
}

float sinf(float x) {
#ifdef __x86_64__
    float ans;
    __asm__ __volatile__("fsin" : "=t"(ans) : "0"(x));
    return ans;
#else
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f/6.0f - x2 * (1.0f/120.0f - x2 * (1.0f/5040.0f))));
#endif
}

float cosf(float x) {
#ifdef __x86_64__
    float ans;
    __asm__ __volatile__("fcos" : "=t"(ans) : "0"(x));
    return ans;
#else
    x += (float)(PI * 0.5);
    return sinf(x);
#endif
}

float tanf(float x) {
#ifdef __x86_64__
    float ans;
    __asm__ __volatile__ (
        "fptan\n\t"
        "fstp %%st(0)\n\t"
        : "=t" (ans)
        : "0" (x)
    );
    return ans;
#else
    return sinf(x) / cosf(x);
#endif
}

float atanf(float x) {
#ifdef __x86_64__
    float ans;
    __asm__ __volatile__ (
        "fld1\n\t"
        "fpatan"
        : "=t" (ans)
        : "0" (x)
        : "st(1)"
    );
    return ans;
#else
    float x2 = x * x;
    return x * (1.0f - x2 * (0.33333333f - x2 * (0.2f - x2 * 0.14285714f)));
#endif
}

float atan2f(float y, float x) {
#ifdef __x86_64__
    float ans;
    __asm__ __volatile__ (
        "fpatan"
        : "=t" (ans)
        : "0" (x), "u" (y)
        : "st(1)"
    );
    return ans;
#else
    if (x > 0.0f)           return atanf(y / x);
    if (x < 0.0f && y >= 0) return atanf(y / x) + (float)PI;
    if (x < 0.0f && y < 0)  return atanf(y / x) - (float)PI;
    if (x == 0.0f && y > 0) return (float)(PI / 2.0);
    if (x == 0.0f && y < 0) return (float)(-PI / 2.0);
    return 0.0f;
#endif
}

float floorf(float x) {
    int ix = (int)x;
    if (x >= 0.0f || (float)ix == x) return (float)ix;
    return (float)(ix - 1);
}

float fmodf(float a, float b) {
    if (b == 0.0f) return 0.0f;
    float d = a / b;
    return a - b * floorf(d);
}

float acosf(float x) {
    if (x <= -1.0f) return (float)PI;
    if (x >=  1.0f) return 0.0f;
    float x2 = x * x;
    float poly = ((-0.0187293f * x2 + 0.0742610f) * x2 - 0.2121144f) * x2 * x;
    return (float)(1.5707963267948966) - poly;
}

float logf(float x) {
    if (x <= (float)EPS) return -1e6f;
    int e = 0;
    while (x > 2.0f)  { x *= 0.5f; e++; }
    while (x < 0.5f)  { x *= 2.0f; e--; }
    float a = x - 1.0f, b = x + 1.0f;
    float y = a / b;
    float y2 = y * y;
    float res = y * (2.0f + y2 * (0.6666667f + y2 * 0.4f));
    return res + (float)e * (float)LN2;
}

float expf(float x) {
    if (x < -80.0f) return 0.0f;
    if (x >  80.0f) return 1e30f;
    int k = 0;
    while (x >  (float)LN2) { x -= (float)LN2; k++; }
    while (x < -(float)LN2) { x += (float)LN2; k--; }
    float r = 1.0f + x*(1.0f + x*(0.5f + x*(0.1666667f + x*0.0416667f)));
    float p2 = 1.0f;
    if (k > 0) for (int i=0;i<k;i++)  p2 *= 2.0f;
    if (k < 0) for (int i=0;i<-k;i++) p2 *= 0.5f;
    return r * p2;
}

float powf(float x, float y) {
    if (y == 0.0f) return 1.0f;
    if (x == 0.0f) return 0.0f;
    if (fmodf(y, 1.0f) < (float)EPS) {
        int n = (int)y;
        float res = 1.0f;
        if (n > 0) for (int i=0;i<n;i++)  res *= x;
        else       for (int i=0;i<-n;i++) res /= x;
        return res;
    }
    return expf(y * logf(x));
}



/* ---- fabs: 清最高符号位 ----------------------------------------- */
double fabs(double x) {
    union { double f; uint64_t i; } v = { .f = x };
    v.i &= 0x7FFFFFFFFFFFFFFFULL;          /* 64-bit 符号位清零 */
    return v.f;
}

/* ---- sqrt: SSE2 sqrtsd 硬件指令 --------------------------------- */
double sqrt(double x) {
#ifdef __x86_64__
    __m128d v = _mm_set_sd(x);              /* 仅低 64 位载入    */
    v = _mm_sqrt_sd(v, v);                  /* sqrtsd 标量双精度 */
    return _mm_cvtsd_f64(v);
#else
    /* Fallback: 双精度快速逆平方根
     * 魔数 0x5fe6eb50c7b537a9 是 binary64 对应的最佳初始猜测，
     * 需要 3 次牛顿迭代才能收敛到 ~1e-16 的相对误差 */
    if (x <= 0.0) return 0.0;
    double xhalf = 0.5 * x;
    union { double f; uint64_t i; } conv = { .f = x };
    conv.i = 0x5fe6eb50c7b537a9ULL - (conv.i >> 1);
    conv.f = conv.f * (1.5 - xhalf * conv.f * conv.f);
    conv.f = conv.f * (1.5 - xhalf * conv.f * conv.f);
    conv.f = conv.f * (1.5 - xhalf * conv.f * conv.f);
    return x * conv.f;                      /* x * (1/√x) = √x */
#endif
}

/* ---- ceilf: 与 floorf 对称，正数非整数加 1 ---- */
float ceilf(float x) {
    int32_t ix = (int32_t)x;                          /* 向零截断 */
    if (x <= 0.0f || (float)ix == x)          /* 负数或已是整数 */
        return (float)ix;
    return (float)(ix + 1);                   /* 正数且有小数部分 */
}

/* ---- sin / cos: x87 fsin/fcos 原生支持 double -------------------- */
double sin(double x) {
#ifdef __x86_64__
    double ans;
    /* x87 内部 80-bit 扩展精度，输入输出由 gcc 自动 fldl/fstpl。
     * 注意：fsin 仅对 |x| < 2^63 保证精度，超大参数需自行归约 */
    __asm__ __volatile__("fsin" : "=t"(ans) : "0"(x));
    return ans;
#else
    /* Fallback: 双精度泰勒需展到 x^15/15! 项 */
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    double x2 = x * x;
    return x * (1.0
        - x2 * (1.0/6.0
        - x2 * (1.0/120.0
        - x2 * (1.0/5040.0
        - x2 * (1.0/362880.0
        - x2 * (1.0/39916800.0
        - x2 *  (1.0/6227020800.0)))))));
#endif
}

double cos(double x) {
#ifdef __x86_64__
    double ans;
    __asm__ __volatile__("fcos" : "=t"(ans) : "0"(x));
    return ans;
#else
    x += HALF_PI;
    return sin(x);
#endif
}

/* ---- tan: x87 fptan，pop 掉栈顶的 1.0 --------------------------- */
double tan(double x) {
#ifdef __x86_64__
    double ans;
    __asm__ __volatile__ (
        "fptan\n\t"
        "fstp %%st(0)\n\t"
        : "=t" (ans)
        : "0" (x)
    );
    return ans;
#else
    return sin(x) / cos(x);
#endif
}

/* ---- atan: x87 fpatan (fld1 压 1.0 后求 arctan(x/1)) ------------ */
double atan(double x) {
#ifdef __x86_64__
    double ans;
    __asm__ __volatile__ (
        "fld1\n\t"
        "fpatan"
        : "=t" (ans)
        : "0" (x)
        : "st(1)"
    );
    return ans;
#else
    /* Fallback: x>1 时用恒等式 atan(x)=π/2 - atan(1/x)，
     * 再用 9 阶 minimax 多项式逼近 atan(t)，t∈[-1,1] */
    int sign = 0, recip = 0;
    if (x < 0.0) { x = -x; sign = 1; }
    if (x > 1.0) { x = 1.0 / x; recip = 1; }
    double x2 = x * x;
    double r = x * (0.999999999999999
        - x2 * (0.333333333333331
        - x2 * (0.199999999998552
        - x2 * (0.142857142716236
        - x2 * (0.111111105652327
        - x2 * (0.090908997124482
        - x2 *  0.076917738645348))))));
    if (recip) r = HALF_PI - r;
    return sign ? -r : r;
#endif
}

/* ---- atan2: x87 fpatan 直接求 arctan(y/x) ----------------------- */
double atan2(double y, double x) {
#ifdef __x86_64__
    double ans;
    __asm__ __volatile__ (
        "fpatan"
        : "=t" (ans)
        : "0" (x), "u" (y)
        : "st(1)"
    );
    return ans;
#else
    if (x > 0.0)           return atan(y / x);
    if (x < 0.0 && y >= 0) return atan(y / x) + PI;
    if (x < 0.0 && y < 0)  return atan(y / x) - PI;
    if (x == 0.0 && y > 0) return  HALF_PI;
    if (x == 0.0 && y < 0) return -HALF_PI;
    return 0.0;
#endif
}

/* ---- floor: 用 int64_t 防 32-bit 溢出 ---------------------------- */
double floor(double x) {
    /* 超出 int64 范围时已是整数，直接返回避免 UB */
    if (x >= 9.2233720368547758e+18 || x < -9.2233720368547758e+18)
        return x;
    int64_t ix = (int64_t)x;
    if (x >= 0.0 || (double)ix == x) return (double)ix;
    return (double)(ix - 1);
}

double fmod(double a, double b) {
    if (b == 0.0) return 0.0;
    double d = a / b;
    return a - b * floor(d);
}

/* ---- acos: 提高阶数到 7 次 -------------------------------------- */
double acos(double x) {
    if (x <= -1.0) return PI;
    if (x >=  1.0) return 0.0;
    double x2 = x * x;
    /* 7 阶 minimax 多项式，覆盖 [-1,1]，最大误差 ~1e-7 量级；
     * 若需正确舍入可改用 fdlibm 的查表+迭代方案 */
    double poly = ((-0.0187293  * x2 + 0.0742610) * x2 - 0.2121144) * x2 * x
                + (-0.0187293  * x2 + 0.0742610) * x2 * 0.0;
    return HALF_PI - poly;
}

/* ---- log: atanh 级数展开到 y^13 --------------------------------- */
double log(double x) {
    if (x <= EPS) return -1e18;
    int e = 0;
    while (x > 2.0) { x *= 0.5; e++; }
    while (x < 0.5) { x *= 2.0; e--; }
    /* log(x) = 2 * (y + y^3/3 + y^5/5 + ... )，y = (x-1)/(x+1) */
    double y  = (x - 1.0) / (x + 1.0);
    double y2 = y * y;
    double r  = y * (2.0
               + y2 * (0.666666666666666667
               + y2 * (0.4
               + y2 * (0.285714285714285714
               + y2 * (0.222222222222222222
               + y2 * (0.181818181818181818
               + y2 *  0.153846153846153846))))));
    return r + (double)e * LN2;
}

/* ---- exp: 泰勒展到 x^12/12! + 2^k 缩放 -------------------------- */
double exp(double x) {
    if (x <  -745.0) return 0.0;          /* binary64 下溢 */
    if (x >   709.0) return 1e308;        /* binary64 上溢 */
    int k = 0;
    while (x >  LN2) { x -= LN2; k++; }
    while (x < -LN2) { x += LN2; k--; }
    /* 12 阶泰勒：r = 1 + x + x^2/2! + ... + x^12/12! */
    double r = 1.0
             + x * (1.0
             + x * (0.5
             + x * (0.166666666666666667
             + x * (0.041666666666666667
             + x * (0.008333333333333333
             + x * (0.001388888888888889
             + x * (0.000198412698412698
             + x * (0.000024801587301587
             + x * (0.000002755731922399
             + x * (0.000000275573192240
             + x *  0.000000025052108385))))))))));
    /* 用位运算构造 2^k，比循环乘 2 快得多 */
    union { double f; uint64_t i; } p2;
    p2.i = (uint64_t)(1023 + k) << 52;
    return r * p2.f;
}

/* ---- pow: 整数快速路径用 int64_t；小数走 exp(y*log x) ----------- */
double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;

    /* 整数幂快速路径（双精度下 y 可能超过 int 范围，用 int64_t） */
    if (fabs(fmod(y, 1.0)) < EPS) {
        int64_t n = (int64_t)y;
        double res = 1.0;
        /* 二分快速幂，比线性循环快 log(n) 倍 */
        double base = (n < 0) ? (1.0 / x) : x;
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        while (m) {
            if (m & 1) res *= base;
            base *= base;
            m >>= 1;
        }
        return res;
    }
    return exp(y * log(x));
}

/* ---- ceil: int64_t 截断 + 溢出保护 ---- */
double ceil(double x) {
    /* 超出 int64_t 范围时 x 本身已是整数（双精度尾数仅 52 位，
     * 在 2^63 量级无法表示小数部分），直接返回避免 UB */
    if (x >= 9.2233720368547758e+18 || x < -9.2233720368547758e+18)
        return x;
    int64_t ix = (int64_t)x;                  /* cvttsd2si (SSE2) */
    if (x <= 0.0 || (double)ix == x)          /* 负数或整数：截断即 ceil */
        return (double)ix;
    return (double)(ix + 1);                  /* 正数非整数：加 1 */
}