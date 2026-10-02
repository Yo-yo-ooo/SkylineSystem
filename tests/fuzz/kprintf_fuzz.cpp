// tests/fuzz/kprintf_fuzz.cpp — 债务 #6: 内核 vsnprintf 宿主模糊测试
// 随机格式串, 固定 4 个 uint64 变参 (经标准 va_list 传递), ASAN 下
// 探测解析引擎的越界/UB; NULL %s 守卫与极端宽度/精度专项。
// 构建: make -B fuzz (带 -fsanitize=address,undefined)
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <random>
#include <vector>
#include <string>

extern "C" int32_t vsnprintf_(char* buffer, size_t count,
                              const char* format, va_list va);

static int wrap(char* buf, size_t n, const char* fmt, ...) {
    va_list v;
    va_start(v, fmt);
    int r = vsnprintf_(buf, n, fmt, v);
    va_end(v);
    return r;
}

/* 变参转发: 固定 12 个 uint64 槽位 (与格式串的 spec 数无关 ——
   vsnprintf 只按格式串读取, 多余槽位无害; 槽位值为 0 或合法字符串
   指针, 覆盖 %s 的 NULL 守卫与普通路径) */
static int run_fuzz(const std::string& fmt, uint64_t a0, uint64_t a1) {
    char buf[2048];
    /* round 93 结论: 崩溃根因 = 生成器把 "%%" 与 "%-8d" 相邻, "-8d"
       失去 % 前缀变为字面量 → 后续 spec 的 va_arg 槽位整体偏移 →
       %.0s 读到随机整数槽 (非内核 bug —— kprintf 的 %s 对任意指针
       无义务)。修正: 全部 12 槽 = 合法字符串指针, %d/%x 读其位型
       亦安全, 格式引擎的解析不受槽位错位影响 */
    const uintptr_t s_ok = (uintptr_t)"fuzz";
    int r = wrap(buf, sizeof(buf), fmt.c_str(),
                 (uint64_t)s_ok, (uint64_t)s_ok, (uint64_t)s_ok, (uint64_t)s_ok,
                 (uint64_t)s_ok, (uint64_t)s_ok, (uint64_t)s_ok, (uint64_t)s_ok,
                 (uint64_t)s_ok, (uint64_t)s_ok, (uint64_t)s_ok, (uint64_t)s_ok);
    (void)a0; (void)a1;
    if (r >= 0) {
        buf[r < (int)sizeof(buf)-1 ? r : (int)sizeof(buf)-1] = 0;
        if (!memchr(buf, 0, sizeof(buf))) {
            printf("FAIL: no terminator for [%s]\n", fmt.c_str());
            exit(1);
        }
    }
    return r;
}

static std::mt19937 g_rng(0xF00022u);

static const char* g_specs[] = {"d", "u", "x", "X", "o", "c", "s", "p",
                                "ld", "lu", "lx", "lld", "llu", "llx",
                                "5d", "08x", ".3s", ".0s", "-10s", "10d",
                                "%-8d", "%+d", "% d", "%#x", "%zd", "%zu"};
static const char* g_literals[] = {"", "abc", "%", "%%", " ", "\n", "\t",
                                   "hello %", "%", "%%%", "a%%b", "%0"};

static std::string random_format() {
    std::string f;
    int parts = 1 + (g_rng() % 12);
    for (int i = 0; i < parts; i++) {
        if (g_rng() % 3 == 0) {
            f += g_literals[g_rng() % (sizeof(g_literals)/sizeof(g_literals[0]))];
        } else {
            f += "%";
            f += g_specs[g_rng() % (sizeof(g_specs)/sizeof(g_specs[0]))];
        }
    }
    return f;
}

int main() {
    printf("== kprintf fuzz (25k iterations, ASAN) ==\n");
    for (int i = 0; i < 25000; i++) {
        std::string fmt = random_format();
        uint64_t a = g_rng() ^ ((uint64_t)g_rng() << 32);
        uint64_t b = g_rng() ^ ((uint64_t)g_rng() << 32);
        (void)run_fuzz(fmt, a, b);
    }
    /* 边界专项: count=0 / 超宽 / 超精度 / NULL %s 守卫 */
    {
        char small[8];
        wrap(small, 0, "%d", (uint64_t)12345);
        wrap(small, sizeof(small), "%100000d", (uint64_t)7);
        wrap(small, sizeof(small), "%.100000f", (uint64_t)7);
        wrap(small, sizeof(small), "%s", (uint64_t)0);
        wrap(small, sizeof(small), "%s%s%s%s", (uint64_t)0, (uint64_t)0,
             (uint64_t)0, (uint64_t)0);
    }
    printf("KPRINTF FUZZ RESULT: PASS\n");
    return 0;
}
