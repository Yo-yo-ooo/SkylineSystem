#ifndef _KLIB_STR_H_
#define _KLIB_STR_H_
// tests/common/klib/str.h — shim: 内核标准字符串函数(strcmp/strchr/atoi...)与
// libc 头冲突; 被测试组件并不调用它们, 标准名一律交给 libc, 仅保留非标准名
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C"{
#endif

int32_t klib_atoi(char *str);

#ifdef __cplusplus
}
#endif

#endif
