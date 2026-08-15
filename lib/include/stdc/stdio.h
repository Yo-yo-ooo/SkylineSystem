//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef _STDIO_H_
#define _STDIO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

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
    volatile uint8_t lock;
} FILE;


int fclose(FILE *stream);
size_t fsize(FILE *stream);
FILE *fopen(const char * __restrict__ filename, const char * __restrict__ mode);
size_t fread(void * __restrict__ ptr, size_t size, size_t nmemb, FILE * __restrict__ stream);
int fseek(FILE * __restrict__ stream, long offset, int whence);
long ftell(FILE * __restrict__ stream);


/**
 * Tiny printf implementation
 * You have to implement _putchar if you use printf()
 * To avoid conflicts with the regular printf() API it is overridden by macro defines
 * and internal underscore-appended functions like printf_() are used
 * \param format A string that specifies the format of the output
 * \return The number of characters that are written into the array, not counting the terminating null character
 */
#define printf printf_
int32_t printf_(const char* format, ...);
#define _printf printf_

/**
 * Tiny sprintf implementation
 * Due to security reasons (buffer overflow) YOU SHOULD CONSIDER USING (V)SNPRINTF INSTEAD!
 * \param buffer A pointer to the buffer where to store the formatted string. MUST be big enough to store the output!
 * \param format A string that specifies the format of the output
 * \return The number of characters that are WRITTEN into the buffer, not counting the terminating null character
 */
#define sprintf sprintf_
int32_t sprintf_(char* buffer, const char* format, ...);

#define ppokln(...) printf_("[\033[38;2;0;255;0m OK \033[0m] " __VA_ARGS__);printf_("\n")
#define pinfoln(...) printf_("[\033[38;2;0;255;255mINFO\033[0m] " __VA_ARGS__ );printf_("\n")
#define pwarnln(...) printf_("[\033[38;2;255;255;0mWARN\033[0m] " __VA_ARGS__);printf_("\n")
#define perrorln(...) printf_("[\033[38;2;255;0;0mKERR\033[0m] " __VA_ARGS__);printf_("\n")

#define ppok(...) printf_("[\033[38;2;0;255;0m OK \033[0m] " __VA_ARGS__)
#define pinfo(...) printf_("[\033[38;2;0;255;255mINFO\033[0m] " __VA_ARGS__)
#define pwarn(...) printf_("[\033[38;2;255;255;0mWARN\033[0m] " __VA_ARGS__)
#define perror(...) printf_("[\033[38;2;255;0;0mKERR\033[0m] " __VA_ARGS__)

/**
 * Tiny snprintf/vsnprintf implementation
 * \param buffer A pointer to the buffer where to store the formatted string
 * \param count The maximum number of characters to store in the buffer, including a terminating null character
 * \param format A string that specifies the format of the output
 * \param va A value identifying a variable arguments list
 * \return The number of characters that COULD have been written into the buffer, not counting the terminating
 *         null character. A value equal or larger than count indicates truncation. Only when the returned value
 *         is non-negative and less than count, the string has been completely written.
 */
#define snprintf  snprintf_
#define vsnprintf vsnprintf_
int32_t  snprintf_(char* buffer, size_t count, const char* format, ...);
int32_t vsnprintf_(char* buffer, size_t count, const char* format, va_list va);


/**
 * Tiny vprintf implementation
 * \param format A string that specifies the format of the output
 * \param va A value identifying a variable arguments list
 * \return The number of characters that are WRITTEN into the buffer, not counting the terminating null character
 */
#define vprintf vprintf_
int32_t vprintf_(const char* format, va_list va);


/**
 * printf with output function
 * You may use this as dynamic alternative to printf() with its fixed _putchar() output
 * \param out An output function which takes one character and an argument pointer
 * \param arg An argument pointer for user data passed to output function
 * \param format A string that specifies the format of the output
 * \return The number of characters that are sent to the output function, not counting the terminating null character
 */
int32_t fctprintf(void (*out)(char character, void* arg), void* arg, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif