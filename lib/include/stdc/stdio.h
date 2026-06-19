#pragma once
#ifndef _STDIO_H_
#define _STDIO_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef uint32_t FILE;

FILE *fopen(const char* filename,const char* mode);
size_t fread(void *ptr,size_t size,size_t nmemb,FILE *stream);
int32_t fseek(FILE* stream,int64_t offset, int32_t whence);

#ifdef __cplusplus
}
#endif


#endif