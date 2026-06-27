//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <mem/new2.h>
#include <mem/heap.h>

void* _Ymalloc(size_t size, const char* func, const char* file, int32_t line)
{
    return kmalloc(size);
}