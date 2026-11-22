// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <stdarg.h>
#include <stddef.h>

void e9_putc(char c);
void e9_print(const char *msg);
void e9_puts(const char *msg);
void e9_printf(const char *format, ...);
void e9_printNL(const char *format, ...);
