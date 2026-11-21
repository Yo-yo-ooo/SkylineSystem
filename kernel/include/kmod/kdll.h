//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kdll.h —— 统一内核动态链接层 (Kernel DynLink / DynModule)
 * ============================================================================
 * 定位：Tier 1（内建组件）与 Tier 2（外部 .ko）合并后的唯一模块模型。
 *
 * 一个 DynModule = manifest（身份/能力/生命周期）+ 导入表（动态链接边）+
 * 导出表（函数/数据/私有契约/座位/syscall）。镜像来源只有两种后端，但对外
 * 语义完全相同：
 *
 *   resident  image : 代码已编进内核镜像（原 Tier 1 内建组件与 Tier 0 核心
 *                     服务）。加载 = 注册节点 + 绑定导入 + 调用 init；
 *   relocatable image: 外部 ET_REL .ko（原 Tier 2）。加载 = 映射/重定位 +
 *                     绑定导入 + 调用 init。
 *
 * 在此之上提供 DLL 式注入：
 *   - KdllInject(name, KMOD_INJ_PIN)   LoadLibrary；
 *   - KdllResolve(provider, symbol)    GetProcAddress；
 *   - KdllEject(name)                  FreeLibrary（引用归零且可卸载才真正卸载）；
 *   - KdllInjectFile(path, flags)      按文件系统路径加载 .ko。
 *
 * 模块侧通过 KMOD_IMPORT_DYNLINK() / KCOMPONENT_IMPORT_DYNLINK() 取得
 * kernel.dynlink.v1 契约；治理层在绑定该契约时要求 KMOD_CAP_MODULE_LOAD。
 *
 * 核心常驻服务（pmm/vmm/heap/syscall 入口）以 resident DynModule 形式登记，
 * 因此 dosvm 之类的模块可以显式 "inject kernel.mem.pmm / kernel.arch.acpi"，
 * 与注入普通 .ko 走同一条路径。
 * ==========================================================================*/
#ifndef _KDLL_H_
#define _KDLL_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 常驻核心服务名（KdllInit 在模块表里登记为 READY 的 resident DynModule） */
#define KDLL_CORE_PMM       "kernel.mem.pmm"
#define KDLL_CORE_VMM       "kernel.mem.vmm"
#define KDLL_CORE_HEAP      "kernel.mem.heap"
#define KDLL_CORE_SYSCALL   "kernel.arch.syscall"

/* 初始化统一动态链接层：清空注入计数并登记常驻核心服务。
 * 必须在 KmodGovInit/KmodSyscallInit 之后、KmodBootComponents 之前调用。 */
int32_t KdllInit(void);

/* 把一个常驻服务登记为 READY 的 resident DynModule（幂等）。
 * 由 KdllInit 内部使用；模块也可以注册自己的常驻服务（测试/扩展）。 */
int32_t KdllRegisterResident(const char *name, const char *version,
                             uint64_t caps, const char *description);

/* DLL 式注入：按名字确保模块已动态链接。
 * 查找顺序：已加载模块 -> resident/builtin 描述符 -> 镜像池 -> 文件路径
 * （name 以 '/' 开头或带 KMOD_INJ_FILE）。依赖按 provider 自动注入。
 * flags: KMOD_INJ_PIN / KMOD_INJ_SILENT / KMOD_INJ_FILE。 */
int32_t KdllInject(const char *name, uint32_t flags);

/* 从内存中的 ET_REL 镜像注入（复制镜像；调用方保留 data 所有权）。 */
int32_t KdllInjectImage(const void *data, uint64_t size, uint32_t flags);

/* 从文件系统读取 .ko 并注入（真正的按路径加载）。 */
int32_t KdllInjectFile(const char *path, uint32_t flags);

/* 释放一次 PIN；pin 归零 + refcount 归零 + 声明 UNLOADABLE 才真正卸载。
 * 常驻服务/内建组件只释放 PIN，不卸载代码。 */
int32_t KdllEject(const char *name);

/* GetProcAddress：在已 READY 模块的导出表中解析符号。 */
void *KdllResolve(const char *provider, const char *symbol);

/* 当前模块持有的注入 PIN 数（观测/自检）。 */
uint32_t KdllPinCount(const char *name);

/* 已登记的常驻核心服务数量。 */
uint32_t KdllResidentCount(void);

/* 打印 DynModule 注入统计（模块表由 KmodDump 一并输出）。 */
int32_t KdllDump(void);

#ifdef __cplusplus
}
#endif

#endif /* _KDLL_H_ */
