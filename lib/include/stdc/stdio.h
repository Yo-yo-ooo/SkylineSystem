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

#define DEFAULT_BUF_SIZE 4096

typedef struct {
    int32_t fd;            // 内核文件句柄
    uint64_t file_size;    // 文件总大小
    uint64_t offset;       // 用户态维护的逻辑偏移量
    
    // 动态缓冲 I/O 专用字段
    uint8_t* buffer;       // 动态分配的缓冲区指针
    size_t buf_capacity;   // 缓冲区容量
    size_t buf_pos;        // 缓冲区当前读取游标
    size_t buf_size;       // 缓冲区中当前实际有效的数据量
} FILE;

int32_t fclose(FILE *stream);
size_t fsize(FILE *stream);
FILE *fopen(const char* filename, const char* mode);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int32_t fseek(FILE* stream, int64_t offset, int32_t whence);
int64_t ftell(FILE* stream);

#ifdef __cplusplus
}
#endif

#endif