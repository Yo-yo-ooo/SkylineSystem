#pragma once

#include <klib/klib.h>

void qsort(void *base, size_t num, size_t width, uint32_t (*sort)(const void *e1, const void *e2)) {
    for (uint32_t i = 0; i < (uint32_t)num - 1; i++) {
        for (uint32_t j = 0; j < (uint32_t)num - 1 - i; j++) {
            if (sort((char *)base + j * width, (char *)base + (j + 1) * width) > 0) {
                for (uint32_t i = 0; i < (uint32_t)width; i++) {
                    char temp                           = ((char *)base + j * width)[i];
                    ((char *)base + j * width)[i]       = ((char *)base + (j + 1) * width)[i];
                    ((char *)base + (j + 1) * width)[i] = temp;
                }
            }
        }
    }
}