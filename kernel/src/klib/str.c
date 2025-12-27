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

size_t strlen(const char* str) {
   const char* eos = str;
   while(*eos++);
   return (eos - str - 1);
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