//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_memory.cpp — allocator / unique_ptr / shared_ptr / weak_ptr
//
// 全部通过框架替换的 ::operator new 分配，用例结束时由框架检查净泄漏，
// 因此“智能指针是否真的释放”是硬断言，不依赖 valgrind。
//=============================================================
#include "framework/test_framework.h"
#include <memory>
#include <new>
#include <cstddef>

namespace {

struct Obj {
    static int live;
    int v;
    explicit Obj(int x = 0) : v(x) { ++live; }
    ~Obj() { --live; }
    static void reset() { live = 0; }
};
int Obj::live = 0;

struct CountingDeleter {
    int* calls;
    explicit CountingDeleter(int* c) : calls(c) {}
    void operator()(Obj* p) const { ++(*calls); delete p; }
};

} // namespace

TEST("memory/allocator basic") {
    std::allocator<Obj> a;
    Obj* p = a.allocate(3);
    CHECK(p != nullptr);
    a.deallocate(p, 3);
}

TEST("memory/unique_ptr owns and releases") {
    Obj::reset();
    {
        std::unique_ptr<Obj> p(new Obj(5));
        CHECK(static_cast<bool>(p));
        CHECK_EQ(p->v, 5);
        CHECK_EQ(Obj::live, 1);
    }
    CHECK_EQ(Obj::live, 0);

    {
        std::unique_ptr<Obj> p(new Obj(1));
        Obj* raw = p.get();
        Obj* rel = p.release();
        CHECK_EQ(raw, rel);
        CHECK(!p);
        CHECK_EQ(Obj::live, 1);
        delete rel;
        CHECK_EQ(Obj::live, 0);
    }

    {
        std::unique_ptr<Obj> p(new Obj(2));
        p.reset(new Obj(3));
        CHECK_EQ(p->v, 3);
        CHECK_EQ(Obj::live, 1);
        p.reset();
        CHECK_EQ(Obj::live, 0);
    }
}

TEST("memory/unique_ptr move semantics") {
    Obj::reset();
    {
        std::unique_ptr<Obj> a(new Obj(9));
        std::unique_ptr<Obj> b(static_cast<std::unique_ptr<Obj>&&>(a));
        CHECK(!a);
        CHECK_EQ(b->v, 9);
        CHECK_EQ(Obj::live, 1);
    }
    CHECK_EQ(Obj::live, 0);
}

TEST("memory/unique_ptr custom deleter") {
    Obj::reset();
    int calls = 0;
    {
        std::unique_ptr<Obj, CountingDeleter> p(new Obj(1), CountingDeleter(&calls));
        CHECK_EQ(Obj::live, 1);
    }
    CHECK_EQ(calls, 1);
    CHECK_EQ(Obj::live, 0);
}

TEST("memory/shared_ptr refcount and weak_ptr") {
    Obj::reset();
    {
        std::shared_ptr<Obj> a(new Obj(1));
        CHECK_EQ(a.use_count(), 1L);
        {
            std::shared_ptr<Obj> b = a;
            CHECK_EQ(a.use_count(), 2L);
            CHECK_EQ(b.use_count(), 2L);

            std::weak_ptr<Obj> w = a;
            CHECK(!w.expired());
            CHECK_EQ(w.use_count(), 2L);

            std::shared_ptr<Obj> c = w.lock();
            CHECK_EQ(a.use_count(), 3L);
            CHECK_EQ(c->v, 1);
        }
        CHECK_EQ(a.use_count(), 1L);
    }
    CHECK_EQ(Obj::live, 0);

    // 对象销毁后 weak 必须过期
    std::weak_ptr<Obj> dangling;
    {
        std::shared_ptr<Obj> s(new Obj(2));
        dangling = s;
        CHECK(!dangling.expired());
    }
    CHECK(dangling.expired());
    CHECK(dangling.lock() == std::shared_ptr<Obj>());
}

TEST("memory/make_shared and make_unique") {
    Obj::reset();
    {
        std::shared_ptr<Obj> s = std::make_shared<Obj>(7);
        CHECK_EQ(s->v, 7);
        CHECK_EQ(Obj::live, 1);

        std::unique_ptr<Obj> u = std::make_unique<Obj>(8);
        CHECK_EQ(u->v, 8);
        CHECK_EQ(Obj::live, 2);
    }
    CHECK_EQ(Obj::live, 0);
}

TEST("memory/shared_ptr reset and swap") {
    Obj::reset();
    {
        std::shared_ptr<Obj> a(new Obj(1));
        std::shared_ptr<Obj> b(new Obj(2));
        CHECK_EQ(Obj::live, 2);
        a.swap(b);
        CHECK_EQ(a->v, 2);
        CHECK_EQ(b->v, 1);
        a.reset();
        CHECK_EQ(Obj::live, 1);
    }
    CHECK_EQ(Obj::live, 0);
}

TEST("memory/addressof and to_address") {
    Obj o(3);
    Obj* p = std::addressof(o);
    CHECK_EQ(p, &o);
#if __cplusplus >= 202002L
    CHECK_EQ(std::to_address(p), p);   // C++20 起提供
#endif
}
