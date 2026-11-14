//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * elf_image.h —— 内核态"可靠读取 + 校验"一份 ELF 镜像 (可执行文件 / .so)
 * ----------------------------------------------------------------------------
 * 背景 (2026-11 偶发启动卡死, 见 docs/kernel-components.md "ELF 镜像读取"):
 *   原来的 exec/task/elf_dyn 三个加载器都是
 *       MP->FSOPS->read(FileDesc, buffer, FSize, 0);
 *   **不检查返回值**, 也不检查实际读到的字节数。ext4_fread 在块设备层返回
 *   EIO 时会**提前结束**并只填好前一半数据 (见 lwext4 src/ext4.cpp 的
 *   `goto Finish`), 于是 kmalloc 出来的未初始化堆内存被当成 ELF 头解析 ——
 *   表现是 `[dynlink] libsky.so: bad ELF header` / 随机崩溃 / 桌面起不来,
 *   而且概率取决于启动期磁盘 I/O 有没有撞上一次瞬时错误 (薛定谔式卡死)。
 *
 * 这里把"读整个文件"和"校验 ELF 头 + 各段边界"收敛成一处, 三个加载器共用:
 *   * 读取: 校验 rc 与 rcnt, 必须 rc==0 且 rcnt==fsize, 否则整文件重读;
 *   * 重读前先清零缓冲区, 保证即使再次短读也不会把堆垃圾当 ELF 内容;
 *   * 失败时打印 路径 / 第几次尝试 / rc / 实读字节 / 期望字节;
 *   * 校验: e_ident 魔数、ELFCLASS64、EM_X86_64、e_type、程序头表边界、
 *           每个 PT_LOAD 的 [p_offset, p_offset+p_filesz) 都在文件内。
 * ==========================================================================*/
#pragma once

#include <stdint.h>

/* 读取整个文件到 out_buf (kmalloc 分配, 调用方负责 kfree)。
 * 成功: 返回 0, out_buf / out_len 有效;
 * 失败: 返回负 errno, out_buf == nullptr (绝不返回半成品缓冲区)。 */
int kelf_read_file(const char *path, uint8_t **out_buf, uint64_t *out_len);

/* 校验一段 ELF 镜像是否自洽 (只做"能不越界地解析"的检查, 不做重定位检查)。
 * tag: 日志里用来标识镜像的来源 (路径或 "main"/"libsky.so")。
 * 成功返回 0, 失败返回 -EINVAL 并打印原因。 */
int kelf_validate_image(const char *tag, const uint8_t *buf, uint64_t len);
