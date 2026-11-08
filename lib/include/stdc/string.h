//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#pragma once
#ifndef SKYLINE_LIB_STRING_H
#define SKYLINE_LIB_STRING_H

#include <stdint.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C"{
#endif

int32_t strcmp(const char *cs, const char *ct);
char *strtok(char *str, const char *delim);
char* strchr(const char* str, int32_t c);
char *strcpy(char *strDest, const char *strSrc);
int32_t strncmp(const char* a, const char* b, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
char* strcat(char* dest, const char* source);
size_t strlen(const char*__restrict__  str);
int32_t atoi(char *str);

void *memcpy(void *str1, const void *str2, size_t n);
void *memset(void *str, int c, size_t n);
void *memmove(void *str1, const void *str2, size_t n);
int memcmp(const void *str1, const void *str2, size_t n);
void *memchr(const void *str, int c, size_t n);

/* ---- 内嵌 Lua 5.4 需要的补充函数 (lib/stdc/string_extra.c) ---- */
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *str, const char *accept);
size_t strcspn(const char *str, const char *reject);
char *strrchr(const char *str, int c);
char *strpbrk(const char *str, const char *accept);
char *strerror(int errnum);
int strcoll(const char *str1, const char *str2);


#ifdef __cplusplus
}
#endif

#endif