//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <base/arch/x86_64/syscall.h>
#include <stdio.h>

// ==========================================
// fopen / fclose
// ==========================================
FILE* fopen(const char* filename, const char* mode) {
    if (!filename || !mode) return NULL;

    int32_t flags = 0;
    switch (mode[0]) {
        case 'r':
            flags = O_RDONLY;
            if (strchr(mode, '+')) flags = O_RDWR;
            break;
        case 'w':
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_TRUNC;
            break;
        case 'a':
            flags = O_WRONLY | O_CREAT | O_APPEND;
            if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_APPEND;
            break;
        default:
            return NULL;
    }

    int32_t fd = sys_fopen((uint64_t)filename, flags);
    if (fd < 0) return NULL;

    FILE* stream = (FILE*)malloc(sizeof(FILE));
    if (!stream) {
        sys_fclose(fd);
        return NULL;
    }

    // 动态分配缓冲区
    stream->buf_capacity = DEFAULT_BUF_SIZE;
    stream->buffer = (uint8_t*)malloc(stream->buf_capacity);
    if (!stream->buffer) {
        free(stream);
        sys_fclose(fd);
        return NULL;
    }

    stream->fd = fd;
    // 空文件 size=0 是合法的，不应报错
    stream->file_size = sys_fsize(fd);
    stream->offset = 0;
    stream->buf_pos = 0;
    stream->buf_size = 0;

    return stream;
}

size_t fsize(FILE *stream) {
    if (!stream) return 0;
    return stream->file_size;
}

int32_t fclose(FILE *stream) {
    if (!stream) return -1;
    int32_t res = sys_fclose(stream->fd);
    if (stream->buffer) free(stream->buffer); // 释放动态缓冲区
    free(stream); 
    return res;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    
    if (nmemb > (SIZE_MAX / size)) return 0; // 防溢出
    
    size_t bytes_to_read = size * nmemb;
    size_t total_read = 0;
    uint8_t *dest = (uint8_t *)ptr;

    // 确保不超过文件剩余大小
    if (stream->file_size > 0) {
        if (stream->offset >= stream->file_size) return 0; // 已到末尾
        size_t remaining_in_file = stream->file_size - stream->offset;
        if (bytes_to_read > remaining_in_file) {
            bytes_to_read = remaining_in_file;
        }
    }
    
    if (bytes_to_read == 0) return 0;

    // 核心循环：带缓冲的数据读取
    while (total_read < bytes_to_read) {
        // 缓冲区还有未消费的数据，直接内存拷贝（零 syscall）
        if (stream->buf_pos < stream->buf_size) {
            size_t avail = stream->buf_size - stream->buf_pos;
            size_t to_copy = (bytes_to_read - total_read < avail) ? (bytes_to_read - total_read) : avail;
            
            memcpy(dest + total_read, stream->buffer + stream->buf_pos, to_copy);
            
            stream->buf_pos += to_copy;
            stream->offset += to_copy; // 修复2：及时更新逻辑偏移量
            total_read += to_copy;
            continue;
        }

        // 缓冲区空了。如果剩余请求量 >= 缓冲区容量，直接 bypass 缓冲区，DMA 到用户内存
        size_t remaining_request = bytes_to_read - total_read;
        if (remaining_request >= stream->buf_capacity) {
            size_t r = sys_fread(stream->fd, (uint64_t)(dest + total_read), remaining_request);
            stream->offset += r; // 修复2：更新偏移量
            total_read += r;
            if (r == 0) break; // EOF 或出错
            continue;
        }

        // 剩余请求量 < 缓冲区容量，refill 缓冲区（触发 1 次 syscall 拉取一整块数据）
        stream->buf_pos = 0;
        stream->buf_size = sys_fread(stream->fd, (uint64_t)stream->buffer, stream->buf_capacity);
        if (stream->buf_size == 0) break; // EOF 或出错
    }

    return total_read / size; // 返回成功读取的元素个数
}

int32_t fseek(FILE* stream, int64_t offset, int32_t whence) {
    if (!stream) return -1;

    // 任何跳转操作都让当前缓冲区作废
    stream->buf_pos = 0;
    stream->buf_size = 0;

    int64_t new_offset = 0;
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = stream->offset + offset;
            break;
        case SEEK_END:
            new_offset = stream->file_size + offset; 
            break;
        default:
            return EINVAL;
    }

    // 统一的越界检查
    if (new_offset < 0 || (uint64_t)new_offset > stream->file_size) {
        return EINVAL;
    }

    stream->offset = new_offset;
    // 通知内核物理指针移动到新位置 (使用 SEEK_SET 传绝对位置最安全)
    int64_t res = sys_flseek(stream->fd, new_offset, SEEK_SET);
    
    return (res >= 0) ? 0 : -1;
}

int64_t ftell(FILE* stream) {
    if (!stream) return -1;
    
    return stream->offset;
}