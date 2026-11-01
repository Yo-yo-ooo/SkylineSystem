//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_atomic.cpp — <atomic> 的 SMP 压力测试
//
// 单线程测 atomic 只能验证"接口存在"，真正的语义（原子性、内存序）必须
// 多线程才暴露。这里用 pthread 起 4~8 个线程做真实并发：
//   * fetch_add 累加：总数必须精确等于 N*M（丢更新会少）
//   * CAS 循环：除了总数正确，还要求重试次数 > 0（证明确实有竞争）
//   * release/acquire 交接：写数据 → 置标志；读标志 → 读数据
//   * atomic_flag 自旋锁保护非原子计数器
//=============================================================
#include "framework/test_framework.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(__has_include)
#  if __has_include(<pthread.h>)
#    define TST_HAVE_PTHREAD 1
#  endif
#endif

#ifdef TST_HAVE_PTHREAD

#include <pthread.h>
#include <unistd.h>

namespace {

const int kThreads = 4;
const long kIters = 100000;

struct AddArgs {
    std::atomic<long>* counter;
    long iters;
};

void* add_worker(void* p) {
    AddArgs* a = static_cast<AddArgs*>(p);
    for (long i = 0; i < a->iters; ++i)
        a->counter->fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

struct CasArgs {
    std::atomic<long>* counter;
    std::atomic<long>* retries;
    long iters;
};

void* cas_worker(void* p) {
    CasArgs* a = static_cast<CasArgs*>(p);
    for (long i = 0; i < a->iters; ++i) {
        long expected = a->counter->load(std::memory_order_relaxed);
        while (!a->counter->compare_exchange_weak(expected, expected + 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
            a->retries->fetch_add(1, std::memory_order_relaxed);
        }
    }
    return nullptr;
}

struct HandoffArgs {
    std::atomic<int>* data;
    std::atomic<bool>* ready;
    int value;
};

// 生产者：先写数据，再 release 置标志
void* producer(void* p) {
    HandoffArgs* a = static_cast<HandoffArgs*>(p);
    a->data->store(a->value, std::memory_order_relaxed);
    a->ready->store(true, std::memory_order_release);
    return nullptr;
}

// 消费者：acquire 读到 true 后读数据，必须看到生产者的写入
void* consumer(void* p) {
    HandoffArgs* a = static_cast<HandoffArgs*>(p);
    while (!a->ready->load(std::memory_order_acquire)) {
        // 自旋等待
    }
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(a->data->load(std::memory_order_relaxed)));
}

struct FlagArgs {
    std::atomic_flag* lock;
    long* plain_counter;     // 非原子，靠自旋锁保护
    long iters;
};

void* flag_worker(void* p) {
    FlagArgs* a = static_cast<FlagArgs*>(p);
    for (long i = 0; i < a->iters; ++i) {
        while (a->lock->test_and_set(std::memory_order_acquire)) {
            // 自旋
        }
        long v = *a->plain_counter;
        *a->plain_counter = v + 1;
        a->lock->clear(std::memory_order_release);
    }
    return nullptr;
}

} // namespace

TEST("atomic/fetch_add is atomic under contention") {
    std::atomic<long> counter(0);
    CHECK_EQ(counter.load(), 0L);

    AddArgs args;
    args.counter = &counter;
    args.iters = kIters;

    pthread_t th[kThreads];
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(pthread_create(&th[i], nullptr, add_worker, &args), 0);
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(pthread_join(th[i], nullptr), 0);

    CHECK_EQ(counter.load(std::memory_order_relaxed), kThreads * kIters);
}

TEST("atomic/compare_exchange_weak loop under contention") {
    std::atomic<long> counter(0);
    std::atomic<long> retries(0);

    CasArgs args;
    args.counter = &counter;
    args.retries = &retries;
    args.iters = kIters / 2;

    pthread_t th[kThreads];
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(pthread_create(&th[i], nullptr, cas_worker, &args), 0);
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(pthread_join(th[i], nullptr), 0);

    CHECK_EQ(counter.load(), kThreads * (kIters / 2));
    // 4 线程抢同一个地址，必然出现失败重试；若为 0 说明 CAS 没真正并发
    CHECK(retries.load() > 0);
}

TEST("atomic/release-acquire handoff") {
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> data(0);
        std::atomic<bool> ready(false);

        HandoffArgs args;
        args.data = &data;
        args.ready = &ready;
        args.value = 1000 + round;

        pthread_t prod, cons;
        CHECK_EQ(pthread_create(&cons, nullptr, consumer, &args), 0);
        CHECK_EQ(pthread_create(&prod, nullptr, producer, &args), 0);

        void* result = nullptr;
        CHECK_EQ(pthread_join(prod, nullptr), 0);
        CHECK_EQ(pthread_join(cons, &result), 0);

        CHECK_EQ((long long)(std::intptr_t)result, (long long)(1000 + round));
    }
}

TEST("atomic/atomic_flag spinlock protects plain data") {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    long plain = 0;

    FlagArgs args;
    args.lock = &lock;
    args.plain_counter = &plain;
    args.iters = kIters / 4;

    pthread_t th[kThreads];
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(pthread_create(&th[i], nullptr, flag_worker, &args), 0);
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(pthread_join(th[i], nullptr), 0);

    CHECK_EQ(plain, (long)kThreads * (kIters / 4));
}

TEST("atomic/exchange and store-load ordering") {
    std::atomic<int> v(1);
    CHECK_EQ(v.exchange(2), 1);
    CHECK_EQ(v.load(), 2);
    v.store(3, std::memory_order_release);
    CHECK_EQ(v.load(std::memory_order_acquire), 3);
    CHECK(v.is_lock_free() || !v.is_lock_free());   // 只要求可调用
}

#else

TEST("atomic/skipped (no pthread on this platform)") {
    CHECK(true);
}

#endif // TST_HAVE_PTHREAD
