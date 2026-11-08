//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * tests/stdc/test_stdc.c — SkylineSystem 用户态 libc (lib/stdc) 宿主测试
 *
 * 覆盖 task-11 新增/补齐的标准 C 函数:
 *   setjmp/longjmp, string.h (memchr/strstr/strspn/strcspn/strrchr/strpbrk/
 *   strcoll/strerror/memcmp), math.h (asin/log2/log10/sinh/cosh/tanh/frexp/
 *   ldexp/modf + HUGE_VAL[F]), locale.h (C locale), time.h (time/difftime/
 *   clock, 宿主桩注入), stdio.h (BUFSIZ/标准流/fwrite/fflush/fprintf/feof/
 *   ferror/clearerr/fgetc/getc/ungetc/fputs/fgets/freopen), stdlib.h (abort
 *   仅链接检查), signal.h (sig_atomic_t).
 *
 * 说明:
 *   - 本文件只依赖本仓库头文件 + 无原型 POSIX write(2) 输出结果,
 *     不走宿主 stdio, 避免被 glibc 的 printf 宏/符号干扰;
 *   - 文件 I/O 由 tests/stdc/host_fs_shim.c 的内存文件桩提供;
 *   - fprintf 的格式化由 host_printf_shim.c 的 vsnprintf_ (薄封装
 *     glibc vsnprintf) 提供 —— printf.c 依赖 flanterm/graphic, 不能宿主链接。
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <locale.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>

/* Lua luaconf.h 的 C99 配置依赖 HUGE_VALF 存在 */
#ifndef HUGE_VALF
#error "math.h must define HUGE_VALF (Lua luaconf.h C99 path)"
#endif

/* 无原型 POSIX write(2): 测试输出不依赖宿主 stdio */
extern long write(int fd, const void *buf, unsigned long count);

/* host_fs_shim.c 提供 */
extern void shim_console_reset(void);
extern unsigned long shim_console_len(void);
extern const char *shim_console_data(void);

/* time.c 宿主桩 (编译 time.c 时通过 -DSKY_*_SOURCE 注入) */
long host_rtc_seconds(void);
long host_monotonic_ms(void);

long host_rtc_seconds(void) { return 1700000000L; }
long host_monotonic_ms(void) { return 1234L; }

/* libm 的 long double 参考实现 (不覆盖本仓库的 double 版本) */
extern long double asinl(long double);
extern long double log2l(long double);
extern long double log10l(long double);
extern long double sinhl(long double);
extern long double coshl(long double);
extern long double tanhl(long double);

/* ------------------------------------------------------------------ */
/* 迷你测试框架                                                         */
/* ------------------------------------------------------------------ */
static int g_pass = 0;
static int g_fail = 0;

static unsigned long sky_slen(const char *s) {
    unsigned long n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void out_str(int fd, const char *s) {
    long r = write(fd, s, sky_slen(s));
    (void)r;
}

static void out_uint(int fd, unsigned long v) {
    char tmp[24];
    char out[24];
    int i = 0;
    int j = 0;

    if (v == 0) {
        out_str(fd, "0");
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (i > 0) out[j++] = tmp[--i];
    long r = write(fd, out, (unsigned long)j);
    (void)r;
}

#define SKY_STR2(x) #x
#define SKY_STR(x) SKY_STR2(x)

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            out_str(2, "FAIL " __FILE__ ":" SKY_STR(__LINE__) ": " #cond "\n"); \
        }                                                                      \
    } while (0)

static double dabs(double x) { return x < 0.0 ? -x : x; }

/* 相对/绝对混合容差: 期望值大时按相对误差, 小时按绝对误差 */
static int near_ok(double actual, double expected, double tol) {
    double err;
    double mag;

    if (__builtin_isnan(actual) || __builtin_isnan(expected)) return 0;
    if (__builtin_isinf(actual) || __builtin_isinf(expected))
        return actual == expected;
    err = dabs(actual - expected);
    mag = dabs(expected);
    if (mag < 1.0) mag = 1.0;
    return err <= tol * mag;
}

static void check_near_impl(const char *what, double actual, double expected,
                            double tol, int line) {
    if (near_ok(actual, expected, tol)) {
        g_pass++;
    } else {
        g_fail++;
        out_str(2, "FAIL near " __FILE__ ":" );
        out_uint(2, (unsigned long)line);
        out_str(2, ": ");
        out_str(2, what);
        out_str(2, "\n");
    }
}

#define CHECK_NEAR(actual, expected, tol) \
    check_near_impl(#actual, (actual), (expected), (tol), __LINE__)

/* ------------------------------------------------------------------ */
/* setjmp / longjmp                                                     */
/* ------------------------------------------------------------------ */
static jmp_buf g_env;

static void jump_helper(int val) {
    longjmp(g_env, val);
}

static void test_setjmp(void) {
    volatile int marker = 100;
    int r;

    /* 正常返回 0 */
    r = setjmp(g_env);
    if (r == 0) {
        marker = 200;
        jump_helper(42);
        CHECK(0 && "longjmp should not return");
    } else {
        CHECK(r == 42);
        CHECK(marker == 200);
    }

    /* val == 0 必须变成 1 */
    r = setjmp(g_env);
    if (r == 0) jump_helper(0);
    CHECK(r == 1);

    /* 嵌套: 内层 longjmp 不影响外层 env */
    {
        jmp_buf outer;
        jmp_buf inner;
        volatile int outer_ok = 0;
        volatile int inner_ok = 0;
        int ro = setjmp(outer);

        if (ro == 0) {
            int ri = setjmp(inner);
            if (ri == 0) {
                longjmp(inner, 7);
            }
            CHECK(ri == 7);
            inner_ok = 1;
            longjmp(outer, 9);
        } else {
            CHECK(ro == 9);
            outer_ok = 1;
        }
        CHECK(outer_ok == 1);
        CHECK(inner_ok == 1);
    }
}

/* ------------------------------------------------------------------ */
/* string.h                                                             */
/* ------------------------------------------------------------------ */
static void test_string(void) {
    static const char hello[] = "hello world";
    static const char abc[] = "abc";
    static const char aaa[] = "aaa";
    const char *p;
    const char *r;

    /* memcmp / memchr */
    CHECK(memcmp("abc", "abd", 3) < 0);
    CHECK(memcmp("abd", "abc", 3) > 0);
    CHECK(memcmp("abc", "abc", 3) == 0);
    CHECK(memcmp("abc", "abd", 0) == 0);
    CHECK(memchr(hello, 'd', 11) == (const void *)(hello + 10));
    CHECK(memchr(hello, 'z', 11) == NULL);
    CHECK(memchr(abc, 'c', 2) == NULL);
    CHECK(memchr(abc, '\0', 4) == (const void *)(abc + 3));

    /* strstr */
    CHECK(strstr(hello, "world") == hello + 6);
    CHECK(strstr(hello, "") == hello);
    CHECK(strstr(hello, "lo") == hello + 3);
    CHECK(strstr(hello, "xyz") == NULL);
    p = strstr(aaa, "aa");
    CHECK(p == aaa);                    /* 重叠匹配取最左 */
    CHECK(strstr("", "") != NULL);
    CHECK(strstr("", "a") == NULL);
    CHECK(strstr("hello", "hello world") == NULL);

    /* strspn / strcspn */
    CHECK(strspn("abcabc", "abc") == (size_t)6);
    CHECK(strspn("abcz", "abc") == (size_t)3);
    CHECK(strspn("xyzabc", "abc") == (size_t)0);
    CHECK(strspn("abc", "") == (size_t)0);
    CHECK(strcspn("abcxyz", "xy") == (size_t)3);
    CHECK(strcspn("abc", "xyz") == (size_t)3);
    CHECK(strcspn("", "abc") == (size_t)0);
    CHECK(strcspn("abc", "") == (size_t)3);

    /* strrchr */
    r = strrchr("abcb", 'b');
    CHECK(r != NULL && strcmp(r, "b") == 0);
    CHECK(strrchr("abcb", 'z') == NULL);
    CHECK(strrchr(abc, '\0') == abc + 3);

    /* strpbrk */
    CHECK(strpbrk(hello, "ow") == hello + 4);
    CHECK(strpbrk(hello, "xyz") == NULL);
    CHECK(strpbrk("", "abc") == NULL);

    /* strcoll: C locale 下与 strcmp 一致 */
    CHECK(strcoll("abc", "abd") < 0);
    CHECK(strcoll("abc", "abc") == 0);
    CHECK(strcoll("abd", "abc") > 0);

    /* strerror */
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0);
    CHECK(strstr(strerror(EDOM), "domain") != NULL);
    CHECK(strcmp(strerror(ERANGE), "Numerical result out of range") == 0);
    CHECK(strcmp(strerror(0), "Success") == 0);
    CHECK(strcmp(strerror(123456), "Unknown error") == 0);
}

/* ------------------------------------------------------------------ */
/* math.h 新增函数                                                      */
/* ------------------------------------------------------------------ */
static void test_math(void) {
    int e;
    double ip;
    double x;
    double f;

    /* asin */
    CHECK_NEAR(asin(0.0), 0.0, 1e-15);
    CHECK_NEAR(asin(0.5), 0.5235987755982988, 1e-12);
    CHECK_NEAR(asin(-0.5), -0.5235987755982988, 1e-12);
    CHECK_NEAR(asin(1.0), 1.5707963267948966, 1e-15);
    CHECK_NEAR(asin(-1.0), -1.5707963267948966, 1e-15);
    CHECK_NEAR(asin(0.999999), (double)asinl(0.999999L), 1e-12);
    CHECK_NEAR(asin(-0.9), (double)asinl(-0.9L), 1e-12);
    errno = 0;
    CHECK(__builtin_isnan(asin(2.0)));
    CHECK(errno == EDOM);
    errno = 0;
    CHECK(__builtin_isnan(asin(-1.000001)));
    CHECK(errno == EDOM);

    /* log2 */
    CHECK_NEAR(log2(1.0), 0.0, 1e-15);
    CHECK_NEAR(log2(2.0), 1.0, 1e-12);
    CHECK_NEAR(log2(8.0), 3.0, 1e-12);
    CHECK_NEAR(log2(0.5), -1.0, 1e-12);
    CHECK_NEAR(log2(1024.0), 10.0, 1e-12);
    CHECK_NEAR(log2(3.0), (double)log2l(3.0L), 1e-12);
    CHECK_NEAR(log2(1e-300), (double)log2l(1e-300L), 1e-12);
    CHECK_NEAR(log2(1e300), (double)log2l(1e300L), 1e-12);
    errno = 0;
    CHECK(log2(0.0) == -HUGE_VAL);
    CHECK(errno == ERANGE);
    errno = 0;
    CHECK(__builtin_isnan(log2(-1.0)));
    CHECK(errno == EDOM);
    CHECK(log2((double)INFINITY) == (double)INFINITY);

    /* log10 */
    CHECK_NEAR(log10(1.0), 0.0, 1e-15);
    CHECK_NEAR(log10(1000.0), 3.0, 1e-12);
    CHECK_NEAR(log10(0.001), -3.0, 1e-12);
    CHECK_NEAR(log10(2.0), (double)log10l(2.0L), 1e-12);
    CHECK_NEAR(log10(7.5e-200), (double)log10l(7.5e-200L), 1e-12);
    errno = 0;
    CHECK(log10(0.0) == -HUGE_VAL);
    CHECK(errno == ERANGE);
    errno = 0;
    CHECK(__builtin_isnan(log10(-1.0)));
    CHECK(errno == EDOM);

    /* sinh / cosh / tanh */
    CHECK_NEAR(sinh(0.0), 0.0, 0.0);
    CHECK_NEAR(cosh(0.0), 1.0, 0.0);
    CHECK_NEAR(tanh(0.0), 0.0, 0.0);
    CHECK_NEAR(sinh(1.0), 1.1752011936438014, 1e-12);
    CHECK_NEAR(cosh(1.0), 1.5430806348152437, 1e-12);
    CHECK_NEAR(tanh(1.0), 0.7615941559557649, 1e-12);
    CHECK_NEAR(sinh(-1.0), -1.1752011936438014, 1e-12);
    CHECK_NEAR(cosh(-1.0), 1.5430806348152437, 1e-12);
    CHECK_NEAR(tanh(-1.0), -0.7615941559557649, 1e-12);
    CHECK_NEAR(sinh(1e-8), (double)sinhl(1e-8L), 1e-12);
    CHECK_NEAR(tanh(1e-8), (double)tanhl(1e-8L), 1e-12);
    CHECK_NEAR(sinh(3.5), (double)sinhl(3.5L), 1e-12);
    CHECK_NEAR(cosh(3.5), (double)coshl(3.5L), 1e-12);
    CHECK_NEAR(tanh(3.5), (double)tanhl(3.5L), 1e-12);
    /* sinh(710) 仍有限 (e^710/2 < DBL_MAX), sinh(711) 上溢 */
    CHECK_NEAR(sinh(710.0), (double)sinhl(710.0L), 1e-12);
    errno = 0;
    CHECK(__builtin_isinf(sinh(711.0)));
    CHECK(errno == ERANGE);
    errno = 0;
    CHECK(__builtin_isinf(cosh(-711.0)));
    CHECK(errno == ERANGE);
    CHECK(tanh(1000.0) == 1.0);
    CHECK(tanh(-1000.0) == -1.0);
    CHECK(tanh((double)INFINITY) == 1.0);
    CHECK(tanh((double)-INFINITY) == -1.0);

    /* frexp / ldexp: 互逆 + 位级精确性 */
    e = 12345;
    CHECK(frexp(1.0, &e) == 0.5);
    CHECK(e == 1);
    e = 12345;
    CHECK(frexp(0.0, &e) == 0.0);
    CHECK(e == 0);
    e = 12345;
    CHECK(frexp(-0.25, &e) == -0.5);
    CHECK(e == -1);
    e = 12345;
    CHECK(frexp(8.0, &e) == 0.5);
    CHECK(e == 4);
    e = 12345;
    CHECK(frexp(0x1p-1074, &e) == 0.5);
    CHECK(e == -1073);
    x = 1.7976931348623157e308;
    f = frexp(x, &e);
    CHECK(e == 1024);
    CHECK(ldexp(f, e) == x);

    CHECK(ldexp(0.5, 1) == 1.0);
    CHECK(ldexp(1.0, -1) == 0.5);
    CHECK(ldexp(-2.0, 3) == -16.0);
    CHECK(ldexp(1.0, 1023) == 0x1p1023);
    CHECK(ldexp(1.0, -1074) == 0x1p-1074);
    errno = 0;
    CHECK(ldexp(1.0, 1024) == HUGE_VAL);
    CHECK(errno == ERANGE);
    errno = 0;
    CHECK(ldexp(1.0, -1075) == 0.0);
    CHECK(errno == ERANGE);

    /* modf */
    CHECK(modf(3.75, &ip) == 0.75);
    CHECK(ip == 3.0);
    CHECK(modf(-3.75, &ip) == -0.75);
    CHECK(ip == -3.0);
    CHECK(modf(5.0, &ip) == 0.0);
    CHECK(ip == 5.0);
    CHECK(modf(-0.5, &ip) == -0.5);
    CHECK(ip == 0.0);
    CHECK(1.0 / ip < 0.0);              /* ip 必须是 -0.0 */
    CHECK(modf(0.0, &ip) == 0.0);
    CHECK(ip == 0.0);
    CHECK(modf(4503599627370496.0, &ip) == 0.0);
    CHECK(ip == 4503599627370496.0);
}

/* ------------------------------------------------------------------ */
/* locale.h                                                             */
/* ------------------------------------------------------------------ */
static void test_locale(void) {
    struct lconv *lc;

    CHECK(setlocale(LC_ALL, NULL) != NULL);
    CHECK(strcmp(setlocale(LC_ALL, NULL), "C") == 0);
    CHECK(strcmp(setlocale(LC_ALL, "C"), "C") == 0);
    CHECK(strcmp(setlocale(LC_ALL, ""), "C") == 0);
    CHECK(strcmp(setlocale(LC_NUMERIC, "POSIX"), "C") == 0);
    CHECK(setlocale(LC_ALL, "en_US.UTF-8") == NULL);
    CHECK(setlocale(99, "C") == NULL);
    CHECK(setlocale(-1, "C") == NULL);

    lc = localeconv();
    CHECK(lc != NULL);
    CHECK(lc->decimal_point[0] == '.');
    CHECK(lc->decimal_point[1] == '\0');
    CHECK(lc->thousands_sep[0] == '\0');
    CHECK(lc->mon_decimal_point[0] == '\0');
}

/* ------------------------------------------------------------------ */
/* time.h                                                               */
/* ------------------------------------------------------------------ */
static void test_time(void) {
    time_t t = 0;

    CHECK(time(NULL) == (time_t)1700000000L);
    CHECK(time(&t) == (time_t)1700000000L);
    CHECK(t == (time_t)1700000000L);
    CHECK(difftime((time_t)1700000005L, (time_t)1700000000L) == 5.0);
    CHECK(difftime((time_t)1700000000L, (time_t)1700000005L) == -5.0);
    CHECK(CLOCKS_PER_SEC == 1000000L);
    CHECK(clock() == (clock_t)(1234L * 1000L));
}

/* ------------------------------------------------------------------ */
/* stdio.h                                                              */
/* ------------------------------------------------------------------ */
static void test_stdio(void) {
    FILE *f;
    char buf[64];
    char line[16];

    CHECK(BUFSIZ == 8192);
    CHECK(stdin != NULL);
    CHECK(stdout != NULL);
    CHECK(stderr != NULL);

    /* ---- 写文件 + 读回 ---- */
    f = fopen("stdc_test.txt", "w");
    CHECK(f != NULL);
    CHECK(fwrite("hello\nworld\n", 1, 12, f) == (size_t)12);
    CHECK(fflush(f) == 0);
    CHECK(fclose(f) == 0);

    f = fopen("stdc_test.txt", "r");
    CHECK(f != NULL);
    memset(buf, 0, sizeof(buf));
    CHECK(fread(buf, 1, 5, f) == (size_t)5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK(feof(f) == 0);
    CHECK(ferror(f) == 0);
    CHECK(fgetc(f) == '\n');
    CHECK(getc(f) == 'w');
    CHECK(ungetc('W', f) == 'W');
    CHECK(fgetc(f) == 'W');
    CHECK(getc(f) == 'o');
    CHECK(fgets(line, (int)sizeof(line), f) != NULL);
    CHECK(strcmp(line, "rld\n") == 0);
    /* 读到 EOF: fread 返回 0 且置 eof, 但不置 ferror */
    CHECK(fread(buf, 1, sizeof(buf), f) == (size_t)0);
    CHECK(feof(f) != 0);
    CHECK(ferror(f) == 0);
    clearerr(f);
    CHECK(feof(f) == 0);
    /* fseek 清 eof, 回到开头 */
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    CHECK(ftell(f) == 0);
    CHECK(feof(f) == 0);
    CHECK(fgetc(f) == 'h');
    CHECK(fseek(f, 0, SEEK_END) == 0);
    CHECK(ftell(f) == 12);
    rewind(f);
    CHECK(ftell(f) == 0);
    CHECK(fgetc(f) == 'h');
    CHECK(fclose(f) == 0);

    /* ---- fopen 失败: 内核负 errno 转用户态 errno (Lua lauxlib 需要) ---- */
    errno = 0;
    CHECK(fopen("missing.txt", "r") == NULL);
    CHECK(errno == ENOENT);

    /* ---- freopen: Lua lauxlib 的 "r" -> "rb" 重开路径 ---- */
    f = fopen("stdc_test.txt", "r");
    CHECK(f != NULL);
    CHECK(fgetc(f) == 'h');
    CHECK(freopen("stdc_test.txt", "rb", f) == f);
    CHECK(fgetc(f) == 'h');             /* 位置回到文件头 */
    CHECK(fclose(f) == 0);

    /* ---- fprintf 到文件 ---- */
    f = fopen("stdc_fmt.txt", "w");
    CHECK(f != NULL);
    CHECK(fprintf(f, "%d %s %.2f", 42, "x", 1.5) == 9);
    CHECK(fclose(f) == 0);
    f = fopen("stdc_fmt.txt", "r");
    CHECK(f != NULL);
    memset(buf, 0, sizeof(buf));
    CHECK(fread(buf, 1, sizeof(buf) - 1, f) == (size_t)9);
    buf[9] = '\0';
    CHECK(strcmp(buf, "42 x 1.50") == 0);
    CHECK(fclose(f) == 0);

    /* ---- stdout/stderr 路由到 console_stdout_write 桩 ---- */
    shim_console_reset();
    CHECK(fputs("console-line\n", stdout) >= 0);
    CHECK(fprintf(stderr, "err %d\n", 7) == 6);
    CHECK(putchar('!') == '!');
    CHECK(putc('?', stdout) == '?');
    CHECK(fflush(stdout) == 0);
    CHECK(shim_console_len() == (unsigned long)21);
    CHECK(strcmp(shim_console_data(), "console-line\nerr 7\n!?") == 0);

    /* ---- stdin: 无内核输入源, 读即 EOF ---- */
    clearerr(stdin);
    CHECK(fgetc(stdin) == EOF);
    CHECK(feof(stdin) != 0);
    clearerr(stdin);
    CHECK(fgets(line, (int)sizeof(line), stdin) == NULL);

    /* ---- abort: 不能在测试进程里真调用, 只验证符号可链接 ---- */
    {
        void (*abort_fn)(void) = abort;
        CHECK(abort_fn != NULL);
    }
}

/* ------------------------------------------------------------------ */
/* signal.h (Lua lstate.h 只需要 sig_atomic_t)                          */
/* ------------------------------------------------------------------ */
static void test_signal_header(void) {
    CHECK(sizeof(sig_atomic_t) == sizeof(int));
    CHECK(SIGINT == 2);
    CHECK(SIGABRT == 6);
    CHECK(SIG_DFL != SIG_IGN);
}

int main(void) {
    out_str(1, "== SkylineSystem lib/stdc host tests ==\n");

    test_setjmp();
    test_string();
    test_math();
    test_locale();
    test_time();
    test_stdio();
    test_signal_header();

    out_str(1, "PASS: ");
    out_uint(1, (unsigned long)g_pass);
    out_str(1, "  FAIL: ");
    out_uint(1, (unsigned long)g_fail);
    out_str(1, "\n");

    if (g_fail != 0) {
        out_str(2, "STDC_TEST: FAILED\n");
        return 1;
    }
    out_str(1, "STDC_TEST: ALL PASS\n");
    return 0;
}
