//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <string.h>
#include <ctype.h>
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

#define ONES  0x0101010101010101ULL
#define HIGHS 0x8080808080808080ULL

int strcmp(const char *cs, const char *ct)
{
    // 1. 双指针对齐阶段
    while (((uintptr_t)cs & 7) != 0 || ((uintptr_t)ct & 7) != 0) {
        uint8_t c1 = *(const uint8_t *)cs;
        uint8_t c2 = *(const uint8_t *)ct;
        if (c1 != c2)
            return (int)c1 - (int)c2; 
        if (c1 == '\0')
            return 0;
        cs++;
        ct++;
    }

    // 2. SWAR 主循环
    while (1) {
        uint64_t x, y;
        
        
        memcpy(&x, cs, 8);
        memcpy(&y, ct, 8);

        // 并行检测 0 字节
        uint64_t zero_mask = ((x - ONES) & ~x & HIGHS) | ((y - ONES) & ~y & HIGHS);

        // 计算差异
        uint64_t diff = x ^ y;
        uint64_t mask = diff | zero_mask;

        if (__builtin_expect(mask != 0, 0)) {
            int byte_idx;
            
        #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            // 大端序：内存低地址对应数值高位。
            // clz 返回前导 0 个数。若最高位 bit63 置 1，clz=0，对应内存字节 0。
            // 因此直接用 clz 除以 8 即可得到正确的内存字节索引。
            byte_idx = __builtin_clzll(mask) / 8;
        #else
            // 小端序：内存低地址对应数值低位。
            // ctz 返回末尾 0 个数。若最低位 bit0 置 1，ctz=0，对应内存字节 0。
            byte_idx = __builtin_ctzll(mask) / 8;
        #endif

            // 统一使用无符号字节指针下标取值，消除隐式符号转换隐患，保持代码一致性
            const uint8_t *p1 = (const uint8_t *)cs;
            const uint8_t *p2 = (const uint8_t *)ct;
            uint8_t c1 = p1[byte_idx];
            uint8_t c2 = p2[byte_idx];

            if (c1 != c2)
                return (int)c1 - (int)c2; // C 标准：返回 unsigned char 的差值
            else
                return 0; // 遇到 '\0' 且之前的字节都相等
        }

        cs += 8;
        ct += 8;
    }
}

/* D5 (round 2 诊断): 临时回退为 static 状态 (对照桌面挂点) */
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
/* strchr: glibc 逐字拷贝实现(LGPL 污染)已替换为自研简单版本 */
char *
strchr (const char *s, int32_t c_in)
{
    unsigned char c = (unsigned char) c_in;
    for (;; s++) {
        if (*s == c)
            return (char *) s;
        if (*s == 0)
            return NULL;
    }
}


char *strcpy(char *strDest, const char *strSrc){
    if (strDest == NULL || strSrc == NULL)
        return strDest;
    char *address = strDest;
    while( (*strDest++ = * strSrc++) != '\0' ) 
         ;
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
    /* P5-102: C 标准语义 —— src 短于 n 时余下字节必须补零
       (原实现不补零, 调用方读未初始化字节) */
    while (n > 0 && (*dest++ = *src++) != '\0') n--;
    while (n-- > 0) *dest++ = '\0';
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
    if (!s) return 0;
#if defined(__GNUC__) || defined (__clang__)
	register const uintptr_t	s0 = (uintptr_t)s;
	register const word_t		*w = (const word_t *)(((uintptr_t)s) & \
									-((uintptr_t)(sizeof(word_t))));

	register word_t	wi = *w;

	register word_t	m = ((word_t)-1 / 0xff) * 0x7f;

#if __BYTE_ORDER == __LITTLE_ENDIAN
	register bytemask_t	 mask = ~(((wi & m) + m) | wi | m) >> (/* CHAR_BIT */ 8 * (s0 % sizeof (word_t)));
#else
	register bytemask_t	 mask = ~(((wi & m) + m) | wi | m) << (/* CHAR_BIT */ 8 * (s0 % sizeof (word_t)));
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
    /* D5 (round 9): 溢出防护 —— 原无界累加在超 int32 时回绕为 UB;
       按 C 标准 atoi 溢出行为未定义, 此处钳制饱和返回 */
    int64_t result = 0;
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
    } else if (*str && *str == '+') {
        str++;
    }

    // Do number (64 位中间量, 饱和钳制到 int32 范围)
    for (; *str && isdigit(*str); str++) {
        result = (result * 10) + (*str - '0');
        if (result > 0x7FFFFFFFLL) {
            return (neg_multiplier < 0) ? -2147483647 - 1 : 0x7FFFFFFF;
        }
    }

    return (int32_t)(result * neg_multiplier);
}