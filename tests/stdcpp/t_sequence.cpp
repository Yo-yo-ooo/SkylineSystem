//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_sequence.cpp — list / forward_list / deque
//=============================================================
#include "framework/test_framework.h"
#include <list>
#include <forward_list>
#include <deque>
#include <stdexcept>

namespace {

struct Tracked {
    static int live;
    static int bad;
    unsigned magic;
    int value;
    explicit Tracked(int v = 0) : magic(0xA5A5u), value(v) { ++live; }
    Tracked(const Tracked& o) : magic(0xA5A5u), value(o.value) { ++live; }
    Tracked(Tracked&& o) noexcept : magic(0xA5A5u), value(o.value) { o.magic = 0xBEEFu; ++live; }
    Tracked& operator=(const Tracked& o) { value = o.value; magic = 0xA5A5u; return *this; }
    ~Tracked() {
        if (magic != 0xA5A5u && magic != 0xBEEFu) ++bad;
        magic = 0;
        --live;
    }
    static void reset() { live = 0; bad = 0; }
};
int Tracked::live = 0;
int Tracked::bad = 0;

} // namespace

TEST("list/basic operations") {
    std::list<int> l;
    CHECK(l.empty());
    l.push_back(2);
    l.push_back(3);
    l.push_front(1);
    CHECK_EQ(l.size(), (std::size_t)3);
    CHECK_EQ(l.front(), 1);
    CHECK_EQ(l.back(), 3);

    int expect[] = {1, 2, 3};
    int i = 0;
    for (std::list<int>::iterator it = l.begin(); it != l.end(); ++it)
        CHECK_EQ(*it, expect[i++]);
    CHECK_EQ(i, 3);

    l.pop_front();
    l.pop_back();
    CHECK_EQ(l.size(), (std::size_t)1);
    CHECK_EQ(l.front(), 2);

    l.clear();
    CHECK(l.empty());
}

TEST("list/element lifetime") {
    Tracked::reset();
    {
        std::list<Tracked> l;
        for (int i = 0; i < 32; ++i) l.push_back(Tracked(i));
        CHECK_EQ(Tracked::live, 32);
        l.clear();
        CHECK_EQ(Tracked::live, 0);
        CHECK_EQ(Tracked::bad, 0);
    }
    CHECK_EQ(Tracked::live, 0);
    CHECK_EQ(Tracked::bad, 0);

    {
        std::list<Tracked> a;
        a.push_back(Tracked(1));
        a.push_back(Tracked(2));
        std::list<Tracked> b(a);
        CHECK_EQ(Tracked::live, 4);
        std::list<Tracked> c(static_cast<std::list<Tracked>&&>(a));
        CHECK_EQ(c.size(), (std::size_t)2);
    }
    CHECK_EQ(Tracked::live, 0);
    CHECK_EQ(Tracked::bad, 0);
}

TEST("list/insert erase and iteration") {
    std::list<int> l;
    for (int i = 0; i < 5; ++i) l.push_back(i);           // 0 1 2 3 4
    std::list<int>::iterator it = l.begin();
    ++it; ++it;                                            // -> 2
    l.insert(it, 99);
    CHECK_EQ(l.size(), (std::size_t)6);
    const int expect[] = {0, 1, 99, 2, 3, 4};
    int i = 0;
    for (std::list<int>::const_iterator p = l.begin(); p != l.end(); ++p)
        CHECK_EQ(*p, expect[i++]);

    it = l.begin();
    ++it; ++it;                                            // -> 99
    it = l.erase(it);
    CHECK_EQ(*it, 2);
    CHECK_EQ(l.size(), (std::size_t)5);

    // 区间删除
    std::list<int>::iterator first = l.begin();
    ++first;
    std::list<int>::iterator last = first;
    ++last; ++last;
    l.erase(first, last);
    const int expect2[] = {0, 3, 4};
    i = 0;
    for (std::list<int>::const_iterator p = l.begin(); p != l.end(); ++p)
        CHECK_EQ(*p, expect2[i++]);
    CHECK_EQ(l.size(), (std::size_t)3);
}

TEST("list/sort reverse unique remove") {
    std::list<int> l;
    l.push_back(3); l.push_back(1); l.push_back(2); l.push_back(2); l.push_back(5);
    l.sort();
    const int sorted[] = {1, 2, 2, 3, 5};
    int i = 0;
    for (std::list<int>::iterator p = l.begin(); p != l.end(); ++p) CHECK_EQ(*p, sorted[i++]);

    l.unique();
    const int uniq[] = {1, 2, 3, 5};
    i = 0;
    for (std::list<int>::iterator p = l.begin(); p != l.end(); ++p) CHECK_EQ(*p, uniq[i++]);

    l.reverse();
    CHECK_EQ(l.front(), 5);
    CHECK_EQ(l.back(), 1);

    l.remove(3);
    CHECK_EQ(l.size(), (std::size_t)3);
    CHECK_EQ(l.front(), 5);
}

TEST("list/splice") {
    std::list<int> a;
    a.push_back(1); a.push_back(2);
    std::list<int> b;
    b.push_back(3); b.push_back(4);

    a.splice(a.end(), b);
    CHECK_EQ(a.size(), (std::size_t)4);
    CHECK(b.empty());
    const int expect[] = {1, 2, 3, 4};
    int i = 0;
    for (std::list<int>::iterator p = a.begin(); p != a.end(); ++p) CHECK_EQ(*p, expect[i++]);
}

TEST("forward_list/basic and insert_after") {
    std::forward_list<int> f;
    CHECK(f.empty());
    f.push_front(3);
    f.push_front(2);
    f.push_front(1);
    CHECK_EQ(f.front(), 1);

    std::size_t n = 0;
    for (std::forward_list<int>::iterator it = f.begin(); it != f.end(); ++it) ++n;
    CHECK_EQ(n, (std::size_t)3);

    f.insert_after(f.begin(), 99);
    CHECK_EQ(*f.begin(), 1);
    CHECK_EQ(*++f.begin(), 99);

    f.erase_after(f.begin());
    CHECK_EQ(*++f.begin(), 2);

    f.pop_front();
    CHECK_EQ(f.front(), 2);

    f.clear();
    CHECK(f.empty());
}

TEST("forward_list/sort reverse remove") {
    std::forward_list<int> f;
    f.push_front(2); f.push_front(5); f.push_front(1); f.push_front(4);
    f.sort();
    const int sorted[] = {1, 2, 4, 5};
    int i = 0;
    for (std::forward_list<int>::iterator it = f.begin(); it != f.end(); ++it)
        CHECK_EQ(*it, sorted[i++]);

    f.reverse();
    CHECK_EQ(f.front(), 5);

    f.remove(4);
    const int after[] = {5, 2, 1};
    i = 0;
    for (std::forward_list<int>::iterator it = f.begin(); it != f.end(); ++it)
        CHECK_EQ(*it, after[i++]);
}

TEST("forward_list/element lifetime") {
    Tracked::reset();
    {
        std::forward_list<Tracked> f;
        for (int i = 0; i < 16; ++i) f.push_front(Tracked(i));
        CHECK_EQ(Tracked::live, 16);
    }
    CHECK_EQ(Tracked::live, 0);
    CHECK_EQ(Tracked::bad, 0);
}

TEST("deque/push pop both ends") {
    std::deque<int> d;
    for (int i = 0; i < 100; ++i) d.push_back(i);
    for (int i = -1; i >= -50; --i) d.push_front(i);

    CHECK_EQ(d.size(), (std::size_t)150);
    CHECK_EQ(d.front(), -50);
    CHECK_EQ(d.back(), 99);

    CHECK_EQ(d[0], -50);
    CHECK_EQ(d[49], -1);
    CHECK_EQ(d[50], 0);
    CHECK_EQ(d[149], 99);

    d.pop_front();
    d.pop_back();
    CHECK_EQ(d.size(), (std::size_t)148);
    CHECK_EQ(d.front(), -49);
    CHECK_EQ(d.back(), 98);
}

TEST("deque/grow beyond block size") {
    std::deque<int> d;
    const int N = 4096;
    for (int i = 0; i < N; ++i) d.push_back(i);
    CHECK_EQ(d.size(), (std::size_t)N);
    bool ok = true;
    for (int i = 0; i < N; ++i) if (d[(std::size_t)i] != i) ok = false;
    CHECK(ok);

    d.clear();
    CHECK(d.empty());
    d.push_back(7);
    CHECK_EQ(d.front(), 7);
    CHECK_EQ(d.back(), 7);
}

TEST("deque/insert erase middle") {
    std::deque<int> d;
    for (int i = 0; i < 6; ++i) d.push_back(i);         // 0..5
    d.insert(d.begin() + 2, 99);
    CHECK_EQ(d.size(), (std::size_t)7);
    CHECK_EQ(d[1], 1);
    CHECK_EQ(d[2], 99);
    CHECK_EQ(d[3], 2);

    d.erase(d.begin() + 2);
    CHECK_EQ(d.size(), (std::size_t)6);
    CHECK_EQ(d[2], 2);

    d.erase(d.begin() + 1, d.begin() + 3);
    const int expect[] = {0, 3, 4, 5};
    CHECK_EQ(d.size(), (std::size_t)4);
    for (int i = 0; i < 4; ++i) CHECK_EQ(d[(std::size_t)i], expect[i]);
}

TEST("deque/at throws") {
    std::deque<int> d;
    d.push_back(1);
    CHECK_EQ(d.at(0), 1);
    CHECK_THROWS(d.at(1), std::out_of_range);
}

TEST("deque/element lifetime") {
    Tracked::reset();
    {
        std::deque<Tracked> d;
        for (int i = 0; i < 64; ++i) d.push_back(Tracked(i));
        CHECK_EQ(Tracked::live, 64);
        d.clear();
        CHECK_EQ(Tracked::live, 0);
        CHECK_EQ(Tracked::bad, 0);
    }
    CHECK_EQ(Tracked::live, 0);
    CHECK_EQ(Tracked::bad, 0);
}
