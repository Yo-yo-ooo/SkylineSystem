//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// detail/config.h — stdcpp 全局配置与特性探测
//
// 设计前提（与 SkylineSystem 的构建方式对齐）：
//   * 内核: gnu++17 + -nostdinc -ffreestanding -fno-exceptions -fno-rtti
//   * 用户态: gnu++20 + -nostdinc -nostdlib -fno-threadsafe-statics
// 因此本库不得假设宿主 libc 的存在，也不得无条件使用异常/ RTTI。
//
// 旧实现用 `#define noexcept throw()` / `#define nullptr 0` 兼容 C++98。
// 这种做法会污染关键字，且在 -fno-exceptions 下直接编译失败；本库统一以
// C++17 为基线（项目实际使用的标准），不再做 C++98 兼容层。
//=============================================================
#ifndef STDCPP_DETAIL_CONFIG_H
#define STDCPP_DETAIL_CONFIG_H

//-------------------------------------------------------------
// 基线标准检查
//-------------------------------------------------------------
#if defined(_MSVC_LANG)
#  define STDCPP_CPLUSPLUS _MSVC_LANG
#else
#  define STDCPP_CPLUSPLUS __cplusplus
#endif

#if STDCPP_CPLUSPLUS < 201703L
#  error "stdcpp requires C++17 or later (kernel: -std=gnu++17, user: -std=gnu++20)"
#endif

#define STDCPP_CXX17 1
#if STDCPP_CPLUSPLUS >= 202002L
#  define STDCPP_CXX20 1
#endif

//-------------------------------------------------------------
// 异常可用性
//   __cpp_exceptions 由编译器在开启异常时给出；-fno-exceptions 下未定义。
//   注意 ICC/MSVC 的差异这里不处理，项目只用 GCC/Clang。
//-------------------------------------------------------------
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#  define STDCPP_HAS_EXCEPTIONS 1
#else
#  define STDCPP_HAS_EXCEPTIONS 0
#endif

//-------------------------------------------------------------
// 失败处理（无异常时）
//   STDCPP_FAIL(msg)：可被平台重定义为 Panic()/kerror() 等。
//   默认 __builtin_trap()：freestanding 下零依赖，且不会静默继续执行。
//-------------------------------------------------------------
#if !defined(STDCPP_FAIL)
#  define STDCPP_FAIL(msg) (__builtin_trap())
#endif

//-------------------------------------------------------------
// 内部断言（默认关闭；STDCPP_DEBUG 打开时用于测试期抓不变量）
//-------------------------------------------------------------
#if defined(STDCPP_DEBUG)
#  define STDCPP_ASSERT(cond) \
      ((cond) ? (void)0 : STDCPP_FAIL("stdcpp: assertion failed: " #cond))
#else
#  define STDCPP_ASSERT(cond) ((void)0)
#endif

//-------------------------------------------------------------
// 内联变量 (C++17)
//-------------------------------------------------------------
#define STDCPP_INLINE_VAR inline

//-------------------------------------------------------------
// 前向声明区：避免容器头之间互相包含造成环
//-------------------------------------------------------------
namespace std {

template<class T> class allocator;
template<class T, class Alloc> class vector;

// basic_string 有三个模板参数（CharT, Traits, Alloc），
// 前向声明必须完全一致，否则与 string 头的定义冲突（redeclared with 3
// template parameters）。
template<class CharT> struct char_traits;
template<class CharT, class Traits, class Alloc> class basic_string;
typedef basic_string<char, char_traits<char>, allocator<char> > string;

} // namespace std

#endif // STDCPP_DETAIL_CONFIG_H
