#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../../memory/heap.h"

int strcmp(const char* str1, const char* str2);
void memcpy(void* src, void* dest, uint64_t size);
size_t strlen(const char* str);
void *malloc(size_t size);
void free(void *ptr);
int toupper(int c);