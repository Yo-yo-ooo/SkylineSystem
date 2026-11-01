//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_adaptors.cpp — array / span / stack / queue / priority_queue
//                   inplace_vector / flat_map / flat_set / hive
//=============================================================
#include "framework/test_framework.h"
#include <array>
#include <span>
#include <stack>
#include <queue>
#include <vector>
#include <deque>
#include <inplace_vector>
#include <flat_map>
#include <flat_set>
#include <hive>
#include <stdexcept>

TEST("array/basics and at") {
    std::array<int, 4> a = {{1, 2, 3, 4}};
    CHECK_EQ(a.size(), (std::size_t)4);
    CHECK(!a.empty());
    CHECK_EQ(a.front(), 1);
    CHECK_EQ(a.back(), 4);
    CHECK_EQ(a[2], 3);
    CHECK_EQ(*a.data(), 1);

    a.fill(7);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK_EQ(a[i], 7);

    CHECK_EQ(a.at(0), 7);
    CHECK_THROWS(a.at(4), std::out_of_range);

    std::array<int, 4> b = {{1, 1, 1, 1}};
    a.swap(b);
    CHECK_EQ(a[0], 1);
    CHECK_EQ(b[0], 7);
}

TEST("array/comparison and iteration") {
    std::array<int, 3> a = {{1, 2, 3}};
    std::array<int, 3> b = {{1, 2, 4}};
    CHECK(a < b);
    CHECK(a != b);
    CHECK(a == a);

    int sum = 0;
    for (std::array<int, 3>::iterator it = a.begin(); it != a.end(); ++it) sum += *it;
    CHECK_EQ(sum, 6);
}

#if __cplusplus >= 202002L
TEST("span/from pointer and subviews") {
    int raw[] = {1, 2, 3, 4, 5};
    std::span<int> s(raw, 5);
    CHECK_EQ(s.size(), (std::size_t)5);
    CHECK_EQ(s[0], 1);
    CHECK_EQ(s.back(), 5);
    CHECK(!s.empty());
    CHECK_EQ(s.size_bytes(), (std::size_t)(5 * sizeof(int)));

    std::span<int> first = s.first(2);
    CHECK_EQ(first.size(), (std::size_t)2);
    CHECK_EQ(first[1], 2);

    std::span<int> last = s.last(2);
    CHECK_EQ(last[0], 4);

    std::span<int> sub = s.subspan(1, 3);
    CHECK_EQ(sub.size(), (std::size_t)3);
    CHECK_EQ(sub[0], 2);
}
#endif // C++20

TEST("stack/lifo") {
    std::stack<int> s;
    CHECK(s.empty());
    for (int i = 0; i < 10; ++i) s.push(i);
    CHECK_EQ(s.size(), (std::size_t)10);
    CHECK_EQ(s.top(), 9);

    int expect = 9;
    while (!s.empty()) {
        CHECK_EQ(s.top(), expect);
        s.pop();
        --expect;
    }
    CHECK(s.empty());

    // 底层容器可替换
    std::stack<int, std::vector<int> > sv;
    sv.push(1);
    sv.push(2);
    CHECK_EQ(sv.top(), 2);
}

TEST("queue/fifo") {
    std::queue<int> q;
    for (int i = 0; i < 5; ++i) q.push(i);
    CHECK_EQ(q.size(), (std::size_t)5);
    CHECK_EQ(q.front(), 0);
    CHECK_EQ(q.back(), 4);

    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(q.front(), i);
        q.pop();
    }
    CHECK(q.empty());
}

TEST("priority_queue/max heap ordering") {
    std::priority_queue<int> pq;
    int data[] = {5, 1, 9, 3, 7};
    for (int i = 0; i < 5; ++i) pq.push(data[i]);
    CHECK_EQ(pq.size(), (std::size_t)5);
    CHECK_EQ(pq.top(), 9);

    int expect[] = {9, 7, 5, 3, 1};
    int i = 0;
    while (!pq.empty()) {
        CHECK_EQ(pq.top(), expect[i++]);
        pq.pop();
    }
    CHECK_EQ(i, 5);
}

TEST("priority_queue/min heap via comparator") {
    std::priority_queue<int, std::vector<int>, std::greater<int> > pq;
    pq.push(5); pq.push(1); pq.push(9);
    CHECK_EQ(pq.top(), 1);
    pq.pop();
    CHECK_EQ(pq.top(), 5);
}

#if __cplusplus >= 202400L
TEST("inplace_vector/fixed capacity") {
    std::inplace_vector<int, 8> v;
    CHECK(v.empty());
    for (int i = 0; i < 8; ++i) v.push_back(i);
    CHECK_EQ(v.size(), (std::size_t)8);
    CHECK_EQ(v[7], 7);
    CHECK_EQ(v.capacity(), (std::size_t)8);

    v.pop_back();
    CHECK_EQ(v.size(), (std::size_t)7);

    v.insert(v.begin() + 1, 99);
    CHECK_EQ(v[1], 99);
    CHECK_EQ(v.size(), (std::size_t)8);

    v.erase(v.begin() + 1);
    CHECK_EQ(v[1], 1);
    CHECK_EQ(v.size(), (std::size_t)7);

    // 空容器上 insert 曾是未定义行为（读 _M_data()[-1]）
    std::inplace_vector<int, 4> e;
    e.insert(e.begin(), 42);
    CHECK_EQ(e.size(), (std::size_t)1);
    CHECK_EQ(e[0], 42);

    // 满容量必须报错，而不是越界写
    std::inplace_vector<int, 2> f;
    f.push_back(1);
    f.push_back(2);
    CHECK(f.full());
    CHECK_THROWS(f.push_back(3), std::length_error);
    CHECK_THROWS(f.insert(f.begin(), 9), std::length_error);
    CHECK_EQ(f.size(), (std::size_t)2);
}
#endif // C++26

#if __cplusplus >= 202100L
TEST("flat_map/insert find erase sorted") {
    std::flat_map<int, int> m;
    m.insert(std::pair<const int, int>(3, 30));
    m.insert(std::pair<const int, int>(1, 10));
    m.insert(std::pair<const int, int>(2, 20));

    CHECK_EQ(m.size(), (std::size_t)3);
    CHECK_EQ(m.at(2), 20);
    CHECK_EQ(m[1], 10);
    CHECK_EQ(m.count(9), (std::size_t)0);

    int expect[] = {1, 2, 3};
    int i = 0;
    for (std::flat_map<int, int>::iterator it = m.begin(); it != m.end(); ++it)
        CHECK_EQ(it->first, expect[i++]);

    CHECK_EQ(m.erase(2), (std::size_t)1);
    CHECK_EQ(m.size(), (std::size_t)2);
    CHECK_THROWS(m.at(2), std::out_of_range);
}

TEST("flat_set/insert find erase sorted") {
    std::flat_set<int> s;
    CHECK(s.insert(3).second);
    CHECK(!s.insert(3).second);
    s.insert(1);
    s.insert(2);
    CHECK_EQ(s.size(), (std::size_t)3);

    int expect[] = {1, 2, 3};
    int i = 0;
    for (std::flat_set<int>::iterator it = s.begin(); it != s.end(); ++it)
        CHECK_EQ(*it, expect[i++]);

    CHECK_EQ(s.count(2), (std::size_t)1);
    s.erase(2);
    CHECK_EQ(s.count(2), (std::size_t)0);
}
#endif // C++23

#if __cplusplus >= 202400L
TEST("hive/basic container semantics") {
    std::hive<int> h;
    for (int i = 0; i < 100; ++i) h.push_back(i);
    CHECK_EQ(h.size(), (std::size_t)100);

    std::size_t n = 0;
    for (std::hive<int>::iterator it = h.begin(); it != h.end(); ++it) ++n;
    CHECK_EQ(n, (std::size_t)100);

    h.clear();
    CHECK(h.empty());
}

TEST("hive/erase returns the next live element") {
    std::hive<int> h;
    h.push_back(1);
    h.push_back(2);
    h.push_back(3);

    std::hive<int>::iterator it = h.begin();
    std::hive<int>::iterator next = h.erase(it);
    CHECK_EQ(*next, 2);
    CHECK_EQ(h.size(), (std::size_t)2);

    // 连续删除同一个迭代器返回的位置
    next = h.erase(next);
    CHECK_EQ(*next, 3);
    next = h.erase(next);
    CHECK(next == h.end());
    CHECK(h.empty());
    CHECK_EQ(h.capacity(), (std::size_t)0);      // 全空后块已回收
}

TEST("hive/erasing a whole block releases it") {
    std::hive<int> h;
    for (int i = 0; i < 128; ++i) h.push_back(i);   // 两块（默认 BlockSize=64）
    CHECK_EQ(h.size(), (std::size_t)128);
    CHECK_EQ(h.capacity(), (std::size_t)128);

    // 删掉前半部分（第一块的全部元素）→ 该块应被回收
    std::size_t erased = 0;
    for (std::hive<int>::iterator it = h.begin(); it != h.end(); ) {
        if (*it < 64) { it = h.erase(it); ++erased; }
        else ++it;
    }
    CHECK_EQ(erased, (std::size_t)64);
    CHECK_EQ(h.size(), (std::size_t)64);
    CHECK_EQ(h.capacity(), (std::size_t)64);        // 空块已摘链释放
    CHECK_EQ(*h.begin(), 64);
}

TEST("hive/insert reuses erased slots without growing") {
    std::hive<int> h;
    for (int i = 0; i < 64; ++i) h.push_back(i);    // 恰好一块
    const std::size_t cap_before = h.capacity();
    CHECK_EQ(cap_before, (std::size_t)64);

    // 制造 10 个空洞
    std::size_t erased = 0;
    for (std::hive<int>::iterator it = h.begin(); it != h.end() && erased < 10; ) {
        it = h.erase(it);
        ++erased;
    }
    CHECK_EQ(h.size(), (std::size_t)54);

    // 再插 10 个：必须复用空洞，容量不变（不新开块）
    for (int i = 0; i < 10; ++i) {
        std::hive<int>::iterator pos = h.insert(1000 + i);
        CHECK_EQ(*pos, 1000 + i);
    }
    CHECK_EQ(h.size(), (std::size_t)64);
    CHECK_EQ(h.capacity(), cap_before);

    // 元素总数（迭代一遍）必须等于 size()
    std::size_t n = 0;
    for (std::hive<int>::iterator it = h.begin(); it != h.end(); ++it) ++n;
    CHECK_EQ(n, (std::size_t)64);
}

namespace hive_probe {
struct L {
    static int live;
    int v;
    explicit L(int x = 0) : v(x) { ++live; }
    L(const L& o) : v(o.v) { ++live; }
    L& operator=(const L& o) { v = o.v; return *this; }
    ~L() { --live; }
};
int L::live = 0;
} // namespace hive_probe

TEST("hive/element lifetime") {
    using hive_probe::L;
    L::live = 0;
    {
        std::hive<L> h;
        for (int i = 0; i < 200; ++i) h.push_back(L(i));
        CHECK_EQ(L::live, 200);

        // 删掉能被 3 整除的（200 个里共 67 个）
        for (std::hive<L>::iterator it = h.begin(); it != h.end(); ) {
            if (it->v % 3 == 0) it = h.erase(it);
            else ++it;
        }
        CHECK_EQ(h.size(), (std::size_t)133);
        CHECK_EQ(L::live, 133);

        h.clear();
        CHECK_EQ(L::live, 0);
    }
    CHECK_EQ(L::live, 0);
}
#endif // C++26
