#ifndef _KLIB_STR_H_
#define _KLIB_STR_H_

#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C"{
#endif

uint8_t strcmp(const char *cs, const char *ct);
char *strtok(char *str, const char *delim);
char* strchr(char* str, int32_t c);
char *strcpy(char *strDest, const char *strSrc);
int32_t strncmp(const char* a, const char* b, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
char* strcat(char* dest, const char* source);
size_t strlen(const char* str);
int32_t atoi(char *str);

#ifdef __cplusplus
}
#endif

#endif