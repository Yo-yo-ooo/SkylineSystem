/*
* SPDX-License-Identifier: GPL-2.0-only
* File: str.c
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <klib/str.h>
#include <stdbool.h>

char* strcat(char* dest, const char* source){
	if (dest == NULL || source == NULL){		//合法性校验
		return dest;
	}
	char* p = dest;			//将目的数组赋给p
	while (*p != '\0'){		//循环看大小
		p++;
	}
	while (*source != '\0'){			//注意指针的用法
		*p = *source;
		p++;			//依次加加进行连接
		source++;
	}
	*p = '\0';
	return dest;
}

uint8_t strcmp(const char *cs, const char *ct)
{
    uint8_t c1, c2;

    while (1)
    {
        c1 = *cs++;
        c2 = *ct++;
        if (c1 != c2)
            return c1 < c2 ? -1 : 1;
        if (!c1)
            break;
    }
    return 0;
}

char *strtok(char *str, const char *delim)
{
    static char *p = 0;
    if (str != 0)
        p = str;
    else if (p == 0)
        return 0;

    char *start = p;
    while (*p != '\0')
    {
        const char *d = delim;
        while (*d != '\0')
        {
            if (*p == *d)
            {
                *p = '\0';
                p++;
                if (start == p){start = p;continue;}
                return start;
            }
            d++;
        }
        p++;
    }
    if (start == p)
        return 0;
    return start;
}


char* strchr(char* str, int32_t c) {
	for (; *str != 0; ++str) {
		if (*str == c) {
			return str;
		}
	}
	return 0;
}

char *strcpy(char *strDest, const char *strSrc){
    char *address = strDest;
    while( (*strDest++ = * strSrc++) != '\0' ) 
        NULL ;
    return address ;
}

int32_t strncmp(const char* a, const char* b, size_t n) {
    while (true) {
        uint8_t ac = n ? *a : '\0', bc = n ? *b : '\0';
        if (ac == '\0' || bc == '\0' || ac != bc) {
            return (ac > bc) - (ac < bc);
        }
        ++a, ++b, --n;
    }
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *tmp = dest;
    while (n-- > 0 && (*dest++ = *src++) != '\0')
        ;
    return tmp;
}

//From Arty3
#if defined(__GNUC__) || defined (__clang__)
typedef unsigned long int __attribute__ ((__may_alias__)) word_t;
typedef unsigned long int __attribute__ ((__may_alias__)) bytemask_t;
#else
typedef unsigned long int word_t;
typedef unsigned long int bytemask_t;
#endif

size_t	strlen(const char *__restrict__  s)
{
#if defined(__GNUC__) || defined (__clang__)
	register const uintptr_t	s0 = (uintptr_t)s;
	register const word_t		*w = (const word_t *)(((uintptr_t)s) & \
									-((uintptr_t)(sizeof(word_t))));

	register word_t	wi = *w;

	register word_t	m = ((word_t)-1 / 0xff) * 0x7f;

#if __BYTE_ORDER == __LITTLE_ENDIAN
	bytemask_t	register mask = ~(((wi & m) + m) | wi | m) >> (/* CHAR_BIT */ 8 * (s0 % sizeof (word_t)));
#else
	bytemask_t	register mask = ~(((wi & m) + m) | wi | m) << (/* CHAR_BIT */ 8 * (s0 % sizeof (word_t)));
#endif

	if (mask)
	{
#if __BYTE_ORDER == __LITTLE_ENDIAN
# if __SIZEOF_POINTER__ == 8
		return (__builtin_ctzl(mask) / /* CHAR_BIT */ 8);
# else
		return (__builtin_ctzll(mask) / /* CHAR_BIT */ 8);
# endif
#else
# if __SIZEOF_POINTER__ == 8
		return (__builtin_clzl(mask) / /* CHAR_BIT */ 8);
# else
		return (__builtin_clzll(mask) / /* CHAR_BIT */ 8);
# endif
#endif
	}

	do
	{
		wi = *++w;
	} while (((wi - (((word_t)-1 / 0xff) * 0x01)) & ~wi & (((word_t)-1 / 0xff) * 0x80)) == 0);

#if __BYTE_ORDER == __LITTLE_ENDIAN
	wi = (wi - ((word_t)-1 / 0xff) * 0x01) & ~wi & ((word_t)-1 / 0xff) * 0x80;
#else
	word_t rb = ((word_t)-1 / 0xff) * 0x7f;
	wi = ~(((wi & rb) + rb) | wi | rb);
#endif

#if __BYTE_ORDER == __LITTLE_ENDIAN
# if __SIZEOF_POINTER__ == 8
		wi = (__builtin_ctzl(wi) / /* CHAR_BIT */ 8);
# else
		wi = (__builtin_ctzll(wi) / /* CHAR_BIT */ 8);
# endif
#else
# if __SIZEOF_POINTER__ == 8
		wi = (__builtin_clzl(wi) / /* CHAR_BIT */ 8);
# else
		wi = (__builtin_clzll(wi) / /* CHAR_BIT */ 8);
# endif
#endif

	return ((const char *)w) + wi - s;
#endif
}

int32_t atoi(char *str) {
    int32_t result = 0;
    int32_t neg_multiplier = 1;

    // Scrub leading whitespace
    while (*str && (
            (*str == ' ') ||
            (*str == '\t'))) 
        str++;

    // Check for negative
    if (*str && *str == '-') {
        neg_multiplier = -1;
        str++;
    }

    // Do number
    for (; *str && isdigit(*str); str++) {
        result = (result * 10) + (*str - '0');
    }

    return result * neg_multiplier;
}