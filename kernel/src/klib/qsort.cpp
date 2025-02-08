#pragma once

#include <klib/klib.h>

void qsort(void *base, size_t num, size_t width, int32_t (*sort)(const void *e1, const void *e2)) {
    for (int32_t i = 0; i < (int32_t)num - 1; i++) {
        for (int32_t j = 0; j < (int32_t)num - 1 - i; j++) {
            if (sort((char *)base + j * width, (char *)base + (j + 1) * width) > 0) {
                for (int32_t i = 0; i < (int32_t)width; i++) {
                    char temp                           = ((char *)base + j * width)[i];
                    ((char *)base + j * width)[i]       = ((char *)base + (j + 1) * width)[i];
                    ((char *)base + (j + 1) * width)[i] = temp;
                }
            }
        }
    }
}