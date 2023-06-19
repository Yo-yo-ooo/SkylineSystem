#include "./header/interpreter.h"

#define NULL 0

typedef struct InterpreterInfo{
    char *name;
    long long value;
    char* type;
}IIFO;

char* strpbrk(const char* str, const char* charset) {
    const char* p = str;
    while (*p) {
        const char* q = charset;
        while (*q) {
            if (*p == *q) {
                // 在字符集中找到了匹配的字符，返回该字符的地址
                return (char*)p;
            }
            q++;
        }
        p++;
    }
    // 没有找到匹配的字符，返回NULL
    return NULL;
}

char* strtok(char* str, const char* delimiter)
{
    static char* buffer = NULL;  // 用来保存上一次的分割结果
    char* result = NULL;         // 返回值
    if (str != NULL) {
        buffer = str;
    }
    if (buffer == NULL) {
        return NULL;
    }
    // 查找下一个分隔符的位置
    char* p = strpbrk(buffer, delimiter);
    if (p != NULL) {
        // 如果找到的话，将分隔符替换成'\0'，并返回结果
        *p = '\0';
        result = buffer;
        // 移动buffer的位置
        buffer = p + 1;
    } else {
        // 如果找不到分隔符，说明已经到达字符串末尾，返回最后一个结果
        result = buffer;
        buffer = NULL;
    }
    return result;
}

void Interpreter::splitString(char* str, char** arr, int* len) {
    char* pch;
    int i = 0;
    pch = strtok(str, " ");
    while(pch != NULL) {
        arr[i++] = pch;
        pch = strtok(NULL, " ");
    }
    *len = i;
}

Interpreter::Interpreter(char* Filename){

}