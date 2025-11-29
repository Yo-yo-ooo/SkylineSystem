#pragma once

#ifdef __GNUC__
#define PACK( __Declaration__ ) __Declaration__ __attribute__((packed))
#endif

#ifdef _MSC_VER
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#endif

#define elif else if

#define GET_BIT(value,bit) ((value)&(1<<(bit)))    //读取指定位
#define CPL_BIT(value,bit) ((value)^=(1<<(bit)))   //取反指定位

#define SET0_BIT(value,bit) ((value)&=~(1<<(bit))) //把某个位置0
#define SET1_BIT(value,bit) ((value)|= (1<<(bit))) //把某个位置1

#define IGNORE_VALUE(VALUE) (void)VALUE

#ifndef __noinline__
#define __noinline__ __attribute__ ((noinline))
#endif

#define rm_mask(num, mask) ((num) & ((typeof(num))~(mask)))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)