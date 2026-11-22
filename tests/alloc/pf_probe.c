// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT

#include <stdio.h>
extern int printf_(const char *fmt, ...);
int main(void) {
    printf_("[%.0s]\n", "hello");
    printf_("[%.2s]\n", "hello");
    printf_("[%s]\n", "hello");
    return 0;
}
