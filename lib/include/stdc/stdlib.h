//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef _STDLIB_H_
#define _STDLIB_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

/* restrict C89 / C++ No restrict keyword! */
#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L) || defined(__cplusplus)
#  ifndef restrict
#    define restrict
#  endif
#endif

/* C89 Basic Functions: strtol strtoul */
#if defined(__STDC__)

long int strtol(const char* restrict nptr, char** restrict endptr, int base);
unsigned long int strtoul(const char* restrict nptr, char** restrict endptr, int base);
void* bsearch(const void* key, void* base, size_t nmemb, size_t size,
               int (*compar)(const void* , const void* ));
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void* , const void* ));

#endif /* __STDC__ C89 */

/* C99 Added:atoll, strtoll, strtoull, strtof, strtod, strtold */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)

long long int atoll(const char* nptr);

double strtod(const char* restrict nptr, char** restrict endptr);
float strtof(const char* restrict nptr, char** restrict endptr);
long double strtold(const char* restrict nptr, char** restrict endptr);

long long int strtoll(const char* restrict nptr, char** restrict endptr, int base);
unsigned long long int strtoull(const char* restrict nptr,
                                char** restrict endptr, int base);

#endif /* C99 */

/* C23 Added: strfromd strfromf strfroml */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)

int strfromd(char* restrict s, size_t n, const char* restrict format, double fp);
int strfromf(char* restrict s, size_t n, const char* restrict format, float fp);
int strfroml(char* restrict s, size_t n, const char* restrict format, long double fp);

#endif /* C23 */

#ifdef __cplusplus
}
#endif

#endif