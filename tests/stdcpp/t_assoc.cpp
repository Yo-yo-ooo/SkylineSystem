//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_assoc.cpp — map / set / multimap / multiset / unordered_*
//
// 除了功能正确性，额外用“比较次数”检测树是否退化：
// 顺序插入 1..N 后做 N 次查找，若树是链表，比较次数约为 N²/2；
// 平衡树应接近 N·log2(N)。这条断言能直接抓出旋转逻辑写错。
//=============================================================
#include "framework/test_framework.h"
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <stdexcept>

namespace {

struct CountingLess {
    static long comparisons;
    bool operator()(int a, int b) const {
        ++comparisons;
        return a < b;
    }
};
long CountingLess::comparisons = 0;

} // namespace

TEST("map/insert find iterate") {
    std::map<int, int> m;
    CHECK(m.empty());

    m.insert(std::pair<const int, int>(3, 30));
    m.insert(std::pair<const int, int>(1, 10));
    m.insert(std::pair<const int, int>(2, 20));

    CHECK_EQ(m.size(), (std::size_t)3);
    CHECK_EQ(m[1], 10);
    CHECK_EQ(m.at(2), 20);
    CHECK_EQ(m.count(3), (std::size_t)1);
    CHECK_EQ(m.count(9), (std::size_t)0);

    // 迭代必须按 key 升序
    int expect_key[] = {1, 2, 3};
    int i = 0;
    for (std::map<int, int>::iterator it = m.begin(); it != m.end(); ++it) {
        CHECK_EQ(it->first, expect_key[i]);
        CHECK_EQ(it->second, expect_key[i] * 10);
        ++i;
    }
    CHECK_EQ(i, 3);

    // operator[] 插入默认值
    m[4] = 40;
    CHECK_EQ(m.size(), (std::size_t)4);
    CHECK_EQ(m[4], 40);
}

TEST("map/duplicate insert does not overwrite") {
    std::map<int, int> m;
    CHECK(m.insert(std::pair<const int, int>(1, 100)).second == true);
    CHECK(m.insert(std::pair<const int, int>(1, 999)).second == false);
    CHECK_EQ(m.size(), (std::size_t)1);
    CHECK_EQ(m[1], 100);
}

TEST("map/erase and at throws") {
    std::map<int, int> m;
    for (int i = 1; i <= 5; ++i) m[i] = i * 10;

    CHECK_EQ(m.erase(3), (std::size_t)1);
    CHECK_EQ(m.erase(3), (std::size_t)0);
    CHECK_EQ(m.size(), (std::size_t)4);
    CHECK_EQ(m.count(3), (std::size_t)0);

    std::map<int, int>::iterator it = m.find(4);
    CHECK(it != m.end());
    it = m.erase(it);
    CHECK_EQ(it->first, 5);

    CHECK_THROWS(m.at(42), std::out_of_range);
}

TEST("map/tree stays balanced on ordered insert") {
    CountingLess::comparisons = 0;
    std::map<int, int, CountingLess> m;
    const int N = 512;
    for (int i = 0; i < N; ++i) m.insert(std::pair<const int, int>(i, i));
    CHECK_EQ(m.size(), (std::size_t)N);

    const long insert_cost = CountingLess::comparisons;
    CountingLess::comparisons = 0;
    for (int i = 0; i < N; ++i) CHECK(m.find(i) != m.end());
    const long find_cost = CountingLess::comparisons;

    // 平衡树: 查找总代价约 N*log2(N)≈4600；链表退化会到 ~130000
    CHECK_MSG(find_cost < (long)N * 40,
              "lookup cost suggests degenerate tree (no rebalancing?)");
    (void)insert_cost;
}

TEST("map/iterate erase all") {
    std::map<int, int> m;
    for (int i = 0; i < 100; ++i) m[i] = i;
    for (std::map<int, int>::iterator it = m.begin(); it != m.end();) {
        if (it->first % 2 == 0) it = m.erase(it);
        else ++it;
    }
    CHECK_EQ(m.size(), (std::size_t)50);
    for (std::map<int, int>::iterator it = m.begin(); it != m.end(); ++it)
        CHECK_EQ(it->first % 2, 1);
}

TEST("multimap/duplicate keys and equal_range") {
    std::multimap<int, int> mm;
    mm.insert(std::pair<const int, int>(1, 10));
    mm.insert(std::pair<const int, int>(1, 11));
    mm.insert(std::pair<const int, int>(2, 20));
    CHECK_EQ(mm.size(), (std::size_t)3);
    CHECK_EQ(mm.count(1), (std::size_t)2);

    std::pair<std::multimap<int, int>::iterator, std::multimap<int, int>::iterator> r = mm.equal_range(1);
    int sum = 0;
    for (std::multimap<int, int>::iterator it = r.first; it != r.second; ++it) sum += it->second;
    CHECK_EQ(sum, 21);
}

TEST("set/insert find erase sorted") {
    std::set<int> s;
    CHECK(s.insert(5).second);
    CHECK(!s.insert(5).second);
    s.insert(1);
    s.insert(3);
    CHECK_EQ(s.size(), (std::size_t)3);

    int expect[] = {1, 3, 5};
    int i = 0;
    for (std::set<int>::iterator it = s.begin(); it != s.end(); ++it) CHECK_EQ(*it, expect[i++]);

    CHECK_EQ(s.count(3), (std::size_t)1);
    s.erase(3);
    CHECK_EQ(s.count(3), (std::size_t)0);
    CHECK_EQ(s.size(), (std::size_t)2);
}

TEST("set/multiset duplicates") {
    std::multiset<int> ms;
    ms.insert(1); ms.insert(1); ms.insert(2);
    CHECK_EQ(ms.size(), (std::size_t)3);
    CHECK_EQ(ms.count(1), (std::size_t)2);
}

TEST("set/balance on ordered insert") {
    CountingLess::comparisons = 0;
    std::set<int, CountingLess> s;
    const int N = 512;
    for (int i = 0; i < N; ++i) s.insert(i);
    CountingLess::comparisons = 0;
    for (int i = 0; i < N; ++i) CHECK(s.find(i) != s.end());
    CHECK(s.size() == (std::size_t)N);
    CHECK_MSG(CountingLess::comparisons < (long)N * 40,
              "set lookups suggest degenerate tree");
}

TEST("unordered_map/basic and growth") {
    std::unordered_map<int, int> m;
    CHECK(m.empty());

    for (int i = 0; i < 500; ++i) m[i] = i * 3;
    CHECK_EQ(m.size(), (std::size_t)500);
    CHECK(m.bucket_count() > 1);

    for (int i = 0; i < 500; ++i) {
        std::unordered_map<int, int>::iterator it = m.find(i);
        CHECK(it != m.end());
        if (it != m.end()) CHECK_EQ(it->second, i * 3);
    }
    CHECK(m.find(5000) == m.end());

    CHECK_EQ(m.erase(0), (std::size_t)1);
    CHECK_EQ(m.size(), (std::size_t)499);
    CHECK(m.find(0) == m.end());

    // 迭代必须覆盖所有元素
    std::size_t n = 0;
    for (std::unordered_map<int, int>::iterator it = m.begin(); it != m.end(); ++it) ++n;
    CHECK_EQ(n, (std::size_t)499);
}

TEST("unordered_map/at and operator[]") {
    std::unordered_map<int, int> m;
    m[7] = 70;
    CHECK_EQ(m.at(7), 70);
    CHECK_THROWS(m.at(8), std::out_of_range);
    CHECK_EQ(m[8], 0);              // operator[] 插入默认值
    CHECK_EQ(m.size(), (std::size_t)2);
}

TEST("unordered_map/string keys") {
    std::unordered_map<std::string, int> m;
    m[std::string("alpha")] = 1;
    m[std::string("beta")] = 2;
    m[std::string("gamma")] = 3;
    CHECK_EQ(m.size(), (std::size_t)3);
    CHECK_EQ(m.at(std::string("beta")), 2);
    CHECK(m.find(std::string("delta")) == m.end());
}

TEST("unordered_set/basic") {
    std::unordered_set<int> s;
    CHECK(s.insert(3).second);
    CHECK(!s.insert(3).second);
    for (int i = 0; i < 200; ++i) s.insert(i);
    CHECK_EQ(s.size(), (std::size_t)200);
    CHECK_EQ(s.count(100), (std::size_t)1);
    CHECK_EQ(s.count(999), (std::size_t)0);
    s.erase(100);
    CHECK_EQ(s.count(100), (std::size_t)0);
    CHECK_EQ(s.size(), (std::size_t)199);
}

TEST("assoc/element lifetime") {
    // 关联容器节点式存储，键值对的构造/析构必须配对
    {
        std::map<int, std::string> m;
        for (int i = 0; i < 64; ++i) m[i] = std::string("value");
        CHECK_EQ(m.size(), (std::size_t)64);
        m.clear();
        CHECK_EQ(m.size(), (std::size_t)0);
        m[1] = std::string("x");
        CHECK_EQ(m.size(), (std::size_t)1);
    }
    CHECK(tst::alloc_live_blocks() == 0);
}
