//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// tests/stdcpp/framework/test_framework.h
//
// 极简测试框架：不依赖任何外部库，也不依赖被测的 stdcpp 容器
// （用 vector 测 vector 会把框架自身的问题伪装成“被测行为”）。
//
//   TEST(name)            注册用例
//   CHECK(expr)           失败后继续，便于一次看到全部问题
//   CHECK_EQ(a, b)        整型会打印左右值，其它类型只打印表达式
//   CHECK_THROWS(expr, T) / CHECK_NOTHROW(expr)
//   CHECK_LEAK_FREE()     当前点无存活分配
//
// 分配器观测（框架替换 ::operator new/delete，stdcpp 容器全部经此分配）：
//   tst::alloc_live_blocks() / alloc_live_bytes()
//   tst::alloc_fail_after(n)  第 n+1 次分配开始抛 bad_alloc
//   tst::alloc_fail_off()
//=============================================================
#ifndef TST_TEST_FRAMEWORK_H
#define TST_TEST_FRAMEWORK_H

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace std {
template<class C, class T, class A> class basic_string;   // 仅前向声明，避免框架强依赖 <string>
}

namespace tst {

typedef void (*test_fn)();

struct test_case {
    const char* name;
    const char* file;
    test_fn     fn;
};

void register_test(const char* name, const char* file, test_fn fn);

// 分配器观测
std::size_t alloc_live_blocks();
std::size_t alloc_live_bytes();
std::size_t alloc_total_calls();
std::size_t alloc_peak_blocks();
void        alloc_reset_peaks();

void alloc_fail_after(long n);   // 0 => 下一次分配立即失败
void alloc_fail_off();
bool alloc_failing();

// 供替换后的 ::operator new 调用（不属于测试用例 API）
void detail_alloc_track(std::size_t bytes, int delta);
void detail_alloc_total();
bool detail_alloc_should_fail();

void fail(const char* file, int line, const char* expr, const char* detail);

int run_all(int argc, char** argv);

namespace detail {

// 失败信息格式化：整型打印数值，其它类型降级为表达式文本
template<class A, class B>
inline const char* eq_fail_text(const A& a, const B& b) {
    static char buf[160];
    if constexpr (std::is_integral<A>::value && std::is_integral<B>::value) {
        std::snprintf(buf, sizeof(buf), "left=%lld right=%lld",
                      (long long)a, (long long)b);
        return buf;
    } else {
        (void)a; (void)b;
        return "values not printable";
    }
}

// 字符串直接打印，便于定位
template<class C, class T, class A>
inline const char* eq_fail_text(const std::basic_string<C, T, A>& a,
                                const std::basic_string<C, T, A>& b) {
    static char buf[512];
    std::snprintf(buf, sizeof(buf), "left=\"%s\" right=\"%s\"", a.c_str(), b.c_str());
    return buf;
}

inline const char* eq_fail_text(const char* a, const char* b) {
    static char buf[320];
    std::snprintf(buf, sizeof(buf), "left=\"%s\" right=\"%s\"",
                  a ? a : "(null)", b ? b : "(null)");
    return buf;
}

inline const char* leak_text(std::size_t blocks) {
    static char buf[96];
    std::snprintf(buf, sizeof(buf), "%zu block(s) still allocated", blocks);
    return buf;
}

} // namespace detail
} // namespace tst

#define TST_CAT2(a, b) a##b
#define TST_CAT(a, b) TST_CAT2(a, b)

//-------------------------------------------------------------
// TEST：静态注册 + 具名函数体
//-------------------------------------------------------------
#define TEST(name)                                                            \
    static void TST_CAT(tst_body_, __LINE__)();                               \
    namespace {                                                               \
    struct TST_CAT(tst_reg_t_, __LINE__) {                                    \
        TST_CAT(tst_reg_t_, __LINE__)() {                                     \
            ::tst::register_test(name, __FILE__, &TST_CAT(tst_body_, __LINE__)); \
        }                                                                     \
    } TST_CAT(tst_reg_, __LINE__);                                            \
    }                                                                         \
    static void TST_CAT(tst_body_, __LINE__)()

//-------------------------------------------------------------
// 断言
//-------------------------------------------------------------
#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) ::tst::fail(__FILE__, __LINE__, #expr, nullptr);          \
    } while (0)

#define CHECK_MSG(expr, msg)                                                  \
    do {                                                                      \
        if (!(expr)) ::tst::fail(__FILE__, __LINE__, #expr, msg);              \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        if (!((a) == (b)))                                                    \
            ::tst::fail(__FILE__, __LINE__, #a " == " #b,                      \
                        ::tst::detail::eq_fail_text(a, b));                    \
    } while (0)

#define CHECK_NE(a, b)                                                        \
    do {                                                                      \
        if (!((a) != (b)))                                                    \
            ::tst::fail(__FILE__, __LINE__, #a " != " #b, nullptr);            \
    } while (0)

#define CHECK_THROWS(expr, ex_type)                                           \
    do {                                                                      \
        bool tst_caught = false;                                              \
        try { (void)(expr); }                                                 \
        catch (const ex_type&) { tst_caught = true; }                         \
        catch (...) {                                                         \
            ::tst::fail(__FILE__, __LINE__, #expr " -> " #ex_type,             \
                        "wrong exception type");                               \
            tst_caught = true;                                                \
        }                                                                     \
        if (!tst_caught)                                                      \
            ::tst::fail(__FILE__, __LINE__, #expr " -> " #ex_type,             \
                        "nothing thrown");                                     \
    } while (0)

#define CHECK_NOTHROW(expr)                                                   \
    do {                                                                      \
        try { (void)(expr); }                                                 \
        catch (...) {                                                         \
            ::tst::fail(__FILE__, __LINE__, #expr " must not throw", nullptr); \
        }                                                                     \
    } while (0)

#define CHECK_LEAK_FREE()                                                     \
    do {                                                                      \
        if (::tst::alloc_live_blocks() != 0)                                  \
            ::tst::fail(__FILE__, __LINE__, "no leaks",                        \
                        ::tst::detail::leak_text(::tst::alloc_live_blocks())); \
    } while (0)

#endif // TST_TEST_FRAMEWORK_H
