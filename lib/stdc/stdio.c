#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <base/arch/x86_64/syscall.h>
#include <stdio.h>


// ==========================================
// fopen / fclose
// ==========================================
FILE* fopen(const char* filename, const char* mode) {
    if (!filename || !mode) return NULL;

    int32_t flags = 0;

    // 解析 mode 字符串
    if (strchr(mode, '+')) {
        flags |= O_RDWR;
    } else if (mode[0] == 'r') {
        flags |= O_RDONLY;
    } else {
        flags |= O_WRONLY;
    }

    if (mode[0] == 'w') {
        flags |= O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags |= O_CREAT | O_APPEND;
    }

    // 调用内核打开文件
    int32_t fd = sys_fopen((uint64_t)filename, flags);
    if (fd < 0) return NULL;

    FILE* stream = (FILE*)malloc(sizeof(FILE));
    if (!stream) {
        sys_fclose(fd);
        return NULL;
    }

    stream->fd = fd;
    stream->buf_pos = 0;  // 初始时缓冲区为空
    stream->buf_size = 0;

    return stream;
}

int32_t fclose(FILE *stream) {
    if (!stream) return -1;
    // 如果你以后实现了 fwrite，这里还需要加上 fflush(stream) 把缓冲区写回内核
    int32_t res = sys_fclose(stream->fd);
    free(stream); // 解决内存泄漏
    return res;
}

// ==========================================
// fread (带智能缓冲的读取机制)
// ==========================================
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    
    size_t bytes_to_read = size * nmemb;
    size_t total_read = 0;
    uint8_t *dest = (uint8_t *)ptr;

    while (bytes_to_read > 0) {
        // 1. 如果缓冲区空了，需要向内核请求填充
        if (stream->buf_pos >= stream->buf_size) {
            
            // 【极客优化】：如果用户要读的数据大于 4KB，干脆绕过缓冲区，直接读到目标内存！
            // 这会让加载 10MB 的字体文件速度极快，不浪费 CPU 去 memcpy。
            if (bytes_to_read >= BUF_SIZE) {
                int32_t read_now = sys_fread(stream->fd, (uint64_t)dest, bytes_to_read);
                if (read_now <= 0) break; // EOF 或错误
                
                total_read += read_now;
                dest += read_now;
                bytes_to_read -= read_now;
                continue; 
            }
            
            // 【常规缓冲】：向内核要 4KB 数据塞进缓冲区
            int32_t read_now = sys_fread(stream->fd, (uint64_t)stream->buffer, BUF_SIZE);
            if (read_now <= 0) break; // EOF 或错误
            
            stream->buf_pos = 0;
            stream->buf_size = read_now;
        }

        // 2. 从缓冲区拷贝数据到目标内存
        size_t available = stream->buf_size - stream->buf_pos;
        size_t to_copy = (available < bytes_to_read) ? available : bytes_to_read;
        
        memcpy(dest, stream->buffer + stream->buf_pos, to_copy);
        
        stream->buf_pos += to_copy;
        dest += to_copy;
        total_read += to_copy;
        bytes_to_read -= to_copy;
    }

    // 返回成功读取的“元素个数”
    return total_read / size;
}

// ==========================================
// fseek 与 ftell (纯粹依靠内核维护的架构)
// ==========================================
int32_t fseek(FILE* stream, int64_t offset, int32_t whence) {
    if (!stream) return -1;

    // 【关键动作】：因为用户进行了跳转，当前预读的 4KB 缓冲数据全部作废（缓存失效）
    stream->buf_pos = 0;
    stream->buf_size = 0;

    // 直接将偏移指令透传给内核
    int64_t res = sys_flseek(stream->fd, offset, whence);
    
    // sys_flseek 返回新的绝对偏移量(>=0)表示成功，返回负数表示失败
    return (res >= 0) ? 0 : -1;
}

// 标准 C 中获取当前文件指针的函数
int64_t ftell(FILE* stream) {
    if (!stream) return -1;
    
    // 问内核当前真实的读写位置在哪里
    int64_t kernel_pos = sys_flseek(stream->fd, 0, SEEK_CUR);
    if (kernel_pos < 0) return -1;
    
    // 【核心算术】：因为缓冲区的存在，内核的指针比用户的逻辑指针“超前”了。
    // 所以逻辑位置 = 内核物理位置 - 缓冲区中未消费的数据量
    int32_t unread_bytes = stream->buf_size - stream->buf_pos;
    return kernel_pos - unread_bytes;
}