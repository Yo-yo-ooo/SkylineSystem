//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>

#define PI 3.14159265358979323846f
#define TWO_PI 6.28318530717958647692f

// 绝对值：直接清空 IEEE 754 浮点数的符号位 (最高位)
float fabs(float x) {
    union { float f; uint32_t i; } v = { .f = x };
    v.i &= 0x7FFFFFFF; 
    return v.f;
}


float sqrt(float x) {
#ifdef __x86_64__
    float buf;
    // 使用 SSE 硬件指令，这是现代 x86_64 最快最准的开方方式
    __asm__ __volatile__ ("sqrtss %1, %0" : "=x"(buf) : "x"(x));
    return buf;
#else
    if (x <= 0.0f) return 0.0f;
    
    // Fallback: 传奇的 Fast Inverse Square Root
    float xhalf = 0.5f * x;
    union { float f; uint32_t i; } conv = { .f = x };
    
    conv.i = 0x5f375a86 - (conv.i >> 1); // 神奇常数
    
    // 牛顿迭代 (Newton-Raphson)
    conv.f = conv.f * (1.5f - xhalf * conv.f * conv.f); // 第一次迭代
    conv.f = conv.f * (1.5f - xhalf * conv.f * conv.f); // 第二次迭代：大幅提升标准精度
    
    return x * conv.f; // (x * 1/√x) = √x
#endif
}


float sin(float x) {
#ifdef __x86_64__
    float ans;
    // 使用 x87 硬件指令
    __asm__ __volatile__("fsin" : "=t"(ans) : "0"(x));
    return ans;
#else
    // Fallback: 泰勒多项式展开逼近 (Taylor Series)
    // 先将输入限制在 [-PI, PI] 范围内
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;

    float x2 = x * x;
    // sin(x) ≈ x - x^3/3! + x^5/5! - x^7/7!
    return x * (1.0f - x2 * (1.0f/6.0f - x2 * (1.0f/120.0f - x2 * (1.0f/5040.0f))));
#endif
}

float cos(float x) {
#ifdef __x86_64__
    float ans;
    __asm__ __volatile__("fcos" : "=t"(ans) : "0"(x));
    return ans;
#else
    // Fallback: 通过平移利用 sin 的实现
    x += PI * 0.5f;
    return sin(x); 
#endif
}

float tan(float x) {
#ifdef __x86_64__
    float ans;
    // fptan 指令将 1.0 压入 FPU 栈顶，将 tan(x) 压入 ST(1)
    // 我们需要通过 fstp 丢弃栈顶的 1.0，从而拿到结果
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


// 反正切 atan(x) -> 返回范围 [-PI/2, PI/2]
float atan(float x) {
#ifdef __x86_64__
    float ans;
    // fpatan 计算 arctan(ST(1)/ST(0))
    // 我们将 x 放 ST(0)，然后利用 fld1 把 1.0 压栈。
    // 此时 ST(0)=1.0, ST(1)=x，fpatan 执行后算出 arctan(x/1.0) 并存在栈顶
    __asm__ __volatile__ (
        "fld1\n\t"
        "fpatan"
        : "=t" (ans)
        : "0" (x)
        : "st(1)"
    );
    return ans;
#else
    // Fallback: 简单的泰勒级数逼近 (仅适用于 -1 <= x <= 1，供参考，实际可扩展)
    // atan(x) ≈ x - x^3/3 + x^5/5 - x^7/7
    float x2 = x * x;
    return x * (1.0f - x2 * (0.33333333f - x2 * (0.2f - x2 * 0.14285714f)));
#endif
}


float atan2(float y, float x) {
#ifdef __x86_64__
    float ans;
    // 直接利用硬件 fpatan 计算 arctan(y/x)
    __asm__ __volatile__ (
        "fpatan"
        : "=t" (ans)
        : "0" (x), "u" (y) // x 入 ST(0), y 入 ST(1)
        : "st(1)"
    );
    return ans;
#else
    // Fallback (标准 C 库逻辑)
    if (x > 0.0f) return atan(y / x);
    if (x < 0.0f && y >= 0.0f) return atan(y / x) + PI;
    if (x < 0.0f && y < 0.0f)  return atan(y / x) - PI;
    if (x == 0.0f && y > 0.0f) return PI / 2.0f;
    if (x == 0.0f && y < 0.0f) return -PI / 2.0f;
    return 0.0f; // undefined: x=0, y=0
#endif
}