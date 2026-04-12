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


/* char* strchr(char* str, int32_t c) {
	for (; *str != 0; ++str) {
		if (*str == c) {
			return str;
		}
	}
	return 0;
} */
/* Find the first occurrence of C in S.  */
extern void Panic(const char* message);

char *
strchr (const char *s, int32_t c_in)
{
    const unsigned char *char_ptr;
    const uint64_t *longword_ptr;
    uint64_t longword, magic_bits, charmask;
    unsigned char c;

    c = (unsigned char) c_in;

    /* Handle the first few characters by reading one character at a time.
       Do this until CHAR_PTR is aligned on a longword boundary.  */
    for (char_ptr = (const unsigned char *) s;
         ((unsigned long int) char_ptr & (sizeof (longword) - 1)) != 0;
         ++char_ptr)
        if (*char_ptr == c)
            return (char *) char_ptr;
        else if (*char_ptr == '\0')
            return NULL;

    /* All these elucidatory comments refer to 4-byte longwords,
       but the theory applies equally well to 8-byte longwords.  */

    longword_ptr = (const unsigned long int *) char_ptr;

    /* Bits 31, 24, 16, and 8 of this number are zero.  Call these bits
       the "holes."  Note that there is a hole just to the left of
       each byte, with an extra at the end:

       bits:  01111110 11111110 11111110 11111111
       bytes: AAAAAAAA BBBBBBBB CCCCCCCC DDDDDDDD

       The 1-bits make sure that carries propagate to the next 0-bit.
       The 0-bits provide holes for carries to fall into.  */
    magic_bits = -1;
    magic_bits = magic_bits / 0xff * 0xfe << 1 >> 1 | 1;

    /* Set up a longword, each of whose bytes is C.  */
    charmask = c | (c << 8);
    charmask |= charmask << 16;
    if (sizeof (longword) > 4)
        /* Do the shift in two steps to avoid a warning if long has 32 bits.  */
        charmask |= (charmask << 16) << 16;
    if (sizeof (longword) > 8)
        Panic("strchr: longword is too big for this method");

    /* Instead of the traditional loop which tests each character,
       we will test a longword at a time.  The tricky part is testing
       if *any of the four* bytes in the longword in question are zero.  */
    for (;;)
    {
        /* We tentatively exit the loop if adding MAGIC_BITS to
           LONGWORD fails to change any of the hole bits of LONGWORD.

           1) Is this safe?  Will it catch all the zero bytes?
           Suppose there is a byte with all zeros.  Any carry bits
           propagating from its left will fall into the hole at its
           least significant bit and stop.  Since there will be no
           carry from its most significant bit, the LSB of the
           byte to the left will be unchanged, and the zero will be
           detected.

           2) Is this worthwhile?  Will it ignore everything except
           zero bytes?  Suppose every byte of LONGWORD has a bit set
           somewhere.  There will be a carry into bit 8.  If bit 8
           is set, this will carry into bit 16.  If bit 8 is clear,
           one of bits 9-15 must be set, so there will be a carry
           into bit 16.  Similarly, there will be a carry into bit
           24.  If one of bits 24-30 is set, there will be a carry
           into bit 31, so all of the hole bits will be changed.

           The one misfire occurs when bits 24-30 are clear and bit
           31 is set; in this case, the hole at bit 31 is not
           changed.  If we had access to the processor carry flag,
           we could close this loophole by putting the fourth hole
           at bit 32!

           So it ignores everything except 128's, when they're aligned
           properly.

           3) But wait!  Aren't we looking for C as well as zero?
           Good point.  So what we do is XOR LONGWORD with a longword,
           each of whose bytes is C.  This turns each byte that is C
           into a zero.  */

        longword = *longword_ptr++;

        /* Add MAGIC_BITS to LONGWORD.  */
        if ((((longword + magic_bits)

              /* Set those bits that were unchanged by the addition.  */
              ^ ~longword)

             /* Look at only the hole bits.  If any of the hole bits
                are unchanged, most likely one of the bytes was a
                zero.  */
             & ~magic_bits) != 0 ||

            /* That caught zeroes.  Now test for C.  */
            ((((longword ^ charmask) + magic_bits) ^ ~(longword ^ charmask))
             & ~magic_bits) != 0)
        {
            /* Which of the bytes was C or zero?
               If none of them were, it was a misfire; continue the search.  */

            const unsigned char *cp = (const unsigned char *) (longword_ptr - 1);

            if (*cp == c)
                return (char *) cp;
            else if (*cp == '\0')
                return NULL;
            if (*++cp == c)
                return (char *) cp;
            else if (*cp == '\0')
                return NULL;
            if (*++cp == c)
                return (char *) cp;
            else if (*cp == '\0')
                return NULL;
            if (*++cp == c)
                return (char *) cp;
            else if (*cp == '\0')
                return NULL;
            if (sizeof (longword) > 4)
            {
                if (*++cp == c)
                    return (char *) cp;
                else if (*cp == '\0')
                    return NULL;
                if (*++cp == c)
                    return (char *) cp;
                else if (*cp == '\0')
                    return NULL;
                if (*++cp == c)
                    return (char *) cp;
                else if (*cp == '\0')
                    return NULL;
                if (*++cp == c)
                    return (char *) cp;
                else if (*cp == '\0')
                    return NULL;
            }
        }
    }

    return NULL;
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