//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
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

    
    // 解析 mode 字符串的首字符，决定基本的读写和创建策略
    switch (mode[0]) {
        case 'r':
            flags = O_RDONLY;  // 使用赋值 '=' 而不是 '|='
            if (strchr(mode, '+')) {
                flags = O_RDWR;
            }
            break;
        case 'w':
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            if (strchr(mode, '+')) {
                flags = O_RDWR | O_CREAT | O_TRUNC;
            }
            break;
        case 'a':
            flags = O_WRONLY | O_CREAT | O_APPEND;
            if (strchr(mode, '+')) {
                flags = O_RDWR | O_CREAT | O_APPEND;
            }
            break;
        default:
            return NULL; // 首字符不是 r/w/a，属于非法模式
    }

    // 调用内核打开文件
    int32_t fd = sys_fopen((uint64_t)filename, flags);
    if (fd < 0) return NULL;

    FILE* stream = (FILE*)malloc(sizeof(FILE));
    if (!stream) {
        sys_fclose(fd);
        return NULL;
    }

    if((stream->file_size = sys_fsize(fd)) == 0){
        return NULL;
    }

    stream->fd = fd;
    stream->buf_pos = 0;  // 初始时缓冲区为空
    stream->buf_size = 0;

    return stream;
}

int32_t fclose(FILE *stream) {
    if (!stream) return -1;
    int32_t res = sys_fclose(stream->fd);
    free(stream); // 解决内存泄漏
    return res;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    
    // 1. 防止整数溢出漏洞 (经典的安全防御)
    if (nmemb > (SIZE_MAX / size)) return 0;
    
    size_t bytes_to_read = size * nmemb;
    size_t total_read = 0;
    //uint8_t *dest = (uint8_t *)ptr;

    // 2. 确保 sys_fread 的请求量不超过文件剩余大小
    // 假设 FILE 结构体中有 file_size (总大小) 和 offset (当前偏移)
    if (stream->file_size > 0) {
        if (stream->offset >= stream->file_size) {
            return 0; // 已经到达文件末尾
        }
        
        size_t remaining_in_file = stream->file_size - stream->offset;
        if (bytes_to_read > remaining_in_file) {
            bytes_to_read = remaining_in_file; // 截断请求量，绝不越界
        }
    }
    
    if (bytes_to_read == 0) return 0;

    total_read = sys_fread(stream->fd,(uint64_t)ptr,bytes_to_read);

    // 返回成功读取的“元素个数”
    return total_read / size;
}
#include <errno.h>
// ==========================================
// fseek 与 ftell (纯粹依靠内核维护的架构)
// ==========================================
int32_t fseek(FILE* stream, int64_t offset, int32_t whence) {
    if (!stream) return -1;

    // 【关键动作】：因为用户进行了跳转，当前预读的 4KB 缓冲数据全部作废（缓存失效）
    stream->buf_pos = 0;
    stream->buf_size = 0;

    switch (whence) {
	case SEEK_SET:
		if (offset < 0 || (uint64_t)offset > stream->file_size)
			return EINVAL;

		stream->offset = offset;
		break;
	case SEEK_CUR:
		if ((offset < 0 && (uint64_t)(-offset) > stream->offset) ||
		    (offset > 0 &&
		     (uint64_t)offset > (stream->file_size - stream->offset)))
			return EINVAL;

		stream->offset += offset;
        break;
	case SEEK_END:
		if (offset < 0 || (uint64_t)offset > stream->file_size)
			return EINVAL;

		stream->offset = stream->file_size - offset;
		break;
	}
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