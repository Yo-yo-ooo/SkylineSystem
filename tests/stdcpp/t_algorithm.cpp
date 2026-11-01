//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_algorithm.cpp — <algorithm> 正确性
//
// 用确定性 LCG 生成数据，与"朴素参考实现"逐项比对：
// 排序类算法比较容易"看起来对"，必须校验 排序性 + 多重集合一致性
// （防止元素丢失/重复）。
//=============================================================
#include "framework/test_framework.h"
#include <algorithm>
#include <vector>
#include <functional>
#include <utility>
#include <cstddef>

namespace {

unsigned g_seed = 0x12345678u;
unsigned lcg() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}
int rnd(int lo, int hi) { return lo + (int)(lcg() % (unsigned)(hi - lo + 1)); }

// 朴素参考：插入排序
void ref_sort(int* a, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i) {
        int key = a[i];
        std::size_t j = i;
        while (j > 0 && a[j - 1] > key) { a[j] = a[j - 1]; --j; }
        a[j] = key;
    }
}

bool is_sorted_asc(const std::vector<int>& v) {
    for (std::size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] > v[i]) return false;
    return true;
}

} // namespace

TEST("algorithm/sort matches reference") {
    for (int trial = 0; trial < 40; ++trial) {
        const std::size_t n = (std::size_t)rnd(0, 200);
        std::vector<int> v;
        for (std::size_t i = 0; i < n; ++i) v.push_back(rnd(-500, 500));
        std::vector<int> ref(v);

        std::sort(v.begin(), v.end());
        ref_sort(ref.data(), ref.size());

        CHECK(is_sorted_asc(v));
        CHECK_EQ(v.size(), ref.size());
        bool same = true;
        for (std::size_t i = 0; i < v.size(); ++i) if (v[i] != ref[i]) same = false;
        CHECK(same);
    }
}

TEST("algorithm/sort with comparator") {
    std::vector<int> v;
    for (int i = 0; i < 100; ++i) v.push_back(rnd(-50, 50));
    std::sort(v.begin(), v.end(), std::greater<int>());
    for (std::size_t i = 1; i < v.size(); ++i) CHECK(v[i - 1] >= v[i]);
}

TEST("algorithm/stable_sort keeps relative order") {
    // (key, seq) 按 key 排序，相等 key 的 seq 必须保持输入顺序
    std::vector<std::pair<int, int> > v;
    for (int i = 0; i < 200; ++i) v.push_back(std::make_pair(rnd(0, 9), i));

    struct Cmp {
        bool operator()(const std::pair<int, int>& a, const std::pair<int, int>& b) const {
            return a.first < b.first;
        }
    };
    std::stable_sort(v.begin(), v.end(), Cmp());
    for (std::size_t i = 1; i < v.size(); ++i) {
        CHECK(v[i - 1].first <= v[i].first);
        if (v[i - 1].first == v[i].first) CHECK(v[i - 1].second < v[i].second);
    }
}

TEST("algorithm/binary search family") {
    std::vector<int> v;
    for (int i = 0; i < 100; ++i) v.push_back(i * 2);       // 0,2,...,198

    CHECK(std::binary_search(v.begin(), v.end(), 50));
    CHECK(!std::binary_search(v.begin(), v.end(), 51));

    CHECK_EQ(*std::lower_bound(v.begin(), v.end(), 50), 50);
    CHECK_EQ(*std::upper_bound(v.begin(), v.end(), 50), 52);
    CHECK_EQ(std::lower_bound(v.begin(), v.end(), 51) - v.begin(), 26);
    CHECK_EQ(std::upper_bound(v.begin(), v.end(), 51) - v.begin(), 26);

    std::pair<std::vector<int>::iterator, std::vector<int>::iterator> r =
        std::equal_range(v.begin(), v.end(), 50);
    CHECK_EQ(r.second - r.first, 1);
    CHECK_EQ(*r.first, 50);

    CHECK_EQ(std::lower_bound(v.begin(), v.end(), -1) - v.begin(), 0);
    CHECK_EQ(std::lower_bound(v.begin(), v.end(), 999) - v.begin(), 100);
}

TEST("algorithm/find count min max") {
    int a[] = {5, 1, 9, 3, 7, 3};
    CHECK_EQ(*std::find(a, a + 6, 9), 9);
    CHECK(std::find(a, a + 6, 100) == a + 6);
    CHECK_EQ(std::count(a, a + 6, 3), (std::ptrdiff_t)2);
    CHECK_EQ(*std::min_element(a, a + 6), 1);
    CHECK_EQ(*std::max_element(a, a + 6), 9);

    CHECK_EQ(std::min(3, 7), 3);
    CHECK_EQ(std::max(3, 7), 7);
    CHECK_EQ(std::minmax(3, 7).first, 3);
    CHECK_EQ(std::minmax(3, 7).second, 7);
    CHECK_EQ(std::clamp(10, 0, 5), 5);
    CHECK_EQ(std::clamp(-10, 0, 5), 0);
    CHECK_EQ(std::clamp(3, 0, 5), 3);

    CHECK(std::all_of(a, a + 6, [](int x) { return x > 0; }));
    CHECK(std::any_of(a, a + 6, [](int x) { return x == 7; }));
    CHECK(!std::none_of(a, a + 6, [](int x) { return x == 7; }));
}

TEST("algorithm/modifiers") {
    std::vector<int> v;
    for (int i = 0; i < 10; ++i) v.push_back(i);

    std::vector<int> out(10, -1);
    std::copy(v.begin(), v.end(), out.begin());
    CHECK(out == v);

    std::reverse(v.begin(), v.end());
    CHECK_EQ(v.front(), 9);
    CHECK_EQ(v.back(), 0);

    // remove/erase 惯用法
    std::vector<int> w;
    for (int i = 0; i < 10; ++i) w.push_back(i % 3);
    w.erase(std::remove(w.begin(), w.end(), 1), w.end());
    for (std::size_t i = 0; i < w.size(); ++i) CHECK_NE(w[i], 1);

    std::vector<int> u;
    u.push_back(1); u.push_back(1); u.push_back(2); u.push_back(2); u.push_back(2); u.push_back(3);
    u.erase(std::unique(u.begin(), u.end()), u.end());
    CHECK_EQ(u.size(), (std::size_t)3);
    CHECK_EQ(u[0], 1); CHECK_EQ(u[1], 2); CHECK_EQ(u[2], 3);

    std::vector<int> f(5, 0);
    std::fill(f.begin(), f.end(), 7);
    for (std::size_t i = 0; i < f.size(); ++i) CHECK_EQ(f[i], 7);

    std::vector<int> t(5, 0);
    int n = 0;
    std::generate(t.begin(), t.end(), [&n]() { return n++; });
    for (int i = 0; i < 5; ++i) CHECK_EQ(t[(std::size_t)i], i);

    std::vector<int> src(5, 3);
    std::vector<int> dst(5, 0);
    std::transform(src.begin(), src.end(), dst.begin(), [](int x) { return x * 2; });
    for (int i = 0; i < 5; ++i) CHECK_EQ(dst[(std::size_t)i], 6);
}

TEST("algorithm/rotate partition") {
    std::vector<int> v;
    for (int i = 0; i < 7; ++i) v.push_back(i);      // 0..6
    std::rotate(v.begin(), v.begin() + 3, v.end());
    const int expect[] = {3, 4, 5, 6, 0, 1, 2};
    for (int i = 0; i < 7; ++i) CHECK_EQ(v[(std::size_t)i], expect[i]);

    std::vector<int> p;
    for (int i = 0; i < 10; ++i) p.push_back(i);
    std::vector<int>::iterator mid = std::partition(p.begin(), p.end(), [](int x) { return x % 2 == 0; });
    for (std::vector<int>::iterator it = p.begin(); it != mid; ++it) CHECK_EQ(*it % 2, 0);
    for (std::vector<int>::iterator it = mid; it != p.end(); ++it) CHECK_EQ(*it % 2, 1);
}

TEST("algorithm/heap operations") {
    std::vector<int> v;
    for (int i = 0; i < 50; ++i) v.push_back(rnd(0, 100));

    std::make_heap(v.begin(), v.end());
    CHECK(std::is_heap(v.begin(), v.end()));

    const std::size_t n = v.size();
    std::vector<int> popped;
    while (!v.empty()) {
        std::pop_heap(v.begin(), v.end());
        popped.push_back(v.back());
        v.pop_back();
    }
    CHECK_EQ(popped.size(), n);
    // pop_heap 必须从大到小
    for (std::size_t i = 1; i < popped.size(); ++i) CHECK(popped[i - 1] >= popped[i]);

    std::vector<int> h;
    h.push_back(10);
    std::push_heap(h.begin(), h.end());
    h.push_back(30);
    std::push_heap(h.begin(), h.end());
    h.push_back(20);
    std::push_heap(h.begin(), h.end());
    CHECK_EQ(h.front(), 30);
    std::sort_heap(h.begin(), h.end());
    CHECK_EQ(h[0], 10);
    CHECK_EQ(h[2], 30);
}

TEST("algorithm/permutation and lexicographic") {
    std::vector<int> v;
    v.push_back(1); v.push_back(2); v.push_back(3);
    int count = 0;
    do { ++count; } while (std::next_permutation(v.begin(), v.end()));
    CHECK_EQ(count, 6);
    CHECK(std::is_sorted(v.begin(), v.end()));

    std::vector<int> a;
    a.push_back(1); a.push_back(2);
    std::vector<int> b;
    b.push_back(1); b.push_back(3);
    CHECK(std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end()));
    CHECK(std::equal(a.begin(), a.end(), a.begin()));
}

TEST("algorithm/set operations") {
    std::vector<int> a;
    a.push_back(1); a.push_back(3); a.push_back(5);
    std::vector<int> b;
    b.push_back(3); b.push_back(5); b.push_back(7);

    std::vector<int> inter(3, 0);
    std::vector<int>::iterator ie = std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), inter.begin());
    CHECK_EQ(ie - inter.begin(), 2);
    CHECK_EQ(inter[0], 3);
    CHECK_EQ(inter[1], 5);

    std::vector<int> uni(6, 0);
    std::vector<int>::iterator ue = std::set_union(a.begin(), a.end(), b.begin(), b.end(), uni.begin());
    CHECK_EQ(ue - uni.begin(), 4);
    CHECK_EQ(uni[3], 7);

    std::vector<int> diff(3, 0);
    std::vector<int>::iterator de = std::set_difference(a.begin(), a.end(), b.begin(), b.end(), diff.begin());
    CHECK_EQ(de - diff.begin(), 1);
    CHECK_EQ(diff[0], 1);

    CHECK(std::includes(a.begin(), a.end(), b.begin(), b.end()) == false);
}
