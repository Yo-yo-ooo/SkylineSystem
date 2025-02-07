#pragma once

#include <klib/klib.h>

void qsort(void *base, size_t num, size_t width, int (*sort)(const void *e1, const void *e2)) {
    for (int i = 0; i < (int)num - 1; i++) {
        for (int j = 0; j < (int)num - 1 - i; j++) {
            if (sort((char *)base + j * width, (char *)base + (j + 1) * width) > 0) {
                for (int i = 0; i < (int)width; i++) {
                    char temp                           = ((char *)base + j * width)[i];
                    ((char *)base + j * width)[i]       = ((char *)base + (j + 1) * width)[i];
                    ((char *)base + (j + 1) * width)[i] = temp;
                }
            }
        }
    }
}