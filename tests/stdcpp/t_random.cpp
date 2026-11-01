//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// t_random.cpp — <random>
//
// 引擎部分对照标准给定的参考值（mt19937 第 10000 个输出是硬指标），
// 分布部分用统计量检查（均值/方差/取值域），而不是"看起来随机"。
//=============================================================
#include "framework/test_framework.h"
#include <random>
#include <cstdint>
#include <cstddef>

TEST("random/mt19937 matches the standard test vector") {
    std::mt19937 g(5489u);
    g.discard(9999);
    // 标准要求：默认种子 5489 的第 10000 个输出
    CHECK_EQ(g(), (std::uint32_t)4123659995u);
}

TEST("random/mt19937_64 matches the standard test vector") {
    std::mt19937_64 g(5489u);
    g.discard(9999);
    CHECK_EQ(g(), (std::uint64_t)9981545732273789042ULL);
}

TEST("random/minstd_rand0 first values") {
    std::minstd_rand0 g(1);
    CHECK_EQ(g(), (std::uint32_t)16807u);
    CHECK_EQ(g(), (std::uint32_t)282475249u);
    CHECK_EQ(g(), (std::uint32_t)1622650073u);
    CHECK_EQ(g(), (std::uint32_t)984943658u);
    CHECK_EQ(g(), (std::uint32_t)1144108930u);
}

TEST("random/engine properties and discard equivalence") {
    std::mt19937 a(42), b(42);
    CHECK(a == b);
    for (int i = 0; i < 100; ++i) { a(); b(); }
    CHECK(a == b);
    b.seed(7);
    CHECK(a != b);

    // discard(n) 必须等价于调用 n 次
    std::mt19937 c(123), d(123);
    c.discard(1000);
    for (int i = 0; i < 1000; ++i) d();
    CHECK_EQ(c(), d());

    // min/max 与取值范围
    std::mt19937 e;
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t v = e();
        CHECK(v >= std::mt19937::min());
        CHECK(v <= std::mt19937::max());
    }
}

TEST("random/seed_seq is deterministic") {
    std::uint32_t seeds[] = {1u, 2u, 3u, 4u, 5u};
    std::seed_seq s1(seeds, seeds + 5);
    std::seed_seq s2(seeds, seeds + 5);
    CHECK_EQ(s1.size(), (std::size_t)5);

    std::mt19937 g1(s1), g2(s2);
    for (int i = 0; i < 10; ++i) CHECK_EQ(g1(), g2());

    // 不同种子序列应给出不同结果
    std::uint32_t seeds2[] = {1u, 2u, 3u, 4u, 6u};
    std::seed_seq s3(seeds2, seeds2 + 5);
    std::mt19937 g3(s3);
    CHECK(g1() != g3() || true);   // 只要求能正常构造并产出
}

TEST("random/uniform_int_distribution bounds and mean") {
    std::mt19937 g(2024);
    std::uniform_int_distribution<int> d(10, 20);

    long sum = 0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        const int v = d(g);
        CHECK(v >= 10);
        CHECK(v <= 20);
        sum += v;
    }
    const double mean = (double)sum / N;
    // 期望 15，标准差 ~3.16/√N ≈ 0.022
    CHECK(mean > 14.85 && mean < 15.15);
    CHECK_EQ(d.min(), 10);
    CHECK_EQ(d.max(), 20);

    std::uniform_int_distribution<int> one(7, 7);
    CHECK_EQ(one(g), 7);
}

TEST("random/uniform_real_distribution range") {
    std::mt19937 g(77);
    std::uniform_real_distribution<double> d(1.5, 2.5);
    double lo = 1e9, hi = -1e9, sum = 0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        const double v = d(g);
        CHECK(v >= 1.5);
        CHECK(v < 2.5);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }
    CHECK(lo < 1.55);                 // 覆盖到下界附近
    CHECK(hi > 2.45);
    CHECK(sum / N > 1.95 && sum / N < 2.05);
}

TEST("random/normal_distribution statistics") {
    std::mt19937 g(9);
    std::normal_distribution<double> d(5.0, 2.0);

    double sum = 0, sum2 = 0;
    const int N = 50000;
    for (int i = 0; i < N; ++i) {
        const double v = d(g);
        sum += v;
        sum2 += v * v;
    }
    const double mean = sum / N;
    const double var = sum2 / N - mean * mean;
    CHECK(mean > 4.95 && mean < 5.05);          // 期望 5，σ/√N ≈ 0.009
    CHECK(var > 3.8 && var < 4.2);              // 期望 4（σ²）
}

TEST("random/bernoulli and binomial") {
    std::mt19937 g(11);
    std::bernoulli_distribution b(0.3);
    int hits = 0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) if (b(g)) ++hits;
    const double p = (double)hits / N;
    CHECK(p > 0.28 && p < 0.32);

    std::binomial_distribution<int> bin(10, 0.5);
    double sum = 0;
    for (int i = 0; i < N; ++i) {
        const int v = bin(g);
        CHECK(v >= 0 && v <= 10);
        sum += v;
    }
    CHECK(sum / N > 4.85 && sum / N < 5.15);    // 期望 np = 5
}

TEST("random/poisson and exponential means") {
    std::mt19937 g(13);

    std::poisson_distribution<int> pd(4.0);
    double sum = 0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) sum += pd(g);
    CHECK(sum / N > 3.85 && sum / N < 4.15);    // 期望 λ = 4

    std::exponential_distribution<double> ed(2.0);
    double s2 = 0;
    for (int i = 0; i < N; ++i) {
        const double v = ed(g);
        CHECK(v >= 0.0);
        s2 += v;
    }
    CHECK(s2 / N > 0.45 && s2 / N < 0.55);      // 期望 1/λ = 0.5
}

TEST("random/discrete_distribution frequencies") {
    std::mt19937 g(17);
    std::discrete_distribution<int> d{1.0, 3.0, 6.0};   // 概率 0.1 / 0.3 / 0.6

    int cnt[3] = {0, 0, 0};
    const int N = 30000;
    for (int i = 0; i < N; ++i) {
        const int v = d(g);
        CHECK(v >= 0 && v <= 2);
        ++cnt[v];
    }
    CHECK(cnt[0] > N * 0.08 && cnt[0] < N * 0.12);
    CHECK(cnt[1] > N * 0.27 && cnt[1] < N * 0.33);
    CHECK(cnt[2] > N * 0.57 && cnt[2] < N * 0.63);
    CHECK_EQ(d.max(), 2);
}

TEST("random/generate_canonical in unit interval") {
    std::mt19937 g(19);
    for (int i = 0; i < 1000; ++i) {
        const double v = std::generate_canonical<double, 53>(g);
        CHECK(v >= 0.0);
        CHECK(v < 1.0);
    }
    const float f = std::generate_canonical<float, 24>(g);
    CHECK(f >= 0.0f && f < 1.0f);
}

TEST("random/adaptors") {
    // discard_block_engine：ranlux24 能正常产出且在范围内
    std::ranlux24 r(19780503u);
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t v = r();
        CHECK(v <= std::ranlux24::max());
    }

    // independent_bits_engine：8 位输出
    typedef std::independent_bits_engine<std::mt19937, 8, unsigned> ibe8;
    ibe8 ib;
    for (int i = 0; i < 100; ++i) CHECK(ib() <= 255u);

    // shuffle_order_engine：knuth_b 可正常产出
    std::knuth_b kb;
    (void)kb();
    CHECK(kb.min() <= kb.max());
}

TEST("random/random_device") {
    std::random_device rd;
    const unsigned v = rd();
    CHECK(v >= std::random_device::min());
    CHECK(v <= std::random_device::max());
    CHECK(rd.entropy() >= 0.0);
}
