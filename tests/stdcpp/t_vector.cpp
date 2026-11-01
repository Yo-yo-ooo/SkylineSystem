//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_vector.cpp — vector 语义与异常安全
//
// 关注点：
//   * 元素生命周期（构造/析构配对，无二次析构、无未构造访问）
//   * 扩容后数据完整、容量语义正确
//   * insert/erase 各重载的位移正确性与自引用安全
//   * 分配失败时不留泄漏、不留悬垂（basic guarantee 至少成立）
//=============================================================
#include "framework/test_framework.h"
#include <vector>
#include <new>
#include <stdexcept>

namespace {

// 带生命周期的探针类型：析构时校验魔数，可抓“二次析构/未构造析构”
struct Tracked {
    static int live;
    static int ctor;
    static int dtor;
    static int copy;
    static int move;

    unsigned magic;
    int value;

    explicit Tracked(int v = 0) : magic(0xA5A5u), value(v) { ++live; ++ctor; }
    Tracked(const Tracked& o) : magic(0xA5A5u), value(o.value) { ++live; ++copy; }
    Tracked(Tracked&& o) noexcept : magic(0xA5A5u), value(o.value) {
        o.magic = 0xBEEFu;          // 已移出但仍存活，析构时不应算作异常
        ++live; ++move;
    }
    Tracked& operator=(const Tracked& o) {
        value = o.value; magic = 0xA5A5u; return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept {
        value = o.value; magic = 0xA5A5u; o.magic = 0xBEEFu; return *this;
    }
    ~Tracked() {
        // 只把“对已析构对象再次析构”记为错误
        if (magic != 0xA5A5u && magic != 0xBEEFu) ++bad_dtor;
        magic = 0;
        --live; ++dtor;
    }
    static int bad_dtor;

    static void reset() { live = ctor = dtor = copy = move = bad_dtor = 0; }
};

int Tracked::live = 0;
int Tracked::ctor = 0;
int Tracked::dtor = 0;
int Tracked::copy = 0;
int Tracked::move = 0;
int Tracked::bad_dtor = 0;

} // namespace

TEST("vector/basic push_back and growth") {
    std::vector<int> v;
    CHECK(v.empty());
    CHECK_EQ(v.size(), (std::size_t)0);

    for (int i = 0; i < 100; ++i) v.push_back(i);

    CHECK_EQ(v.size(), (std::size_t)100);
    CHECK(v.capacity() >= v.size());
    for (int i = 0; i < 100; ++i) CHECK_EQ(v[(std::size_t)i], i);
    CHECK_EQ(v.front(), 0);
    CHECK_EQ(v.back(), 99);
    CHECK_EQ((int)(v.end() - v.begin()), 100);
}

TEST("vector/reserve does not reallocate unnecessarily") {
    std::vector<int> v;
    v.reserve(64);
    const int* base = v.data();
    for (int i = 0; i < 64; ++i) v.push_back(i);
    CHECK_EQ(v.data(), base);          // 预留足够时不得搬迁
    CHECK_EQ(v.capacity(), (std::size_t)64);
    v.shrink_to_fit();
    CHECK_EQ(v.size(), (std::size_t)64);
    CHECK_EQ(v.capacity(), (std::size_t)64);
}

TEST("vector/element lifetime balanced") {
    Tracked::reset();
    {
        std::vector<Tracked> v;
        for (int i = 0; i < 50; ++i) v.push_back(Tracked(i));
        CHECK_EQ(Tracked::live, 50);
        v.clear();
        CHECK_EQ(Tracked::live, 0);
        CHECK_EQ(Tracked::bad_dtor, 0);
        for (int i = 0; i < 10; ++i) v.emplace_back(i);
        CHECK_EQ(Tracked::live, 10);
    }
    CHECK_EQ(Tracked::live, 0);
    CHECK_EQ(Tracked::bad_dtor, 0);
    CHECK_EQ(Tracked::ctor + Tracked::copy + Tracked::move, Tracked::dtor);
}

TEST("vector/copy and move") {
    std::vector<int> a;
    for (int i = 0; i < 8; ++i) a.push_back(i);

    std::vector<int> b(a);
    CHECK_EQ(b.size(), a.size());
    for (int i = 0; i < 8; ++i) CHECK_EQ(b[(std::size_t)i], i);

    std::vector<int> c;
    c = a;
    CHECK_EQ(c.size(), a.size());
    CHECK(a == c);
    CHECK(!(a != b));

    int* raw = a.data();
    std::vector<int> d(static_cast<std::vector<int>&&>(a));
    CHECK_EQ(d.data(), raw);          // 移动必须窃取缓冲区
    CHECK(a.empty());
}

TEST("vector/insert single in middle") {
    std::vector<int> v;
    for (int i = 0; i < 5; ++i) v.push_back(i);      // 0 1 2 3 4

    std::vector<int>::iterator it = v.insert(v.begin() + 2, 99);
    CHECK_EQ(*it, 99);
    CHECK_EQ(v.size(), (std::size_t)6);
    const int expect[] = {0, 1, 99, 2, 3, 4};
    for (int i = 0; i < 6; ++i) CHECK_EQ(v[(std::size_t)i], expect[i]);

    v.insert(v.begin(), -1);
    v.insert(v.end(), 42);
    CHECK_EQ(v.front(), -1);
    CHECK_EQ(v.back(), 42);
    CHECK_EQ(v.size(), (std::size_t)8);
}

TEST("vector/insert n copies") {
    std::vector<int> v;
    for (int i = 0; i < 4; ++i) v.push_back(i);      // 0 1 2 3
    v.insert(v.begin() + 1, (std::size_t)3, 7);
    const int expect[] = {0, 7, 7, 7, 1, 2, 3};
    CHECK_EQ(v.size(), (std::size_t)7);
    for (int i = 0; i < 7; ++i) CHECK_EQ(v[(std::size_t)i], expect[i]);
}

TEST("vector/insert range") {
    std::vector<int> v;
    v.push_back(1);
    v.push_back(2);
    int src[] = {7, 8, 9};
    v.insert(v.begin() + 1, src, src + 3);
    const int expect[] = {1, 7, 8, 9, 2};
    CHECK_EQ(v.size(), (std::size_t)5);
    for (int i = 0; i < 5; ++i) CHECK_EQ(v[(std::size_t)i], expect[i]);
}

TEST("vector/insert self-reference is safe") {
    std::vector<int> v;
    v.push_back(11);
    v.push_back(22);
    // 自引用：源是自身元素，且会触发扩容
    v.reserve(v.size());
    v.insert(v.begin(), v[0]);
    CHECK_EQ(v.size(), (std::size_t)3);
    CHECK_EQ(v[0], 11);
    CHECK_EQ(v[1], 11);
    CHECK_EQ(v[2], 22);
}

TEST("vector/erase and pop_back") {
    std::vector<int> v;
    for (int i = 0; i < 6; ++i) v.push_back(i);
    v.erase(v.begin() + 1);
    const int after1[] = {0, 2, 3, 4, 5};
    CHECK_EQ(v.size(), (std::size_t)5);
    for (int i = 0; i < 5; ++i) CHECK_EQ(v[(std::size_t)i], after1[i]);

    v.erase(v.begin() + 1, v.begin() + 3);
    const int after2[] = {0, 4, 5};
    CHECK_EQ(v.size(), (std::size_t)3);
    for (int i = 0; i < 3; ++i) CHECK_EQ(v[(std::size_t)i], after2[i]);

    v.pop_back();
    CHECK_EQ(v.size(), (std::size_t)2);
    CHECK_EQ(v.back(), 4);
}

TEST("vector/resize and assign") {
    std::vector<int> v;
    v.resize(5);
    CHECK_EQ(v.size(), (std::size_t)5);
    for (int i = 0; i < 5; ++i) CHECK_EQ(v[(std::size_t)i], 0);

    v.resize(8, 7);
    CHECK_EQ(v.size(), (std::size_t)8);
    for (std::size_t i = 5; i < 8; ++i) CHECK_EQ(v[i], 7);

    v.resize(2);
    CHECK_EQ(v.size(), (std::size_t)2);

    v.assign((std::size_t)4, 3);
    CHECK_EQ(v.size(), (std::size_t)4);
    for (std::size_t i = 0; i < 4; ++i) CHECK_EQ(v[i], 3);

    int src[] = {1, 2, 3};
    v.assign(src, src + 3);
    CHECK_EQ(v.size(), (std::size_t)3);
    CHECK_EQ(v[2], 3);
}

TEST("vector/at throws out_of_range") {
    std::vector<int> v;
    v.push_back(1);
    CHECK_EQ(v.at(0), 1);
    CHECK_THROWS(v.at(1), std::out_of_range);
    const std::vector<int>& cv = v;
    CHECK_THROWS(cv.at(5), std::out_of_range);
}

TEST("vector/emplace returns reference") {
    std::vector<Tracked> v;
    Tracked::reset();
    Tracked& r = v.emplace_back(5);
    CHECK_EQ(r.value, 5);
    CHECK_EQ(v.size(), (std::size_t)1);
    v.emplace(v.begin(), 1);
    CHECK_EQ(v.size(), (std::size_t)2);
    CHECK_EQ(v[0].value, 1);
    CHECK_EQ(v[1].value, 5);
    CHECK_EQ(Tracked::bad_dtor, 0);
}

TEST("vector/allocation failure keeps container valid") {
    std::vector<int> v;
    for (int i = 0; i < 8; ++i) v.push_back(i);
    const std::size_t before_size = v.size();

    // 让下一次分配失败：push_back 扩容时应抛出且容器自洽
    tst::alloc_fail_after(0);
    bool threw = false;
    try {
        for (int i = 0; i < 1000; ++i) v.push_back(i);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    tst::alloc_fail_off();
    CHECK(threw);
    CHECK(v.size() >= before_size);
    for (std::size_t i = 0; i < before_size; ++i) CHECK_EQ(v[i], (int)i);
}

TEST("vector/comparison operators") {
    std::vector<int> a;
    a.push_back(1); a.push_back(2);
    std::vector<int> b;
    b.push_back(1); b.push_back(2);
    std::vector<int> c;
    c.push_back(1); c.push_back(3);

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
    CHECK(!(c < a));
}

//=============================================================
// 批量插入：正确性 + 复杂度
//=============================================================
namespace {

// 统计元素拷贝/移动次数，用来断言 insert 是 O(size+n) 而不是 O(n·size)
struct Counted {
    static long ops;
    int v;
    explicit Counted(int x = 0) : v(x) {}
    Counted(const Counted& o) : v(o.v) { ++ops; }
    Counted(Counted&& o) noexcept : v(o.v) { ++ops; }
    Counted& operator=(const Counted& o) { v = o.v; ++ops; return *this; }
    Counted& operator=(Counted&& o) noexcept { v = o.v; ++ops; return *this; }
};
long Counted::ops = 0;

} // namespace

TEST("vector/insert n copies correctness at each position") {
    const int N = 5;
    for (int where = 0; where <= N; ++where) {
        std::vector<int> v;
        for (int i = 0; i < N; ++i) v.push_back(i);      // 0..4

        v.insert(v.begin() + where, (std::size_t)3, 99);

        CHECK_EQ(v.size(), (std::size_t)(N + 3));
        for (int i = 0; i < where; ++i) CHECK_EQ(v[(std::size_t)i], i);
        for (int i = 0; i < 3; ++i) CHECK_EQ(v[(std::size_t)(where + i)], 99);
        for (int i = where; i < N; ++i) CHECK_EQ(v[(std::size_t)(i + 3)], i);
    }
}

TEST("vector/insert n copies is linear, not quadratic") {
    std::vector<Counted> v;
    v.reserve(2000);
    for (int i = 0; i < 500; ++i) v.push_back(Counted(i));
    Counted::ops = 0;

    Counted c(7);
    v.insert(v.begin() + 250, (std::size_t)100, c);

    // 线性实现: 位移 + 赋值约 2*(500+100) 次；旧的"逐个 emplace"
    // 会做 100 轮整段位移 ≈ 50000 次，用 5000 作分界足够区分。
    CHECK_EQ(v.size(), (std::size_t)600);
    CHECK_MSG(Counted::ops < 5000, "bulk insert looks quadratic");
    CHECK_EQ(v[250].v, 7);
    CHECK_EQ(v[349].v, 7);
    CHECK_EQ(v[350].v, 250);
}

TEST("vector/insert range correctness and linearity") {
    std::vector<Counted> src;
    for (int i = 0; i < 100; ++i) src.push_back(Counted(1000 + i));

    std::vector<Counted> v;
    v.reserve(2000);
    for (int i = 0; i < 500; ++i) v.push_back(Counted(i));

    Counted::ops = 0;
    v.insert(v.begin() + 250, src.begin(), src.end());
    CHECK_EQ(v.size(), (std::size_t)600);
    CHECK_MSG(Counted::ops < 5000, "range insert looks quadratic");
    CHECK_EQ(v[250].v, 1000);
    CHECK_EQ(v[349].v, 1099);
    CHECK_EQ(v[350].v, 250);
}

TEST("vector/insert self range is safe") {
    std::vector<int> v;
    for (int i = 0; i < 5; ++i) v.push_back(i);          // 0 1 2 3 4
    v.insert(v.begin() + 2, v.begin(), v.begin() + 3);   // 插入 0 1 2
    const int expect[] = {0, 1, 0, 1, 2, 2, 3, 4};
    CHECK_EQ(v.size(), (std::size_t)8);
    for (int i = 0; i < 8; ++i) CHECK_EQ(v[(std::size_t)i], expect[i]);
}

TEST("vector/insert at end with n copies") {
    std::vector<int> v;
    v.push_back(1);
    v.insert(v.end(), (std::size_t)4, 2);
    CHECK_EQ(v.size(), (std::size_t)5);
    CHECK_EQ(v[0], 1);
    for (int i = 1; i < 5; ++i) CHECK_EQ(v[(std::size_t)i], 2);
}
