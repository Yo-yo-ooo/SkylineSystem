//=============================================================
// <regex> 测试
//
// 原则：不跟另一个 regex 实现"对拍"（内核里没有第二个实现），
// 而是把 ECMAScript 语义写成显式用例——尤其是几个容易写错的：
//   * 交替必须能回溯到后面的分支（历史上被编译成了拼接）
//   * regex_match 要求整串消耗，且失败时要继续回溯而不是直接 return false
//   * 贪婪/懒惰的差异只在 regex_search 下可见
//   * 空匹配循环（(a*)*）不能死循环
//   * 长输入不能爆栈
//=============================================================
#include "framework/test_framework.h"
#include <regex>
#include <string>
#include <cstdio>

namespace {

bool re_match(const char* pat, const char* text) {
    return std::regex_match(std::string(text), std::regex(pat));
}
bool re_search(const char* pat, const char* text) {
    return std::regex_search(std::string(text), std::regex(pat));
}

// 取出第 n 个捕获组（越界或未参与返回 "<none>"）
std::string group_of(const char* pat, const char* text, std::size_t n) {
    std::smatch m;
    if (!std::regex_match(std::string(text), m, std::regex(pat))) return "<no-match>";
    if (n >= m.size() || !m[n].matched) return "<none>";
    return m[n].str();
}

std::string search_group_of(const char* pat, const char* text, std::size_t n) {
    std::smatch m;
    if (!std::regex_search(std::string(text), m, std::regex(pat))) return "<no-match>";
    if (n >= m.size() || !m[n].matched) return "<none>";
    return m[n].str();
}

} // namespace

//=============================================================
// 基本匹配
//=============================================================

TEST("regex/literals and concatenation") {
    CHECK(re_match("abc", "abc"));
    CHECK(!re_match("abc", "abd"));
    CHECK(!re_match("abc", "ab"));
    CHECK(!re_match("abc", "abcd"));       // regex_match 要求整串消耗
    CHECK(re_match("", ""));
    CHECK(re_match("a", "a"));
}

TEST("regex/case sensitivity and icase") {
    CHECK(!re_match("abc", "ABC"));
    CHECK(std::regex_match(std::string("ABC"),
                           std::regex("abc", std::regex::icase)));
    CHECK(std::regex_match(std::string("AbC"),
                           std::regex("abc", std::regex::icase)));
    CHECK(!std::regex_match(std::string("abd"),
                            std::regex("abc", std::regex::icase)));
}

TEST("regex/regex_match vs regex_search") {
    CHECK(!re_match("bcd", "abcd"));
    CHECK(re_search("bcd", "abcd"));
    CHECK(!re_search("xyz", "abcd"));
}

//=============================================================
// 量词
//=============================================================

TEST("regex/star plus question") {
    CHECK(re_match("ab*", "a"));
    CHECK(re_match("ab*", "abbb"));
    CHECK(!re_match("ab*", "ac"));

    CHECK(!re_match("ab+", "a"));
    CHECK(re_match("ab+", "ab"));
    CHECK(re_match("ab+", "abbb"));

    CHECK(re_match("ab?c", "ac"));
    CHECK(re_match("ab?c", "abc"));
    CHECK(!re_match("ab?c", "abbc"));
}

TEST("regex/braced repetition") {
    CHECK(re_match("a{2}", "aa"));
    CHECK(!re_match("a{2}", "a"));
    CHECK(!re_match("a{2}", "aaa"));

    CHECK(re_match("a{2,3}", "aa"));
    CHECK(re_match("a{2,3}", "aaa"));
    CHECK(!re_match("a{2,3}", "a"));
    CHECK(!re_match("a{2,3}", "aaaa"));

    CHECK(re_match("a{2,}", "aa"));
    CHECK(re_match("a{2,}", "aaaaa"));
    CHECK(!re_match("a{2,}", "a"));

    // 量词作用于分组
    CHECK(re_match("(ab){3}", "ababab"));
    CHECK(!re_match("(ab){3}", "abab"));
}

TEST("regex/greedy vs lazy under regex_search") {
    // 差异只在 search 下可见：match 要求整串消耗，两者都会被迫吃到最后
    CHECK_EQ(search_group_of("<(.+)>", "<a><b>", 1), std::string("a><b"));
    CHECK_EQ(search_group_of("<(.+?)>", "<a><b>", 1), std::string("a"));
    CHECK_EQ(search_group_of("(a+?)b", "aaab", 1), std::string("aaa"));
    CHECK_EQ(search_group_of("(a+?)", "aaa", 1), std::string("a"));
}

TEST("regex/lazy still satisfies full-match requirement") {
    // regex_match 下，懒惰量词也必须能被"拉长"到覆盖整串，
    // 而不是取最短后直接判定失败。这是历史上真实存在的 bug。
    CHECK_EQ(group_of("<(.+?)>", "<a><b>", 1), std::string("a><b"));
    CHECK_EQ(group_of("<(.+)>", "<a><b>", 1), std::string("a><b"));
    CHECK(re_match("a+?b", "aab"));
}

TEST("regex/empty-match loops terminate") {
    // (a*)* 的循环体可以匹配空串；没有空循环保护就会无限递归/死循环
    CHECK(re_match("(a*)*", ""));
    CHECK(re_match("(a*)*", "aaa"));
    CHECK(re_match("(a?)*", "aa"));
    CHECK(re_match("a*", ""));
    CHECK(re_match("(ab|a)*", "aab"));
}

TEST("regex/long input does not overflow the stack") {
    // 匹配引擎是迭代式的（显式栈），不应随输入长度加深调用栈
    std::string s(20000, 'a');
    CHECK(std::regex_match(s, std::regex("a*")));
    CHECK(std::regex_match(s, std::regex("a+")));
    CHECK(!std::regex_match(s, std::regex("a*b")));

    std::string t;
    for (int i = 0; i < 2000; ++i) t += "ab";
    CHECK(std::regex_match(t, std::regex("(ab)*")));
}

//=============================================================
// 交替
//=============================================================

TEST("regex/alternation") {
    CHECK(re_match("cat|dog", "cat"));
    CHECK(re_match("cat|dog", "dog"));
    CHECK(!re_match("cat|dog", "cow"));
    CHECK(!re_match("cat|dog", "catdog"));   // 绝不能被编译成拼接

    CHECK(re_match("a|b|c", "a"));
    CHECK(re_match("a|b|c", "b"));
    CHECK(re_match("a|b|c", "c"));
    CHECK(!re_match("a|b|c", "d"));
}

TEST("regex/alternation with grouping and anchors") {
    CHECK_EQ(group_of("(cat)|(dog)", "dog", 1), std::string("<none>"));
    CHECK_EQ(group_of("(cat)|(dog)", "dog", 2), std::string("dog"));
    CHECK_EQ(group_of("(cat)|(dog)", "cat", 1), std::string("cat"));

    CHECK(re_match("^(ab|cd)$", "ab"));
    CHECK(re_match("^(ab|cd)$", "cd"));
    CHECK(!re_match("^(ab|cd)$", "abcd"));

    // 交替只作用于本层：a(b|c)d
    CHECK(re_match("a(b|c)d", "abd"));
    CHECK(re_match("a(b|c)d", "acd"));
    CHECK(!re_match("a(b|c)d", "ad"));
}

TEST("regex/alternation backtracks into later branches") {
    // 分支 1 能匹配但后半段失败时，必须回溯去试分支 2
    CHECK(re_match("(ab|a)c", "ac"));
    CHECK(re_match("(aa|a)ab", "aab"));
    CHECK(re_match("x(y|xy)z", "xyz"));
}

//=============================================================
// 字符类与转义
//=============================================================

TEST("regex/character classes") {
    CHECK(re_match("[abc]+", "abcba"));
    CHECK(!re_match("[abc]+", "abcd"));
    CHECK(re_match("[a-cX-Z]{6}", "abcXYZ"));
    CHECK(!re_match("[a-cX-Z]{6}", "abcXYd"));

    CHECK(re_match("[^abc]+", "xyz"));
    CHECK(!re_match("[^abc]+", "xyz a"));

    CHECK(re_match("a[.]b", "a.b"));        // 类内 . 是字面量
    CHECK(!re_match("a[.]b", "axb"));
}

TEST("regex/escape shorthands") {
    CHECK(re_match("a\\db", "a1b"));
    CHECK(!re_match("a\\db", "axb"));
    CHECK(re_match("\\w+", "abc_123"));
    CHECK(!re_match("\\w+", "abc 123"));
    CHECK(re_match("a\\sb", "a b"));
    CHECK(re_match("a\\sb", "a\tb"));
    CHECK(!re_match("a\\sb", "ab"));
    CHECK(re_match("\\d{3}-\\d{4}", "010-1234"));
    CHECK(re_match("\\S+", "abc"));
    CHECK(re_match("\\D+", "abc"));
}

TEST("regex/dot excludes newline") {
    CHECK(!re_match("a.b", "a\nb"));
    CHECK(re_match("a.b", "a b"));
    CHECK(!re_search("a.b", "a\nb"));
    CHECK(re_search("a.b", "xa b"));
}

TEST("regex/anchors and word boundaries") {
    CHECK(re_match("^abc$", "abc"));
    CHECK(!re_search("^abc$", "xabc"));
    CHECK(re_search("^abc", "abcdef"));
    CHECK(!re_search("^bcd", "abcdef"));
    CHECK(re_search("def$", "abcdef"));
    CHECK(!re_search("bcd$", "abcdef"));
    CHECK(!re_search("abc$", "abcdef"));

    CHECK(re_search("\\bword\\b", "a word here"));
    CHECK(!re_search("\\bword\\b", "sword"));
    CHECK(!re_search("\\bword\\b", "words"));
    CHECK(re_search("\\Bord", "word"));     // ord 不在词边界上
    CHECK(!re_search("\\Bword", "a word"));
}

//=============================================================
// 分组与反向引用
//=============================================================

TEST("regex/capture groups") {
    CHECK_EQ(group_of("(\\d{4})-(\\d{2})-(\\d{2})", "2024-05-06", 1), std::string("2024"));
    CHECK_EQ(group_of("(\\d{4})-(\\d{2})-(\\d{2})", "2024-05-06", 2), std::string("05"));
    CHECK_EQ(group_of("(\\d{4})-(\\d{2})-(\\d{2})", "2024-05-06", 3), std::string("06"));
    CHECK_EQ(group_of("(\\d{4})-(\\d{2})-(\\d{2})", "2024-05-06", 0), std::string("2024-05-06"));
    CHECK_EQ(group_of("(\\d{4})-(\\d{2})-(\\d{2})", "24-05-06", 1), std::string("<no-match>"));
}

TEST("regex/nested groups") {
    CHECK_EQ(group_of("((a)(b))c", "abc", 1), std::string("ab"));
    CHECK_EQ(group_of("((a)(b))c", "abc", 2), std::string("a"));
    CHECK_EQ(group_of("((a)(b))c", "abc", 3), std::string("b"));
}

TEST("regex/optional group did not participate") {
    // 未参与的组 matched == false，str() 为空
    CHECK_EQ(group_of("a(x)?b", "ab", 1), std::string("<none>"));
    CHECK_EQ(group_of("a(x)?b", "axb", 1), std::string("x"));
}

TEST("regex/backreferences") {
    CHECK(re_match("(ab)\\1", "abab"));
    CHECK(!re_match("(ab)\\1", "abcd"));
    CHECK(re_match("(abc)\\1", "abcabc"));
    CHECK(re_match("(\\w+) \\1", "hi hi"));
    CHECK(!re_match("(\\w+) \\1", "hi ho"));
}

//=============================================================
// match_results / 前缀后缀
//=============================================================

TEST("regex/match_results prefix suffix position") {
    std::string s = "the quick brown";
    std::smatch m;
    bool ok = std::regex_search(s, m, std::regex("quick"));
    CHECK(ok);
    if (ok) {
        CHECK_EQ(m.str(), std::string("quick"));
        CHECK_EQ(m.prefix().str(), std::string("the "));
        CHECK_EQ(m.suffix().str(), std::string(" brown"));
        CHECK_EQ((long)m.position(), (long)4);
        CHECK_EQ((long)m.length(), (long)5);
        CHECK(m.ready());
        CHECK_EQ((std::size_t)m.size(), (std::size_t)1);
    }
}

TEST("regex/failed match leaves results indexable") {
    // 标准说失败后 m 处于"未指定"状态；这里额外保证槽位已填好，
    // 避免调用方索引到未初始化的 sub_match（这在 freestanding 下就是野指针）
    std::smatch m;
    bool ok = std::regex_match(std::string("abc"), m, std::regex("(\\d+)"));
    CHECK(!ok);
    CHECK_EQ(m.size(), (std::size_t)2);
    CHECK(!m[1].matched);
    CHECK_EQ(m[1].str(), std::string(""));
}

//=============================================================
// 替换
//=============================================================

TEST("regex/regex_replace") {
    CHECK_EQ(std::regex_replace(std::string("hello world"), std::regex("world"),
                                std::string("there")),
             std::string("hello there"));

    // 全部替换（默认行为）
    CHECK_EQ(std::regex_replace(std::string("a1b2c3"), std::regex("\\d"),
                                std::string("#")),
             std::string("a#b#c#"));

    // 只替换第一个
    CHECK_EQ(std::regex_replace(std::string("a1b2c3"), std::regex("\\d"),
                                std::string("#"),
                                std::regex_constants::format_first_only),
             std::string("a#b2c3"));

    // 无匹配时原样返回
    CHECK_EQ(std::regex_replace(std::string("abc"), std::regex("xyz"),
                                std::string("#")),
             std::string("abc"));
}

TEST("regex/regex_replace with group references") {
    // $1 / $2 引用捕获组
    CHECK_EQ(std::regex_replace(std::string("2024-05-06"),
                                std::regex("(\\d{4})-(\\d{2})-(\\d{2})"),
                                std::string("$3/$2/$1")),
             std::string("06/05/2024"));

    // $& 整体匹配
    CHECK_EQ(std::regex_replace(std::string("a1b"), std::regex("\\d"),
                                std::string("[$&]")),
             std::string("a[1]b"));
}

//=============================================================
// 迭代器
//=============================================================

TEST("regex/sregex_iterator walks all matches") {
    std::string s = "one two three";
    std::regex re("\\w+");
    int n = 0;
    std::string last;
    for (std::sregex_iterator it(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
        ++n;
        last = it->str();
    }
    CHECK_EQ(n, 3);
    CHECK_EQ(last, std::string("three"));
}

TEST("regex/sregex_iterator on empty matches advances") {
    // 空匹配的迭代器必须能自行前进，否则是死循环
    std::string s = "ab";
    std::regex re("x*");
    int n = 0;
    for (std::sregex_iterator it(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
        ++n;
        if (n > 10) break;      // 防护：真死循环时把这个用例变成失败而不是挂住
    }
    CHECK_MSG(n <= 10, "sregex_iterator did not advance on empty matches");
    CHECK(n > 0);
}

TEST("regex/sregex_token_iterator splits on delimiter") {
    std::string s = "a,b,c";
    std::regex re(",");
    std::vector<std::string> parts;
    for (std::sregex_token_iterator it(s.begin(), s.end(), re, -1);
         it != std::sregex_token_iterator(); ++it)
        parts.push_back(it->str());

    CHECK_EQ(parts.size(), (std::size_t)3);
    if (parts.size() == 3) {
        CHECK_EQ(parts[0], std::string("a"));
        CHECK_EQ(parts[1], std::string("b"));
        CHECK_EQ(parts[2], std::string("c"));
    }
}

TEST("regex/sregex_token_iterator with no delimiter match") {
    std::string s = "abc";
    std::regex re(",");
    int n = 0;
    std::string got;
    for (std::sregex_token_iterator it(s.begin(), s.end(), re, -1);
         it != std::sregex_token_iterator(); ++it) {
        ++n;
        got = it->str();
    }
    CHECK_EQ(n, 1);
    CHECK_EQ(got, std::string("abc"));
}

TEST("regex/sregex_token_iterator submatch mode") {
    // submatch = 1：每次匹配产出第 1 组
    std::string s = "k1=10;k2=20";
    std::regex re("(\\w+)=(\\d+)");
    std::vector<std::string> keys;
    for (std::sregex_token_iterator it(s.begin(), s.end(), re, 1);
         it != std::sregex_token_iterator(); ++it)
        keys.push_back(it->str());
    CHECK_EQ(keys.size(), (std::size_t)2);
    if (keys.size() == 2) {
        CHECK_EQ(keys[0], std::string("k1"));
        CHECK_EQ(keys[1], std::string("k2"));
    }
}

//=============================================================
// 错误处理
//=============================================================

TEST("regex/regex_error on malformed patterns") {
    const char* bad[] = { "a(b", "a)b", "[a-", "a{2,1}", "*a", "a\\" };
    const int n = (int)(sizeof(bad) / sizeof(bad[0]));
    for (int i = 0; i < n; ++i) {
        bool threw = false;
        try {
            std::regex re(bad[i]);
            (void)re;
        } catch (const std::regex_error&) {
            threw = true;
        } catch (...) {
            threw = true;
        }
        CHECK_MSG(threw, bad[i]);
    }
}

TEST("regex/regex_error carries a code") {
    bool got_paren = false;
    try {
        std::regex re("a(b");
        (void)re;
    } catch (const std::regex_error& e) {
        got_paren = (e.code() == std::regex_constants::error_paren);
    } catch (...) {
    }
    CHECK(got_paren);
}

TEST("regex/valid patterns do not throw") {
    const char* good[] = { "a", "a|b", "(a)(b)", "[a-z]+", "\\d{3}", "^a$",
                           "a{2,}", "(?:ab)+", "\\bword\\b" };
    const int n = (int)(sizeof(good) / sizeof(good[0]));
    for (int i = 0; i < n; ++i) {
        CHECK_NOTHROW((void)std::regex(good[i]));
    }
}

//=============================================================
// basic_regex 自身
//=============================================================

TEST("regex/basic_regex copy assign and mark_count") {
    std::regex a("(\\d+)-(\\d+)");
    CHECK_EQ(a.mark_count(), (std::size_t)2);

    std::regex b = a;
    CHECK_EQ(b.mark_count(), (std::size_t)2);
    CHECK(std::regex_match(std::string("12-34"), b));

    std::regex c;
    c = a;
    CHECK(std::regex_match(std::string("12-34"), c));

    std::regex d("(\\d+)", std::regex::nosubs);
    CHECK_EQ(d.mark_count(), (std::size_t)0);
}

TEST("regex/non-capturing group") {
    CHECK(re_match("(?:ab)+", "abab"));
    std::regex re("(?:a)(b)");
    CHECK_EQ(re.mark_count(), (std::size_t)1);
    CHECK_EQ(group_of("(?:a)(b)", "ab", 1), std::string("b"));
}
