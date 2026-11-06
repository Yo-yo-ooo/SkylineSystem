//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod.h —— 内核侧 (Tier0) 模块加载与治理公共 API
 * ============================================================================
 * 该头由内核内部代码、系统调用实现与宿主测试使用；.ko 模块作者用
 * kmod_module.h（只依赖 kmod_abi.h）。两者唯一的共享契约是 kmod_abi.h。
 *
 * 分层（对应 docs/kernel-modules.md "Tier 0 固定"）：
 *   Tier0/Tier1 机制:  本头 + elf_loader + governance + capability + topology
 *   Tier1+ 可模块化:   .ko 提供的 policy/impl/seat/syscall
 * ==========================================================================*/
#ifndef _KMOD_H_
#define _KMOD_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>
#include <kmod/kmod_seats.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 运行时状态机（对外快照用；内部枚举在 kmod_internal.h）               */
/* ------------------------------------------------------------------ */
#define KMOD_STATE_EMPTY        0u
#define KMOD_STATE_LOADING      1u
#define KMOD_STATE_RESOLVING    2u
#define KMOD_STATE_READY        3u
#define KMOD_STATE_QUIESCING    4u
#define KMOD_STATE_REPLACING    5u
#define KMOD_STATE_UNLOADING    6u
#define KMOD_STATE_FAILED       7u
#define KMOD_STATE_REMOVED      8u

/* KmodLoad() 行为 flags */
#define KMOD_LOAD_NORMAL        0u
#define KMOD_LOAD_FORCE         (1u << 0)  /* OPEN 策略下强制加载实验性模块 */
#define KMOD_LOAD_SILENT        (1u << 1)  /* 不打印常规加载日志 */
#define KMOD_LOAD_DEPENDENCY    (1u << 2)  /* 内部：作为依赖被拉起 */

/* ------------------------------------------------------------------ */
/* 观测快照                                                            */
/* ------------------------------------------------------------------ */
typedef struct kmod_module_info {
    char name[KMOD_NAME_MAX];
    char version[KMOD_VERSION_MAX];
    uint16_t tier;
    uint16_t state;
    uint32_t flags;
    uint64_t caps_granted;
    uint64_t caps_denied;
    uint32_t refcount;          /* 外部持有者：消费者导入边 + 座位 + syscall */
    uint32_t epoch_active;      /* 当前 in-flight 调用数 */
    uint32_t n_imports;
    uint32_t n_exports;
    uint32_t n_deps;
    uint32_t n_consumers;
    uint32_t image_size;
    uint32_t generation;
} kmod_module_info_t;

typedef struct kmod_stats {
    uint64_t loads_ok;
    uint64_t loads_fail;
    uint64_t unloads_ok;
    uint64_t unloads_fail;
    uint64_t hotswaps_ok;
    uint64_t hotswaps_fail;
    uint64_t rollbacks;
    uint64_t reject_cap;
    uint64_t reject_cycle;
    uint64_t reject_version;
    uint64_t reject_format;
    uint64_t reject_reloc;
    uint64_t reject_name;
    uint64_t reject_policy;
    uint64_t reject_busy;
    uint64_t seat_swaps;
    uint64_t syscall_binds;
    uint64_t syscall_overrides;
    uint64_t epoch_waits;
    uint64_t budget_exceeded;
    uint64_t selftest_pass;
    uint64_t selftest_fail;
} kmod_stats_t;

/* ------------------------------------------------------------------ */
/* 生命周期 / 加载 / 卸载 / 热替换                                     */
/* ------------------------------------------------------------------ */
int32_t KmodInit(void);            /* Tier0: 注册表/导出表/座位/syscall 契约表初始化 */
/* Tier0 内建组件图初始化：扫描 .kbuiltin.desc、按拓扑图 bind/apply/init。
 * 这是"整个内核组件化"的启动序列入口（取代硬编码 init 长链）。 */
int32_t KmodBootComponents(void);
uint32_t KmodComponentCount(void);
int32_t KmodBootIngest(void);      /* 把引导期拿到的 .ko 镜像装入镜像池（不加载） */
int32_t KmodBootLoad(void);        /* 加载 KMOD_MF_BOOT 模块（按依赖拓扑顺序） */
int32_t KmodLoad(const char *name, uint32_t flags);
/* 指定版本加载（热替换/多版本镜像池场景；version=NULL/"" = 池内最高版本） */
int32_t KmodLoadVersion(const char *name, const char *version, uint32_t flags);
int32_t KmodUnload(const char *name);
/* 热替换到镜像池中 name 模块的 new_version（"x.y.z"；NULL/"" = 池内最高版本）。 */
int32_t KmodHotReplace(const char *name, const char *new_version);
int32_t KmodSelfTest(void);
int32_t KmodDump(void);            /* 打印拓扑图/能力/状态/统计（串口可观测） */
const char *KmodElfErrorString(int32_t rc);  /* 错误码 -> 诊断串（观测/启动日志） */

const kmod_stats_t *KmodStats(void);
uint32_t KmodModuleCount(void);
int32_t KmodModuleInfoAt(uint32_t index, kmod_module_info_t *out);
int32_t KmodModuleState(const char *name);

/* epoch 治理：模块提供的 ops/syscall 在被调用期间持有 epoch，热替换/卸载
 * 必须等 epoch 排空才能切换或释放（看门狗超时则失败回退）。
 * 返回 KMOD_OK 表示允许进入；模块状态非 READY/正在静默时返回 KMOD_ERR_BUSY。 */
int32_t KmodEpochEnter(const char *name);
void KmodEpochExit(const char *name);

/* ------------------------------------------------------------------ */
/* syscall 固定入口挂钩（Tier0 dispatch 调用；见 syscall.cpp）          */
/* ------------------------------------------------------------------ */
typedef uint64_t (*kmod_syscall_fn)(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5,
                                    void *frame);

/* 固定入口在槽位上是"治理哨兵"时调用的唯一入口；frame 为 syscall_frame_t* */
uint64_t KmodSyscallEntry(void *frame);
/* Tier0 dispatch 用：返回哨兵指针，用于和 syscall_lists[slot] 比较 */
void *KmodSyscallSentinel(void);

/* SYSCALL_KMOD_INFO 的用户态快照布局（固定契约，只读） */
typedef struct kmod_sysinfo_record {
    char name[KMOD_NAME_MAX];
    char version[KMOD_VERSION_MAX];
    uint16_t tier;
    uint16_t state;
    uint32_t flags;
    uint64_t caps_granted;
    uint64_t caps_denied;
    uint32_t refcount;
    uint32_t epoch_active;
    uint32_t n_imports;
    uint32_t n_exports;
    uint32_t n_deps;
    uint32_t n_consumers;
} kmod_sysinfo_record_t;

typedef struct kmod_sysinfo {
    uint32_t abi_version;   /* = KMOD_ABI_VERSION */
    uint32_t count;         /* 本次写入的记录数 */
    uint32_t total;         /* 当前模块总数 */
    uint32_t reserved;
    kmod_stats_t stats;
    /* kmod_sysinfo_record_t records[] 紧随其后（内核按调用方 size 截断写入） */
} kmod_sysinfo_t;

#ifdef __cplusplus
}
#endif

#endif /* _KMOD_H_ */
