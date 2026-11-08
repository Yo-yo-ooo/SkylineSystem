//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * tests/stdc/host_printf_shim.c — 宿主版 vsnprintf_ 符号
 *
 * lib/stdc/outfb/printf.c 依赖 flanterm/graphic 子系统, 无法在宿主链接;
 * 但 lib/stdc/stdio.c 的 fprintf/vfprintf 需要 vsnprintf_ 做格式化。
 * 这里用宿主 glibc 的 vsnprintf 提供同签名符号, 从而在宿主上完整测试
 * stdio.c 的 "格式化 -> fwrite -> 流" 管线 (printf.c 本身不在本测试范围)。
 *
 * 编译: **不带** 本仓库头文件 (见 tests/stdc/Makefile 的 SYS_CFLAGS),
 *       避免本仓库 stdio.h 的 vsnprintf/snprintf 宏重命名干扰。
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

int32_t vsnprintf_(char *buffer, size_t count, const char *format, va_list va) {
    return (int32_t)vsnprintf(buffer, count, format, va);
}
