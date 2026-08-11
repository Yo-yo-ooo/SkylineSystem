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

typedef struct _div_t {
    int quot;
    int rem;
} div_t;

typedef struct _ldiv_t {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long int quot;   /* 商 quotient */
    long long int rem;    /* 余数 remainder */
} lldiv_t;


long int strtol(const char* restrict nptr, char** restrict endptr, int base);
unsigned long int strtoul(const char* restrict nptr, char** restrict endptr, int base);
void* bsearch(const void* key, void* base, size_t nmemb, size_t size,
               int (*compar)(const void* , const void* ));
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void* , const void* ));
int abs(int j);
long int labs(long int j);
long long int llabs(long long int j);
div_t div(int numer, int denom);
ldiv_t ldiv(long int numer, long int denom);
lldiv_t lldiv(long long int numer, long long int denom);

int rand(void);
void srand(unsigned int seed);
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

[[noreturn]] void _Exit();

#else
__attribute__((__noreturn__)) void _Exit();
#endif /* C23 */



#ifdef __cplusplus
}
#endif

#endif