#pragma once

#ifndef _DEPEND_FLOATX_H
#define _DEPEND_FLOATX_H

#include <klib/klib.h>
#include <stdint.h>
#include <stddef.h>

namespace FLOATX_DEPEND {
    template<size_t N>
    class bitset {
    public:
        bitset() {
            //bits.resize((N / 32) + 1, 0); // 初始化位图
            bits = (int32_t*)kmalloc((N / 32) + 1);
        }

        void set(size_t pos) {
            ASSERT(pos < N);
            size_t i = pos / 32;
            size_t j = pos % 32;
            bits[i] |= (1 << j); // 设置指定位为1
        }

        void reset(size_t pos) {
            ASSERT(pos < N);
            size_t i = pos / 32;
            size_t j = pos % 32;
            bits[i] &= ~(1 << j); // 清除指定位
        }

        void flip(size_t pos) {
            ASSERT(pos < N);
            size_t i = pos / 32;
            size_t j = pos % 32;
            bits[i] ^= (1 << j); // 翻转指定位
        }

        bool test(size_t pos) const {
            ASSERT(pos < N);
            size_t i = pos / 32;
            size_t j = pos % 32;
            return bits[i] & (1 << j); // 检查指定位是否为1
        }

        void print() const {
            for (size_t i = 0; i < N; ++i) {
                
                kinfo("%d",this->test(i));
                if ((i + 1) % 8 == 0) kprintf(" "); // 每8位分隔
            }
            kinfoln();
        }

    private:
        int32_t* bits; // 存储位图的整数数组
    };

    template <bool _Test, class _Ty = void>
    struct enable_if {}; // no member "type" when !_Test

    template <class _Ty>
    struct enable_if<true, _Ty> { // type is _Ty for _Test
        using type = _Ty;
    };

    template <class _Ty, _Ty _Val>
    struct integral_constant {
        static constexpr _Ty value = _Val;

        using value_type = _Ty;
        using type = integral_constant;

        constexpr operator value_type() const noexcept {
            return value;
        }

        [[nodiscard]] constexpr value_type operator()() const noexcept {
            return value;
        }
    };

    _EXPORT_STD template <bool _Val>
        using bool_constant = integral_constant<bool, _Val>;

    template <class _Ty>
    struct is_integral : bool_constant<is_integral_v<_Ty>> {};

    template<bool Test, class T1, class T2>
    struct conditional
    {
        typedef T2 type;
    };
    template<class T1, class T2>
    struct conditional<true, T1, T2>
    {
        typedef T1 type;
    };

    constexpr int32_t max(int32_t x, int32_t y) { return (x > y) ? x : y; }
}

#endif