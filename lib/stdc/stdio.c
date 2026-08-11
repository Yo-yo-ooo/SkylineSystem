//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <base/arch/x86_64/syscall.h>
#include <stdio.h>
#include <limits.h>
#include <atomic/atomic.h>

static inline void file_spin_lock(volatile uint8_t *lock) {
    while (atomic_test_and_set(lock, ATOMIC_ACQUIRE)) {
#ifdef __x86_64__
        asm volatile("pause");
#elif defined(__aarch64__)
        asm volatile("yield");
#elif defined (__riscv)
        asm volatile("pause");
#endif
    }
}

static inline void file_spin_unlock(volatile uint8_t *lock) {
    atomic_clear(lock, ATOMIC_RELEASE);
}

FILE* fopen(const char * __restrict__ filename, const char * __restrict__ mode) {
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

    stream->buf_capacity = DEFAULT_BUF_SIZE;
    stream->buffer = (uint8_t*)malloc(stream->buf_capacity);
    if (!stream->buffer) {
        free(stream);
        sys_fclose(fd);
        return NULL;
    }

    stream->fd = fd;
    stream->file_size = sys_fsize(fd);
    stream->offset = 0;
    stream->buf_pos = 0;
    stream->buf_size = 0;
    stream->lock = 0; // 初始化锁

    return stream;
}


size_t fsize(FILE *stream) {
    if (!stream) return 0;
    return atomic_load_8(&stream->file_size, ATOMIC_ACQUIRE);
}

int fclose(FILE *stream) {
    if (!stream) return -1;

    file_spin_lock(&stream->lock);
    int32_t res = sys_fclose(stream->fd);
    if (stream->buffer)
        free(stream->buffer);
    file_spin_unlock(&stream->lock); // 先解锁，再free，修复UB
    free(stream);

    return (res >= 0) ? 0 : -1;
}

size_t fread(void * __restrict__ ptr, size_t size, size_t nmemb, FILE * __restrict__ stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;

    if (nmemb > (SIZE_MAX / size)) return 0; // 防溢出

    size_t bytes_to_read = size * nmemb;
    size_t total_read = 0;
    uint8_t *dest = (uint8_t *)ptr;

    file_spin_lock(&stream->lock);

    if (stream->file_size > 0) {
        if (stream->offset >= stream->file_size) {
            file_spin_unlock(&stream->lock);
            return 0; // 已到末尾
        }
        size_t remaining_in_file = stream->file_size - stream->offset;
        if (bytes_to_read > remaining_in_file) {
            bytes_to_read = remaining_in_file;
        }
    }

    if (bytes_to_read == 0) {
        file_spin_unlock(&stream->lock);
        return 0;
    }

    while (total_read < bytes_to_read) {
        if (stream->buf_pos < stream->buf_size) {
            size_t avail = stream->buf_size - stream->buf_pos;
            size_t to_copy = (bytes_to_read - total_read < avail) ? (bytes_to_read - total_read) : avail;

            memcpy(dest + total_read, stream->buffer + stream->buf_pos, to_copy);

            stream->buf_pos += to_copy;
            stream->offset += to_copy;
            total_read += to_copy;
            continue;
        }

        size_t remaining_request = bytes_to_read - total_read;
        if (remaining_request >= stream->buf_capacity) {
            size_t r = sys_fread(stream->fd, (uint64_t)(dest + total_read), remaining_request);
            stream->offset += r;
            total_read += r;
            if (r == 0) break;
            continue;
        }

        stream->buf_pos = 0;
        stream->buf_size = sys_fread(stream->fd, (uint64_t)stream->buffer, stream->buf_capacity);
        if (stream->buf_size == 0) break;
    }

    file_spin_unlock(&stream->lock);
    return total_read / size;
}


int fseek(FILE * __restrict__ stream, long offset, int whence) {
    if (!stream) return -1;

    file_spin_lock(&stream->lock);

    stream->buf_pos = 0;
    stream->buf_size = 0;

    int64_t new_offset = 0;
    switch (whence) {
        case SEEK_SET:
            new_offset = (int64_t)offset;
            break;
        case SEEK_CUR:
            new_offset = stream->offset + (int64_t)offset;
            break;
        case SEEK_END:
            new_offset = stream->file_size + (int64_t)offset;
            break;
        default:
            file_spin_unlock(&stream->lock);
            errno = EINVAL;
            return -1;
    }

    if (new_offset < 0 || (uint64_t)new_offset > stream->file_size) {
        file_spin_unlock(&stream->lock);
        errno = EINVAL;
        return -1;
    }

    stream->offset = new_offset;
    int64_t res = sys_flseek(stream->fd, new_offset, SEEK_SET);

    file_spin_unlock(&stream->lock);
    return (res >= 0) ? 0 : -1;
}

/* 标准C ftell 返回 long；内部offset是64位，做截断转换 */
long ftell(FILE * __restrict__ stream) {
    if (!stream) return -1L;

    int64_t off = (int64_t)atomic_load_8(&stream->offset, ATOMIC_ACQUIRE);
    /* 标准ftell只能返回long；超出long范围返回‑1并置errno */
    if (off > LONG_MAX || off < LONG_MIN) {
        errno = EOVERFLOW;
        return -1L;
    }
    return (long)off;
}
