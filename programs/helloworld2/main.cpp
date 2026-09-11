//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// hw2: a plain, portable "Hello World". It knows nothing about windows, the
// shared-framebuffer protocol, or Skyline syscalls: the desktop window
// manager creates the window and paints its decoration before launch, and libc
// routes stdout (printf) into the shared inner content area automatically.
// main() returns like any normal hosted C program; the painted window stays
// on screen because the WM holds a reference-counted mapping to the surface.
// This file compiles and runs unchanged on a regular hosted toolchain.

#include <stdio.h>

int main(void) {
    printf("Hello World\n");
    printf("Skyline userspace console\n");
    printf("shared framebuffer OK\n");
    return 0;
}
