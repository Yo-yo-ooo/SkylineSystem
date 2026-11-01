//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_string.cpp — basic_string / string_view
//
// 重点：
//   * SSO：短串不上堆（旧实现空串就要一次 malloc）
//   * c_str() 恒定 NUL 结尾，data() 与 size() 一致
//   * 自引用 append/insert/replace 不因扩容而读到已释放内存
//   * 查找族与 std::string 语义一致（边界处 npos）
//   * 数值双向转换
//=============================================================
#include "framework/test_framework.h"
#include <string>
#include <string_view>
#include <stdexcept>

TEST("string/SSO avoids heap for short text") {
    CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)0);
    {
        std::string s("hello");
        CHECK_EQ(s.size(), (std::size_t)5);
        CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)0);   // 短串必须在栈内
        CHECK_EQ(std::string(s.c_str()), s);
    }
    CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)0);

    {
        std::string s("0123456789abcdef");                     // 16 > 15，上堆
        CHECK_EQ(s.size(), (std::size_t)16);
        CHECK(tst::alloc_live_blocks() >= 1);
        CHECK_EQ(s[0], '0');
        CHECK_EQ(s[15], 'f');
    }
    CHECK_EQ(tst::alloc_live_blocks(), (std::size_t)0);
}

TEST("string/construction and c_str") {
    std::string a;
    CHECK(a.empty());
    CHECK_EQ(a.size(), (std::size_t)0);
    CHECK_EQ(a.c_str()[0], '\0');

    std::string b("abc");
    CHECK_EQ(b.size(), (std::size_t)3);
    CHECK_EQ(b.c_str()[3], '\0');

    std::string c("abcdef", 3);
    CHECK_EQ(c, std::string("abc"));

    std::string d((std::size_t)4, 'x');
    CHECK_EQ(d, std::string("xxxx"));

    std::string e(b);
    CHECK_EQ(e, b);

    std::string f(b, 1, 2);
    CHECK_EQ(f, std::string("bc"));

    std::string g{'x', 'y', 'z'};
    CHECK_EQ(g, std::string("xyz"));
}

TEST("string/append and self-append") {
    std::string s("abc");
    s.append("def");
    CHECK_EQ(s, std::string("abcdef"));

    s.append((std::size_t)2, '!');
    CHECK_EQ(s, std::string("abcdef!!"));

    // 自引用：源就在自己缓冲区里，且会触发扩容
    std::string t("0123456789");
    t.append(t);
    CHECK_EQ(t.size(), (std::size_t)20);
    CHECK_EQ(t, std::string("01234567890123456789"));

    std::string u("ab");
    u += u;
    CHECK_EQ(u, std::string("abab"));

    u.push_back('c');
    CHECK_EQ(u.back(), 'c');
    u.pop_back();
    CHECK_EQ(u.size(), (std::size_t)4);
}

TEST("string/insert erase replace") {
    std::string s("hello world");
    s.insert((std::size_t)5, ",");
    CHECK_EQ(s, std::string("hello, world"));

    s.erase((std::size_t)5, 1);
    CHECK_EQ(s, std::string("hello world"));

    s.replace((std::size_t)6, 5, "there");
    CHECK_EQ(s, std::string("hello there"));

    s.insert(s.begin(), '[');
    CHECK_EQ(s.front(), '[');
    s.erase(s.begin());
    CHECK_EQ(s.front(), 'h');

    // 自引用插入：s 的第 0 个元素插到开头
    std::string t("xy");
    t.insert(t.begin(), t[0]);
    CHECK_EQ(t, std::string("xxy"));

    std::string u("abc");
    u.insert((std::size_t)1, u);
    CHECK_EQ(u, std::string("aabcbc"));
}

TEST("string/find family") {
    std::string s("abcabcabc");
    CHECK_EQ(s.find("abc"), (std::size_t)0);
    CHECK_EQ(s.find("abc", 1), (std::size_t)3);
    CHECK_EQ(s.find('b'), (std::size_t)1);
    CHECK_EQ(s.find("zzz"), std::string::npos);

    CHECK_EQ(s.rfind("abc"), (std::size_t)6);
    CHECK_EQ(s.rfind('a'), (std::size_t)6);
    CHECK_EQ(std::string("abc").rfind("b"), (std::size_t)1);

    CHECK_EQ(s.find_first_of("cb"), (std::size_t)1);
    CHECK_EQ(s.find_last_of("ab"), (std::size_t)7);
    CHECK_EQ(s.find_first_not_of("ab"), (std::size_t)2);
    CHECK_EQ(s.find_last_not_of("bc"), (std::size_t)6);

    std::string empty;
    CHECK_EQ(empty.find(""), (std::size_t)0);
    CHECK_EQ(empty.find('a'), std::string::npos);
}

TEST("string/compare substr copy") {
    std::string a("abc");
    std::string b("abd");
    CHECK(a < b);
    CHECK(a != b);
    CHECK_EQ(a.compare(b) < 0, true);
    CHECK_EQ(a.compare(std::string("abc")), 0);
    CHECK_EQ(a.compare("abd") < 0, true);

    CHECK_EQ(b.substr(0, 2), std::string("ab"));
    CHECK_EQ(b.substr(1), std::string("bd"));

    char buf[8] = {0};
    std::size_t n = a.copy(buf, 2, 1);
    CHECK_EQ(n, (std::size_t)2);
    CHECK_EQ(buf[0], 'b');
    CHECK_EQ(buf[1], 'c');
}

TEST("string/at and substr throw") {
    std::string s("abc");
    CHECK_EQ(s.at(0), 'a');
    CHECK_THROWS(s.at(3), std::out_of_range);
    CHECK_THROWS(s.substr(4), std::out_of_range);
    CHECK_THROWS(s.erase(9), std::out_of_range);
    CHECK_THROWS(s.insert(9, "x"), std::out_of_range);
}

TEST("string/resize reserve shrink") {
    std::string s("abcdef");
    s.resize(3);
    CHECK_EQ(s, std::string("abc"));
    s.resize(6, 'z');
    CHECK_EQ(s, std::string("abczzz"));

    std::string t("hi");
    t.reserve(64);
    CHECK(t.capacity() >= 64);
    t.append(" there");
    t.shrink_to_fit();
    CHECK_EQ(t, std::string("hi there"));
    CHECK(t.capacity() >= t.size());

    t.clear();
    CHECK(t.empty());
    CHECK_EQ(t.c_str()[0], '\0');
}

TEST("string/concatenation operators") {
    std::string a("foo");
    std::string b("bar");
    CHECK_EQ(a + b, std::string("foobar"));
    CHECK_EQ(a + "bar", std::string("foobar"));
    CHECK_EQ("foo" + b, std::string("foobar"));
    CHECK_EQ(a + '!', std::string("foo!"));
    CHECK(a == "foo");
    CHECK("foo" == a);
}

TEST("string/numeric conversions") {
    CHECK_EQ(std::stoi(std::string("123")), 123);
    CHECK_EQ(std::stoi(std::string("-45")), -45);
    CHECK_EQ(std::stoi(std::string("7f"), nullptr, 16), 127);
    CHECK_EQ(std::stol(std::string("100000")), 100000L);
    // long 宽度随平台不同（LP64=64 位 / LLP64=32 位），越界应按平台抛错
    if (sizeof(long) == 4)
        CHECK_THROWS(std::stol(std::string("2147483648")), std::out_of_range);
    else
        CHECK_EQ(std::stol(std::string("2147483648")), 2147483648L);
    CHECK_EQ(std::stoll(std::string("-9223372036854775808")), (long long)(-9223372036854775807LL - 1));

    std::size_t pos = 0;
    CHECK_EQ(std::stoi(std::string("42abc"), &pos), 42);
    CHECK_EQ(pos, (std::size_t)2);

    CHECK_THROWS(std::stoi(std::string("abc")), std::invalid_argument);
    CHECK_THROWS(std::stoi(std::string("999999999999999999999")), std::out_of_range);

    CHECK_EQ(std::stod(std::string("3.5")), 3.5);
    CHECK_EQ(std::stof(std::string("-1.25")), -1.25f);

    CHECK_EQ(std::to_string(0), std::string("0"));
    CHECK_EQ(std::to_string(-123), std::string("-123"));
    CHECK_EQ(std::to_string(4294967295u), std::string("4294967295"));
    CHECK_EQ(std::to_string((long long)-9223372036854775807LL),
             std::string("-9223372036854775807"));
}

TEST("string/to_string floating") {
    CHECK_EQ(std::to_string(1.5), std::string("1.500000"));
    CHECK_EQ(std::to_string(-2.25), std::string("-2.250000"));
    CHECK_EQ(std::to_string(0.0), std::string("0.000000"));
    CHECK_EQ(std::to_string(1e6), std::string("1000000.000000"));
    CHECK_EQ(std::to_string(3.14159265).substr(0, 4), std::string("3.14"));
}

TEST("string_view basics") {
    std::string_view v("hello world");
    CHECK_EQ(v.size(), (std::size_t)11);
    CHECK_EQ(v.substr(6), std::string_view("world"));
    CHECK(v.starts_with("hello"));
    CHECK(v.ends_with("world"));
    CHECK(v.contains("lo wo"));
    CHECK_EQ(v.find("o"), (std::size_t)4);
    CHECK_EQ(v.rfind("o"), (std::size_t)7);
    CHECK_EQ(v.find_first_of("wo"), (std::size_t)4);

    v.remove_prefix(6);
    CHECK_EQ(v, std::string_view("world"));
    v.remove_suffix(1);
    CHECK_EQ(v, std::string_view("worl"));

    std::string_view empty;
    CHECK(empty.empty());
    CHECK_EQ(empty.find("x"), std::string_view::npos);
}

TEST("string/as container element") {
    std::string a("aaa");
    std::string b("bbb");
    CHECK(a < b);
    CHECK(!(b < a));
    std::string c = b;
    c.swap(a);
    CHECK_EQ(a, std::string("bbb"));
    CHECK_EQ(c, std::string("aaa"));
}

//=============================================================
// 浮点格式化：与 host printf("%.6f") 逐字符对拍
//=============================================================
#include <cstdio>
#include <cstring>

namespace {

int same_as_printf(double v) {
    if (v != v) return 1;                     // NaN 的符号位 printf 会打印 -nan，跳过
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string got = std::to_string(v);
    if (got == std::string(buf)) return 1;
    std::printf("    mismatch: v=%.17g\n      printf=%s\n         ours=%s\n",
                v, buf, got.c_str());
    return 0;
}

} // namespace

TEST("string/to_string double matches printf on edge values") {
    const double vals[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 0.0000005, 0.0000015,
        1e-7, 1e-6, 1e-5, 123456789.123456789, 999999.9999995, 1000000.0,
        1e18, 1e100, 1e308, 1e-100, 1e-308,
        4.9406564584124654e-324,               // 最小次正规数
        2.2250738585072014e-308,               // DBL_MIN
        1.7976931348623157e308                 // DBL_MAX
    };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; ++i) CHECK(same_as_printf(vals[i]));
}

TEST("string/to_string double matches printf on random bit patterns") {
    // 确定性 xorshift，覆盖正负、大小、次正规
    unsigned long long s = 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < 3000; ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        double v;
        std::memcpy(&v, &s, sizeof(v));
        if (v != v) continue;                 // NaN 不比
        CHECK(same_as_printf(v));
    }
}

TEST("string/to_string float matches printf") {
    const float vals[] = {0.0f, -0.0f, 1.5f, -2.25f, 1e-7f, 1e7f, 3.4028235e38f};
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; ++i) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%.6f", (double)vals[i]);
        CHECK_EQ(std::to_string(vals[i]), std::string(buf));
    }
}

TEST("string/to_string special values") {
    const double inf = 1.0 / 0.0;
    CHECK_EQ(std::to_string(inf), std::string("inf"));
    CHECK_EQ(std::to_string(-inf), std::string("-inf"));
    CHECK_EQ(std::to_string(0.0 / 0.0), std::string("nan"));
}

TEST("string/to_string long double matches printf") {
    const long double vals[] = {
        0.0L, 1.0L / 3.0L, -2.0L / 7.0L, 1e300L, 1e-300L,
        1234567890123456789.0L
    };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    for (int i = 0; i < n; ++i) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%.6Lf", vals[i]);
        CHECK_EQ(std::to_string(vals[i]), std::string(buf));
    }
}
