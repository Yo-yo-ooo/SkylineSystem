//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// hw2: portable hosted test for the scanf / sscanf family. The desktop window
// manager creates the window and paints its decoration; libc routes stdout and
// stdin into the shared content area. This file uses no Skyline-specific
// headers beyond the C library and builds unchanged on a hosted toolchain.

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

static int failures = 0;

static void check(const char *name, int ok) {
    printf("%s %s\n", name, ok ? "OK" : "FAIL");
    if (!ok) failures++;
}

static void self_tests(void) {
    int a = 0, b = 0;
    unsigned u = 0, u2 = 0;
    char w[32];

    check("int",   sscanf("42", "%d", &a) == 1 && a == 42);
    check("hex",   sscanf("ff 077", "%x %o", &u, &u2) == 2
                   && u == 255u && u2 == 63u);
    check("ibase", sscanf("0x10 010", "%i %i", &a, &b) == 2
                   && a == 16 && b == 8);
    check("supp",  sscanf("1 2", "%*d %d", &a) == 1 && a == 2);
    check("width", sscanf("12345", "%3d", &a) == 1 && a == 123);

    float f = 0.0f;
    check("float", sscanf("3.14", "%f", &f) == 1 && f > 3.13f && f < 3.15f);

    double d = 0.0;
    check("dbl",   sscanf("1e3", "%lf", &d) == 1 && d > 999.0 && d < 1001.0);

    double hf = 0.0, hf2 = 0.0;
    check("hexf",  sscanf("0x1.8p3", "%la", &hf) == 1 && hf == 12.0);
    check("hexf2", sscanf("0X1p-2", "%la", &hf2) == 1 && hf2 == 0.25);

    check("set",   sscanf("abc123", "%[a-z]", w) == 1
                   && w[0] == 'a' && w[1] == 'b' && w[2] == 'c' && w[3] == '\0');
    check("negset",sscanf("hello,world", "%[^,]", w) == 1
                   && w[0] == 'h' && w[4] == 'o' && w[5] == '\0');

    int n = -1;
    check("pctn",  sscanf("abc def", "%s%n", w, &n) == 1 && n == 3);

    void *p = NULL;
    check("pctp",  sscanf("0x1234", "%p", &p) == 1 && p == (void *)0x1234);

    long long ll = 0;
    check("lmod",  sscanf("1234567890123", "%lld", &ll) == 1
                   && ll == 1234567890123LL);

    size_t z = 0;
    check("zmod",  sscanf("555", "%zu", &z) == 1 && z == 555u);
}

int main(void) {
    printf("Hello World\n");
    printf("Skyline userspace console\n");
    printf("shared framebuffer OK\n");

    self_tests();
    if (failures == 0) printf("sscanf: all tests passed\n");
    else               printf("sscanf: %d test(s) failed\n", failures);

    char name[64];
    int  number = 0;
    float val = 0.0f;

    printf("What is your name? ");
    if (scanf("%63s", name) == 1)
        printf("Hello, %s!\n", name);

    printf("Type an integer: ");
    if (scanf("%d", &number) == 1)
        printf("%d * 2 = %d\n", number, number * 2);

    printf("Type a decimal (e.g. 3.14): ");
    if (scanf("%f", &val) == 1)
        printf("x100 = %d\n", (int)(val * 100.0f));

    while (true);
    return 0;
}
