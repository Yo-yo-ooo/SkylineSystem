//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_file.cpp —— 内建组件 kernel.syscall.file：文件类 syscall 实现
 * ============================================================================
 * 整个内核组件化 (B2)：syscall 的**编号/参数/返回/错误码/语义**仍由 Tier0
 * 固定契约表 (kmod/syscall_registry.cpp) 管，这里只提供"实现"。
 *
 * 家族槽位（syscalln.h 固定编号）：
 *   0 fopen   -> sys_fopen
 *   1 fwrite  -> sys_fwrite
 *   2 fread   -> sys_fread
 *   3 fclose  -> sys_fclose
 *   4 flseek  -> sys_flseek
 *   5 fsize   -> sys_fsize
 *  22 mkdir   -> sys_mkdir        (Setup 安装器建 /mp/system 目录树)
 *  23 readdir -> sys_readdir      (Explorer 枚举 ext4 目录)
 *
 * 导出条目由 KCOMPONENT_EXPORT_SYSCALL 写入 .kbuiltin.out.<ID>；启动期由
 * Tier0 组件管理器按拓扑序提交：KmodModuleAllocBuiltin -> BindImports ->
 * KmodApplyExports (syscall_registry 注册实现) -> init -> READY。
 * 语义不变：实现函数就是迁移前的 sys_* 本体，签名即固定 dispatch ABI
 * (6×u64 + frame*)，内核不生成任何适配层。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_file, "kernel.syscall.file", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT, (uint64_t)KMOD_CAP_FS, 0,
                 "syscall file family (slots 0-5,22,23): open/read/write/seek/size/mkdir/readdir",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

/* VFS 核心必须先于文件类 syscall 家族就绪（组件图保证 provider 先序）。 */
KCOMPONENT_DEPENDS(syscall_file, "kernel.sched.core", sched)
KCOMPONENT_DEPENDS(syscall_file, "kernel.fs.vfs", vfs)

KCOMPONENT_EXPORT_SYSCALL(syscall_file, 0u, sys_fopen)
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 1u, sys_fwrite)
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 2u, sys_fread)
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 3u, sys_fclose)
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 4u, sys_flseek)
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 5u, sys_fsize)
/* 固定槽 22：mkdir（原 reserved 空洞；Setup 需要建目录树）。契约表同名
   "mkdir"，ABI 固定 (path, mode) -> 0 / -errno，仅 ext4 支持。 */
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 22u, sys_mkdir)
/* 固定槽 23：readdir（原 reserved 空洞；Explorer 真正枚举 ext4 目录）。
   ABI 固定 (path, buf, cap, flags) -> 写入字节数 / -errno，仅 ext4 支持。 */
KCOMPONENT_EXPORT_SYSCALL(syscall_file, 23u, sys_readdir)

KCOMPONENT_END(syscall_file)
