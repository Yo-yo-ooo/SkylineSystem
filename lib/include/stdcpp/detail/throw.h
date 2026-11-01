//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// detail/throw.h — 统一抛出/失败策略
//
// 内核以 -fno-exceptions 构建，头文件里任何 `throw` 都会编译失败。因此所有
// 越界/长度错误都必须经由 STDCPP_THROW() 表达：
//   * 开异常:  正常 throw，行为与标准一致（测试可 catch）
//   * 关异常:  走 STDCPP_FAIL()（默认 __builtin_trap），不静默继续
// 注意无异常分支里宏参数不会被求值，所以无需构造异常对象。
//=============================================================
#ifndef STDCPP_DETAIL_THROW_H
#define STDCPP_DETAIL_THROW_H

#include "config.h"

#if STDCPP_HAS_EXCEPTIONS
#  define STDCPP_THROW(ex) (throw (ex))
#else
#  define STDCPP_THROW(ex) (STDCPP_FAIL("stdcpp: exception in no-exceptions build: " #ex))
#endif

//-------------------------------------------------------------
// 异常安全代码的双模式写法
//
// 问题：容器在扩容失败时要“回滚已构造的元素再释放缓冲区”，这必须靠
// try/catch。但 -fno-exceptions 下连 try/catch 都不能出现（编译错误），
// 不只是 throw。于是用宏把清理逻辑包起来：
//
//   STDCPP_TRY { ... } STDCPP_CATCH_ALL { 回滚 } STDCPP_RETHROW;
//
// 开异常时展开为真正的 try/catch/rethrow；关异常时展开为
// `if (true) {...} if (false) {...} (void)0;`，回滚分支不可达但语义清晰。
// 语义差异：无异常构建下 operator new 不会失败返回（内核实现直接停机），
// 所以回滚分支本就不需要。
//-------------------------------------------------------------
#if STDCPP_HAS_EXCEPTIONS
#  define STDCPP_TRY        try
#  define STDCPP_CATCH_ALL  catch (...)
#  define STDCPP_RETHROW    throw
#else
#  define STDCPP_TRY        if (true)
#  define STDCPP_CATCH_ALL  if (false)
#  define STDCPP_RETHROW    ((void)0)
#endif

#endif // STDCPP_DETAIL_THROW_H
