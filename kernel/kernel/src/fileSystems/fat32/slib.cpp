#include "slib.h"

int strcmp(const char* str1, const char* str2)
{
	while ( *(unsigned char*)str1 == *(unsigned char*)str2 )
	{  
        if( *(unsigned char*)str1 !='\0')
        {
            return 0;//当*(unsigned char*)str1 ==*(unsigned char*)str2==‘\0'时两个字符串完全相等
		}
        str1++;  //比较下个字符
		str2++;
	}
     //*(unsigned char*)str1 与*(unsigned char*)str2的差值与返回值正负匹配
	return *(unsigned char*)str1 - *(unsigned char*)str2; 
}


size_t strlen(const char* str) {
    const char* s = str;
    while (*s) {
        s++;
    }
    return (size_t)(s - str);
}

void memcpy(void* src, void* dest, uint64_t size)
{
    uint8_t* _src  = (uint8_t*)src;
    uint8_t* _dest = (uint8_t*)dest;
    while (size--)
    {
        *_dest = *_src;
        _dest++;
        _src++;
    }
}

void *malloc(size_t size){
    (void*)_Malloc1(size);
}

void free(void *ptr){
    _Free(ptr);
    return;
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') {
        return c - ('a' - 'A'); // 将小写字母转换为对应的大写字母
    }
    return c;
}
