//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef _STDIO_H_
#define _STDIO_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

#define BUF_SIZE    4096

typedef struct {
    int32_t fd;          // 内核文件句柄
    uint64_t file_size;
    uint64_t offset;
    
    // 缓冲 I/O 专用字段 (去掉了引起歧义的全局 pos 和 size)
    int32_t buf_pos;     // 缓冲区当前读取游标
    int32_t buf_size;    // 缓冲区中当前实际有效的数据量
    uint8_t buffer[BUF_SIZE]; // 4KB 缓冲池
} FILE;


int32_t fclose(FILE *stream);
size_t fsize(FILE *stream);
FILE *fopen(const char* filename,const char* mode);
size_t fread(void *ptr,size_t size,size_t nmemb,FILE *stream);
int32_t fseek(FILE* stream,int64_t offset, int32_t whence);

#ifdef __cplusplus
}
#endif


#endif