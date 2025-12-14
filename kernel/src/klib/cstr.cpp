#include <klib/klib.h>

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

char* StrCombine(const char* a, const char* b)
{
    int32_t lenA = strlen(a);
    int32_t lenB = strlen(b);
    
    int32_t totalLen = lenA + lenB;
    char* tempStr = (char*) kmalloc(totalLen + 1);
    tempStr[totalLen] = 0;

    for (int32_t i = 0; i < lenA; i++)
        tempStr[i] = a[i];
    for (int32_t i = 0; i < lenB; i++)
        tempStr[i + lenA] = b[i];

    return tempStr;
}

char intTo_stringOutput[128];
const char *to_string(uint64_t value)
{
    uint8_t size = 0;
    uint64_t sizetest = value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;
    while (value / 10 > 0)
    {
        uint8_t remainder = value % 10;
        value /= 10;
        intTo_stringOutput[size - index] = remainder + '0';
        index++;
    }
    uint8_t remainder = value % 10;
    intTo_stringOutput[size - index] = remainder + '0';
    intTo_stringOutput[size + 1] = 0;
    intTo_stringOutput[size + 2] = 0;
    return intTo_stringOutput;
}

const char *to_string(int64_t value)
{
    uint8_t size = 0;
    if (value < 0)
    {
        value *= -1;
        intTo_stringOutput[0] = '-';
        size = 1;
    }

    uint64_t sizetest = value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;
    while (value / 10 > 0)
    {
        uint8_t remainder = value % 10;
        value /= 10;
        intTo_stringOutput[size - index] = remainder + '0';
        index++;
    }

    uint8_t remainder = value % 10;
    intTo_stringOutput[size - index] = remainder + '0';
    intTo_stringOutput[size + 1] = 0;
    intTo_stringOutput[size + 2] = 0;

    return intTo_stringOutput;
}

const char *to_string(int32_t value)
{
    return to_string((int64_t)value);
}

const char *to_string(uint32_t value)
{
    return to_string((int64_t)value);
}

const char *to_string(char value)
{
    intTo_stringOutput[0] = value;
    intTo_stringOutput[1] = 0;
    return intTo_stringOutput;
}

char doubleTo_stringOutput[128];
#ifdef __x86_64__
__ffunc const char *to_string(double value, uint8_t places)
{
    uint8_t size = 0;
    if (value < 0)
    {
        value *= -1;
        doubleTo_stringOutput[0] = '-';
        size = 1;
    }

    uint64_t sizetest = (int64_t)value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;

    if (places > 0)
    {
        size += places + 1;

        double temp = 1;

        for (int32_t i = 0; i < places; i++)
            temp *= 10;

        uint64_t value3 = (uint64_t)((value - ((uint64_t)value)) * temp);

        for (int32_t i = 0; i < places; i++)
        {
            uint8_t remainder = value3 % 10;
            value3 /= 10;
            doubleTo_stringOutput[size - index] = remainder + '0';
            index++;
        }

        doubleTo_stringOutput[size - index] = '.';
        index++;
    }

    uint64_t value2 = (int64_t)value;
    if (value2 == 0)
        doubleTo_stringOutput[size - index] = '0';
    else
        while (value2 > 0)
        {
            uint8_t remainder = value2 % 10;
            value2 /= 10;
            doubleTo_stringOutput[size - index] = remainder + '0';
            index++;
        }

    doubleTo_stringOutput[size + 1] = 0;

    return doubleTo_stringOutput;
}

const char *to_string(double value)
{
    return to_string(value, 2);
}
#endif

const char *to_string(bool value)
{
    if (value)
        return "true";
    else
        return "false";
}

__ffunc uint32_t ConvertStringToHex(const char *data)
{
    uint32_t hex = 0;

    for (uint32_t i = 0; i < 6;)
    {
        uint8_t temp = 0;
        for (uint32_t i2 = 16; i2 != 0; i2 /= 16)
        {
            if (data[i] >= '0' && data[i] <= '9')
                temp += (data[i] - '0') * i2;
            else if (data[i] >= 'A' && data[i] <= 'F')
                temp += (data[i] + 10 - 'A') * i2;
            else if (data[i] >= 'a' && data[i] <= 'f')
                temp += (data[i] + 10 - 'a') * i2;
            i++;
        }
        hex = hex << 8;
        hex += temp;
    }

    return hex;
}

unsigned long ConvertStringToLongHex(const char *data)
{
    unsigned long hex = 0;

    for (uint32_t i = 0; i < 16; i++)
    {
        if (data[i] == 0)
            break;
        uint8_t temp = 0;
        {
            if (data[i] >= '0' && data[i] <= '9')
                temp += (data[i] - '0');
            else if (data[i] >= 'A' && data[i] <= 'F')
                temp += (data[i] + 10 - 'A');
            else if (data[i] >= 'a' && data[i] <= 'f')
                temp += (data[i] + 10 - 'a');
        }
        hex = hex << 4;
        hex += temp;
    }

    return hex;
}

char hexTo_stringOutput[128];
const char *hexABC = "0123456789ABCDEF";
const char *ConvertHexToString(uint64_t hex, uint8_t size)
{
    hex = (hex << (16 - size) * 4);
    for (uint8_t i = 0; i < size; i++)
        hexTo_stringOutput[i] = hexABC[((hex << i * 4) >> ((15) * 4))];

    hexTo_stringOutput[size] = 0;

    return hexTo_stringOutput;
}
const char *ConvertHexToString(uint64_t hex)
{
    return ConvertHexToString(hex, (64 / 4));
}
const char *ConvertHexToString(uint32_t hex)
{
    return ConvertHexToString(hex, (32 / 4));
}
const char *ConvertHexToString(uint16_t hex)
{
    return ConvertHexToString(hex, (16 / 4));
}
const char *ConvertHexToString(uint8_t hex)
{
    return ConvertHexToString(hex, (8 / 4));
}

int64_t to_int(const char *string)
{
    uint64_t number = 0;
    uint64_t size = 0;
    while (string[size] != 0)
        size++;

    int64_t end = 0;
    int64_t exp = 1;
    if (string[0] == '-')
    {
        end = 1;
        exp *= -1;
    }

    for (int64_t i = size - 1; i >= end; i--)
    {
        number += exp * (string[i] - '0');
        exp *= 10;
    }
    return number;
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
    ASSERT((strDest!=NULL) && (strSrc !=NULL)); 
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