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

/* ==============================================================
 * SkylineSystem 用户态 stdio
 * --------------------------------------------------------------
 * 常规文件: sys_fopen/sys_fread/sys_fwrite/sys_flseek/sys_fclose/
 *           sys_fsize (内核文件系统 syscall 包装)。
 * stdout/stderr: 没有内核 fd, 写走 printf.c 里的 console_stdout_write()
 *           (共享控制台终端)。
 * stdin: 用户态没有内核输入 fd, 读一律得到 EOF (luaL_loadfilex 处理
 *           filename==NULL 时的行为依赖这一点)。
 *
 * 读缓冲: fread 会在 stream->buffer 里预读, 因此"用户态逻辑偏移"
 *         stream->offset 与内核 fd 位置可能不一致。任何写操作前都会
 *         丢弃读缓冲并 sys_flseek 到 stream->offset。
 * 写缓冲: 不实现 —— fwrite 直接 sys_fwrite, fflush 因此是 no-op。
 * ============================================================== */

/* 标准流对象 (静态, 不参与 fclose 释放) */
static FILE sky_stdin_file = {
    .fd = -1, .ungot = -1, .kind = SKY_FILE_KIND_STDIN,
};
static FILE sky_stdout_file = {
    .fd = -1, .ungot = -1, .kind = SKY_FILE_KIND_CONSOLE,
};
static FILE sky_stderr_file = {
    .fd = -1, .ungot = -1, .kind = SKY_FILE_KIND_CONSOLE,
};

FILE *stdin = &sky_stdin_file;
FILE *stdout = &sky_stdout_file;
FILE *stderr = &sky_stderr_file;

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

/* mode 字符串 -> 内核 open flags; 非法返回 -1 */
static int32_t parse_mode(const char *mode) {
    if (!mode) return -1;
    switch (mode[0]) {
        case 'r':
            return strchr(mode, '+') ? O_RDWR : O_RDONLY;
        case 'w':
            return strchr(mode, '+') ? (O_RDWR | O_CREAT | O_TRUNC)
                                     : (O_WRONLY | O_CREAT | O_TRUNC);
        case 'a':
            return strchr(mode, '+') ? (O_RDWR | O_CREAT | O_APPEND)
                                     : (O_WRONLY | O_CREAT | O_APPEND);
        default:
            return -1;
    }
}

/* 内核 syscall 失败返回**负 errno** (见 kernel .../syscall/fops.cpp);
   转成用户态 errno。越界/0 时退化为 EIO。 */
static void set_errno_from_syscall(int32_t ret) {
    int32_t err = -ret;
    errno = (err > 0 && err <= EHWPOISON) ? err : EIO;
}

/* 文件大小 syscall 返回值可能是负 errno; 失败按 0 处理 */
static uint64_t syscall_fsize_u64(int32_t fd) {
    int64_t sz = (int64_t)sys_fsize(fd);
    return (sz < 0) ? 0 : (uint64_t)sz;
}

FILE* fopen(const char * __restrict__ filename, const char * __restrict__ mode) {
    if (!filename || !mode) return NULL;

    int32_t flags = parse_mode(mode);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }

    int32_t fd = (int32_t)sys_fopen((uint64_t)filename, (uint64_t)flags);
    if (fd < 0) {
        set_errno_from_syscall(fd);
        return NULL;
    }

    FILE* stream = (FILE*)malloc(sizeof(FILE));
    if (!stream) {
        sys_fclose(fd);
        errno = ENOMEM;
        return NULL;
    }

    stream->buf_capacity = DEFAULT_BUF_SIZE;
    stream->buffer = (uint8_t*)malloc(stream->buf_capacity);
    if (!stream->buffer) {
        free(stream);
        sys_fclose(fd);
        errno = ENOMEM;
        return NULL;
    }

    stream->fd = fd;
    stream->file_size = syscall_fsize_u64(fd);
    stream->offset = (flags & O_APPEND) ? stream->file_size : 0;
    stream->buf_pos = 0;
    stream->buf_size = 0;
    stream->lock = 0; // 初始化锁
    stream->eof = 0;
    stream->error = 0;
    stream->ungot = -1;
    stream->kind = SKY_FILE_KIND_FILE;

    return stream;
}


size_t fsize(FILE *stream) {
    if (!stream) return 0;
    return atomic_load_8(&stream->file_size, ATOMIC_ACQUIRE);
}

int fclose(FILE *stream) {
    if (!stream) return -1;

    /* 标准流/控制台流不是 fopen 分配的, 不能 free */
    if (stream->kind != SKY_FILE_KIND_FILE) return 0;

    file_spin_lock(&stream->lock);
    int32_t res = (int32_t)sys_fclose(stream->fd);
    if (res < 0) set_errno_from_syscall(res);
    if (stream->buffer)
        free(stream->buffer);
    file_spin_unlock(&stream->lock); // 先解锁，再free，修复UB
    free(stream);

    return (res >= 0) ? 0 : -1;
}

size_t fread(void * __restrict__ ptr, size_t size, size_t nmemb, FILE * __restrict__ stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;

    /* stdin 没有内核输入源; 控制台输出流不可读 —— 两者都按 EOF 处理 */
    if (stream->kind != SKY_FILE_KIND_FILE) {
        stream->eof = 1;
        return 0;
    }

    if (nmemb > (SIZE_MAX / size)) return 0; // 防溢出

    size_t bytes_to_read = size * nmemb;
    size_t total_read = 0;
    uint8_t *dest = (uint8_t *)ptr;

    file_spin_lock(&stream->lock);

    /* ungetc 回退字符优先消费。它已经被读过一次 (offset 已计入),
       这里不再推进 offset。 */
    if (stream->ungot >= 0) {
        dest[total_read++] = (uint8_t)stream->ungot;
        stream->ungot = -1;
    }

    if (stream->file_size > 0) {
        if (stream->offset >= stream->file_size) {
            if (total_read == 0) {
                stream->eof = 1;
                file_spin_unlock(&stream->lock);
                return 0; // 已到末尾
            }
        } else {
            size_t remaining_in_file = (size_t)(stream->file_size - stream->offset);
            size_t want = bytes_to_read - total_read;
            if (want > remaining_in_file) bytes_to_read = total_read + remaining_in_file;
        }
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
            int64_t rr = (int64_t)sys_fread(stream->fd, (uint64_t)(dest + total_read), remaining_request);
            if (rr < 0) {
                stream->error = 1;
                break;
            }
            stream->offset += (uint64_t)rr;
            total_read += (size_t)rr;
            if (rr == 0) break;
            continue;
        }

        stream->buf_pos = 0;
        {
            int64_t rb = (int64_t)sys_fread(stream->fd, (uint64_t)stream->buffer, stream->buf_capacity);
            if (rb < 0) {
                stream->error = 1;
                break;
            }
            stream->buf_size = (size_t)rb;
        }
        if (stream->buf_size == 0) break;
    }

    /* 没读满 = 命中 EOF 或读错误。文件大小已知时能区分两者。 */
    if (total_read < bytes_to_read && stream->error == 0) {
        if (stream->file_size > 0 && stream->offset < stream->file_size)
            stream->error = 1;
        else
            stream->eof = 1;
    }

    file_spin_unlock(&stream->lock);
    return total_read / size;
}

size_t fwrite(const void * __restrict__ ptr, size_t size, size_t nmemb, FILE * __restrict__ stream) {
    if (!stream || size == 0 || nmemb == 0) return 0;
    if (nmemb > (SIZE_MAX / size)) return 0;

    size_t bytes = size * nmemb;
    if (!ptr) return 0;

    /* stdout/stderr: 没有内核 fd, 直接写共享控制台终端 */
    if (stream->kind == SKY_FILE_KIND_CONSOLE) {
        console_stdout_write((const char *)ptr, bytes);
        return nmemb;
    }
    if (stream->kind != SKY_FILE_KIND_FILE) {
        stream->error = 1;
        return 0;
    }

    file_spin_lock(&stream->lock);

    /* 丢弃读缓冲, 把内核 fd 位置对齐到用户态逻辑偏移 */
    stream->buf_pos = 0;
    stream->buf_size = 0;

    int64_t seekres = (int64_t)sys_flseek(stream->fd, stream->offset, SEEK_SET);
    if (seekres < 0) {
        stream->error = 1;
        file_spin_unlock(&stream->lock);
        return 0;
    }

    uint64_t w = sys_fwrite(stream->fd, (uint64_t)ptr, bytes);
    if ((int64_t)w < 0) w = 0;

    stream->offset += w;
    if (stream->offset > stream->file_size) stream->file_size = stream->offset;
    if (w < bytes) stream->error = 1;

    file_spin_unlock(&stream->lock);
    return (size_t)(w / size);
}

int fseek(FILE * __restrict__ stream, long offset, int whence) {    if (!stream) return -1;
    if (stream->kind != SKY_FILE_KIND_FILE) return -1;

    file_spin_lock(&stream->lock);

    stream->buf_pos = 0;
    stream->buf_size = 0;

    int64_t new_offset = 0;
    switch (whence) {
        case SEEK_SET:
            new_offset = (int64_t)offset;
            break;
        case SEEK_CUR:
            new_offset = (int64_t)stream->offset + (int64_t)offset;
            break;
        case SEEK_END:
            new_offset = (int64_t)stream->file_size + (int64_t)offset;
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

    stream->offset = (uint64_t)new_offset;
    stream->ungot = -1;         /* ISO C: fseek 丢弃 ungetc 回退字符 */
    int64_t res = (int64_t)sys_flseek(stream->fd, (uint64_t)new_offset, SEEK_SET);
    if (res < 0) {
        stream->error = 1;
        set_errno_from_syscall((int32_t)res);
        file_spin_unlock(&stream->lock);
        return -1;
    }
    stream->eof = 0;            /* ISO C: 成功 fseek 清除 EOF 指示器 */

    file_spin_unlock(&stream->lock);
    return 0;
}

/* ISO C rewind: 等价于 fseek(stream, 0, SEEK_SET) + clearerr(stream) */
void rewind(FILE *stream) {
    if (!stream) return;
    (void)fseek(stream, 0L, SEEK_SET);
    clearerr(stream);
}

/* 标准C ftell 返回 long；内部offset是64位，做截断转换 */long ftell(FILE * __restrict__ stream) {
    if (!stream) return -1L;

    int64_t off = (int64_t)atomic_load_8(&stream->offset, ATOMIC_ACQUIRE);
    /* 标准ftell只能返回long；超出long范围返回‑1并置errno */
    if (off > LONG_MAX || off < LONG_MIN) {
        errno = EOVERFLOW;
        return -1L;
    }
    return (long)off;
}

/* ==============================================================
 * ISO C 标准流状态与字符 I/O
 * ============================================================== */
int fflush(FILE *stream) {
    /* 本实现没有写缓冲, 所有输出都已直写; 读缓冲的"冲刷"由 fseek 处理。
       ISO C: fflush(NULL) 冲刷全部输出流。 */
    if (!stream) return 0;
    if (stream->kind == SKY_FILE_KIND_FILE && stream->error) return EOF;
    return 0;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream) {
    if (!stream) return;
    stream->eof = 0;
    stream->error = 0;
}

int fgetc(FILE *stream) {
    unsigned char c;

    if (!stream) return EOF;
    if (stream->ungot >= 0) {
        int uc = stream->ungot;
        stream->ungot = -1;
        return uc;
    }
    if (fread(&c, 1, 1, stream) != 1) return EOF;
    return (int)c;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) return EOF;
    /* 只有 1 个回退槽; 已有回退字符时失败 (标准只保证 1 个) */
    if (stream->ungot >= 0) return EOF;
    stream->ungot = (int)(unsigned char)c;
    stream->eof = 0;
    return (int)(unsigned char)c;
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    if (!stream) return EOF;
    if (fwrite(&ch, 1, 1, stream) != 1) return EOF;
    return (int)ch;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fputs(const char * __restrict__ str, FILE * __restrict__ stream) {
    size_t len;
    if (!str || !stream) return EOF;
    len = strlen(str);
    if (fwrite(str, 1, len, stream) != len) return EOF;
    return 0;
}

char *fgets(char * __restrict__ str, int count, FILE * __restrict__ stream) {
    int i = 0;

    if (!str || !stream || count <= 0) return NULL;
    if (count == 1) {
        str[0] = '\0';
        return str;
    }
    while (i < count - 1) {
        int c = fgetc(stream);
        if (c == EOF) break;
        str[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    str[i] = '\0';
    return str;
}

FILE *freopen(const char * __restrict__ filename, const char * __restrict__ mode,
              FILE * __restrict__ stream) {
    if (!stream || !filename || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int32_t flags = parse_mode(mode);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }

    /* ISA C: freopen 先关闭 stream 关联的文件 (标准流这里不动 fd) */
    if (stream->kind == SKY_FILE_KIND_FILE && stream->fd >= 0) {
        sys_fclose(stream->fd);
        stream->fd = -1;
    }

    int32_t fd = (int32_t)sys_fopen((uint64_t)filename, (uint64_t)flags);
    if (fd < 0) {
        set_errno_from_syscall(fd);
        stream->kind = SKY_FILE_KIND_FILE;
        stream->fd = -1;
        stream->buffer = NULL;
        stream->buf_capacity = 0;
        stream->error = 1;
        return NULL;
    }

    if (!stream->buffer || stream->buf_capacity == 0) {
        stream->buf_capacity = DEFAULT_BUF_SIZE;
        stream->buffer = (uint8_t*)malloc(stream->buf_capacity);
        if (!stream->buffer) {
            sys_fclose(fd);
            stream->buf_capacity = 0;
            stream->fd = -1;
            stream->error = 1;
            errno = ENOMEM;
            return NULL;
        }
    }

    stream->fd = fd;
    stream->file_size = syscall_fsize_u64(fd);
    stream->offset = (flags & O_APPEND) ? stream->file_size : 0;
    stream->buf_pos = 0;
    stream->buf_size = 0;
    stream->lock = 0;
    stream->eof = 0;
    stream->error = 0;
    stream->ungot = -1;
    stream->kind = SKY_FILE_KIND_FILE;
    return stream;
}

/* ==============================================================
 * fprintf / vfprintf —— 复用 printf.c 的 vsnprintf_ 做格式化
 * ============================================================== */
int vfprintf(FILE *stream, const char *format, va_list va) {
    char stackbuf[256];
    va_list copy;
    int32_t n;

    if (!stream || !format) return -1;

    va_copy(copy, va);
    n = vsnprintf_(stackbuf, sizeof(stackbuf), format, copy);
    va_end(copy);
    if (n < 0) return -1;

    if ((size_t)n < sizeof(stackbuf)) {
        size_t w = fwrite(stackbuf, 1, (size_t)n, stream);
        return (w == (size_t)n) ? (int)n : -1;
    }

    /* 超出栈缓冲: 动态分配精确大小重来一次 */
    {
        char *dyn = (char *)malloc((size_t)n + 1);
        if (!dyn) {
            size_t w = fwrite(stackbuf, 1, sizeof(stackbuf) - 1, stream);
            return (int)w;
        }
        va_copy(copy, va);
        int32_t m = vsnprintf_(dyn, (size_t)n + 1, format, copy);
        va_end(copy);
        if (m < 0) {
            free(dyn);
            return -1;
        }
        size_t to_write = (size_t)m;
        if (to_write > (size_t)n) to_write = (size_t)n;
        {
            size_t w = fwrite(dyn, 1, to_write, stream);
            free(dyn);
            return (w == to_write) ? (int)m : -1;
        }
    }
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list va;
    int ret;

    va_start(va, format);
    ret = vfprintf(stream, format, va);
    va_end(va);
    return ret;
}
