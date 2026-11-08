//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * tests/stdc/host_fs_shim.c — 宿主测试用的内核 syscall 桩
 *
 * lib/stdc/stdio.c 与 time.c 在用户态通过 sys_* 包装调用内核文件系统 /
 * RTC; 宿主进程里不能真的执行 Skyline 系统调用 (x86_64 syscall 17 是
 * pread64!)。这里用:
 *   - 单个内存文件模拟 sys_fopen/sys_fread/sys_fwrite/sys_flseek/
 *     sys_fclose/sys_fsize;
 *   - console_stdout_write() 把 stdout/stderr 输出捕获到内存, 供断言;
 * 测试通过 shim_console_* 访问捕获内容。
 *
 * 编译: 与 lib/stdc 相同的头文件路径 (见 tests/stdc/Makefile)。
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <base/arch/x86_64/syscall.h>

#define SHIM_FS_CAP 8192

static unsigned char shim_fs_data[SHIM_FS_CAP];
static size_t shim_fs_size;
static size_t shim_fs_pos;
static int shim_fs_open;

uint64_t sys_fopen(uint64_t path, uint64_t flags) {
    const char *p = (const char *)(uintptr_t)path;

    if (!p || p[0] == '\0') return (uint64_t)-1;
    /* 模拟"打开不存在的文件": 内核 ABI 返回**负 errno** */
    if (strcmp(p, "missing.txt") == 0) return (uint64_t)(int64_t)(-ENOENT);
    if ((flags & (uint64_t)O_TRUNC) != 0) shim_fs_size = 0;
    shim_fs_pos = ((flags & (uint64_t)O_APPEND) != 0) ? shim_fs_size : 0;
    shim_fs_open = 1;
    return (uint64_t)100;   /* 伪 fd */
}

uint64_t sys_fread(int32_t fd_idx, uint64_t buf, uint64_t count) {
    size_t avail;
    size_t n;

    (void)fd_idx;
    if (!shim_fs_open) return 0;
    avail = shim_fs_size - shim_fs_pos;
    n = ((size_t)count < avail) ? (size_t)count : avail;
    if (n > 0) {
        memcpy((void *)(uintptr_t)buf, shim_fs_data + shim_fs_pos, n);
        shim_fs_pos += n;
    }
    return (uint64_t)n;
}

uint64_t sys_fwrite(int32_t fd_idx, uint64_t buf, uint64_t count) {
    size_t n;

    (void)fd_idx;
    if (!shim_fs_open) return 0;
    n = (size_t)count;
    if (n > (size_t)SHIM_FS_CAP - shim_fs_pos) n = (size_t)SHIM_FS_CAP - shim_fs_pos;
    if (n > 0) {
        memcpy(shim_fs_data + shim_fs_pos, (const void *)(uintptr_t)buf, n);
        shim_fs_pos += n;
    }
    if (shim_fs_pos > shim_fs_size) shim_fs_size = shim_fs_pos;
    return (uint64_t)n;
}

uint64_t sys_flseek(int32_t fd_idx, uint64_t offset, uint64_t whence) {
    int64_t newoff;

    (void)fd_idx;
    switch ((int)whence) {
        case 0:     /* SEEK_SET */
            newoff = (int64_t)offset;
            break;
        case 1:     /* SEEK_CUR */
            newoff = (int64_t)shim_fs_pos + (int64_t)offset;
            break;
        case 2:     /* SEEK_END */
            newoff = (int64_t)shim_fs_size + (int64_t)offset;
            break;
        default:
            return (uint64_t)-1;
    }
    if (newoff < 0 || (uint64_t)newoff > (uint64_t)shim_fs_size)
        return (uint64_t)-1;
    shim_fs_pos = (size_t)newoff;
    return (uint64_t)newoff;
}

uint64_t sys_fsize(int32_t fd_idx) {
    (void)fd_idx;
    return (uint64_t)shim_fs_size;
}

uint64_t sys_fclose(int32_t fd_idx) {
    (void)fd_idx;
    shim_fs_open = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* stdout/stderr 捕获                                                   */
/* ------------------------------------------------------------------ */
static char shim_console_buf[4096];
static size_t shim_console_used;

void console_stdout_write(const char *s, size_t n) {
    size_t i;

    if (!s) return;
    for (i = 0; i < n && shim_console_used + 1 < sizeof(shim_console_buf); i++)
        shim_console_buf[shim_console_used++] = s[i];
    shim_console_buf[shim_console_used] = '\0';
}

void shim_console_reset(void) {
    shim_console_used = 0;
    shim_console_buf[0] = '\0';
}

unsigned long shim_console_len(void) {
    return (unsigned long)shim_console_used;
}

const char *shim_console_data(void) {
    return shim_console_buf;
}
