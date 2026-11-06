//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_abi.h —— 私有契约型可加载内核模块 (kmod) 的模块侧 ABI (唯一契约头)
 * ============================================================================
 * 定位：本文件定义的是"运行在内核态、面向宏内核增强的私有契约型可加载模块
 * 链接与治理机制"的**线格式**。它是内核与 .ko 之间唯一固定不变的二进制边界，
 * 与内核内部实现解耦：
 *
 *   .in               ELF 段：导入声明数组 (kmod_import_t)
 *                       { 导入模块的名称 + 导入符号在该模块内被回填的地址槽 }
 *   .out              ELF 段：导出符号数组 (kmod_export_t)
 *                       函数 / 数据 / 私有契约 / 座位(seat) / 系统调用实现
 *   .kmod_manifest    ELF 段：模块清单 (kmod_manifest_t)
 *                       名称、版本、Tier、能力声明、生命周期回调、时间预算
 *
 * 设计红线（与 docs/kernel-modules.md 一一对应）：
 *   1. 模块之间如何约定私有函数、槽位、结构体布局、版本 = 模块自己的事；
 *      内核**只**做依赖加载、导出表暴露、地址回填、能力治理、失败回滚。
 *   2. 内核不生成适配器、不做胶水转换、不强制全局统一业务接口。
 *   3. 私有契约一律带命名空间 (例如 "vendor.sched.v1")，并带版本与结构尺寸，
 *      由内核在做地址回填时做机械校验（不解释语义）。
 *   4. 系统调用编号/参数/返回/错误码/语义固定；可注册的是"实现"；
 *      新增 syscall 只能走动态命名空间槽位。
 * ==========================================================================*/
#ifndef _KMOD_ABI_H_
#define _KMOD_ABI_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 版本与魔数                                                          */
/* ------------------------------------------------------------------ */
#define KMOD_ABI_MAGIC          0x4B4D4F44u   /* 'KMOD' */
#define KMOD_MANIFEST_MAGIC     0x4D414E49u   /* 'MANI' */
#define KMOD_ABI_VERSION        1u

/* 编译期 ABI 指纹：任何 kmod_abi.h 结构布局变更必须同时 +1 并把下面旧值保留 */
#define KMOD_ABI_FINGERPRINT    0x00010001u

/* ------------------------------------------------------------------ */
/* Tier：Tier 0 固定不可模块化，Tier 1+ 可模块化                       */
/* ------------------------------------------------------------------ */
#define KMOD_TIER_CORE          0u   /* Tier 0: 引导/页表/中断/上下文/syscall 入口/
                                        模块加载器/回滚机制/能力注册表。.ko 不得声明 */
#define KMOD_TIER_POLICY        1u   /* Tier 1: 调度策略/分配器/文件系统/驱动/协议栈 */
#define KMOD_TIER_IMPL          2u   /* Tier 2: 具体实现、扩展、诊断 */

/* ------------------------------------------------------------------ */
/* 模块清单 flags                                                      */
/* ------------------------------------------------------------------ */
#define KMOD_MF_BOOT            (1u << 0)  /* 引导期自动加载 */
#define KMOD_MF_HOTREPLACE      (1u << 1)  /* 允许参与热替换（导出可被原子重绑定） */
#define KMOD_MF_UNLOADABLE      (1u << 2)  /* 允许卸载（默认禁止，显式声明才可） */
#define KMOD_MF_DEBUG           (1u << 3)  /* 诊断模块（严格策略下额外审计） */
#define KMOD_MF_NO_AUTODEP      (1u << 4)  /* 不自动拉取依赖：缺依赖即失败 */
#define KMOD_MF_EXPERIMENTAL    (1u << 5)  /* 实验性模块，严格策略下默认拒绝 */

/* ------------------------------------------------------------------ */
/* 能力位（capability）：治理层审计/授权用，不是内核态安全沙箱边界      */
/* ------------------------------------------------------------------ */
#define KMOD_CAP_MEM_ALLOC      (1ull << 0)   /* 使用内核分配器 */
#define KMOD_CAP_MEM_DMA        (1ull << 1)   /* 物理高地址/连续页、DMA */
#define KMOD_CAP_LOG            (1ull << 2)   /* 内核日志出口 */
#define KMOD_CAP_IRQ            (1ull << 3)   /* 中断注册/分发 */
#define KMOD_CAP_MMIO           (1ull << 4)   /* MMIO 映射 */
#define KMOD_CAP_DEVICE         (1ull << 5)   /* 设备注册 */
#define KMOD_CAP_BLOCK          (1ull << 6)   /* 块设备/分区 */
#define KMOD_CAP_FS             (1ull << 7)   /* 文件系统实现挂载 */
#define KMOD_CAP_NET            (1ull << 8)   /* 协议栈/网卡 */
#define KMOD_CAP_SCHED_POLICY   (1ull << 9)   /* Tier1 调度策略座位 */
#define KMOD_CAP_ALLOC_POLICY   (1ull << 10)  /* Tier1 分配器座位 */
#define KMOD_CAP_SYSCALL_IMPL   (1ull << 11)  /* 注册动态命名空间 syscall 实现 */
#define KMOD_CAP_SYSCALL_OVERRIDE (1ull << 12)/* 覆写"可替换实现"的固定契约 syscall */
#define KMOD_CAP_SEAT_PROVIDER  (1ull << 13)  /* 提供 Tier1 座位实现 */
#define KMOD_CAP_HOT_REPLACE    (1ull << 14)  /* 发起/参与热替换 */
#define KMOD_CAP_MODULE_LOAD    (1ull << 15)  /* 触发加载其他模块 */
#define KMOD_CAP_MODULE_UNLOAD  (1ull << 16)  /* 触发卸载 */
#define KMOD_CAP_DEBUG_MEM      (1ull << 17)  /* 内存诊断/毒化 */
#define KMOD_CAP_EXPERIMENTAL   (1ull << 18)  /* 实验性能力 */

/* 内核保留能力位：Tier 0 机制自身使用，任何 .ko 声明即拒绝加载 */
#define KMOD_CAP_RESERVED_CORE  (1ull << 63)
#define KMOD_CAP_ALL_KNOWN      (0x7FFFFFFFFull)   /* bit0..bit34 */
#define KMOD_CAP_MODULE_MASK    (KMOD_CAP_ALL_KNOWN)

/* ------------------------------------------------------------------ */
/* 导出种类 (kmod_export_t.kind)                                       */
/* ------------------------------------------------------------------ */
#define KMOD_SYM_FUNC           1u  /* 普通函数指针 */
#define KMOD_SYM_DATA           2u  /* 数据对象 */
#define KMOD_SYM_CONTRACT       3u  /* 私有契约 ops 结构体（核不做语义解释） */
#define KMOD_SYM_SEAT           4u  /* Tier1 座位实现（内核固定机制的策略提供者） */
#define KMOD_SYM_SYSCALL        5u  /* syscall 实现（aux = 槽位号） */
#define KMOD_SYM_NAMESPACE      6u  /* 声明动态 syscall 命名空间（symbol = 命名空间） */

/* 导出 flags */
#define KMOD_EXF_HOTREPLACEABLE (1u << 0)   /* 该导出允许热替换重绑定 */
#define KMOD_EXF_OPTIONAL       (1u << 1)   /* 无消费者时不阻止卸载 */

/* ------------------------------------------------------------------ */
/* 导入 flags                                                          */
/* ------------------------------------------------------------------ */
#define KMOD_IMP_REQUIRED       0u
#define KMOD_IMP_OPTIONAL       (1u << 0)   /* 缺失不阻止加载，槽保持 NULL */
#define KMOD_IMP_WEAK           (1u << 1)   /* 弱依赖：不强制拉取 provider */
#define KMOD_IMP_DEP_EDGE       (1u << 2)   /* 纯模块级依赖（symbol/slot 为空） */

/* ------------------------------------------------------------------ */
/* 段名（.ko 必须使用这三个段；内核加载器按名解析）                    */
/* ------------------------------------------------------------------ */
#define KMOD_SEC_IN             ".in"
#define KMOD_SEC_OUT            ".out"
#define KMOD_SEC_MANIFEST       ".kmod_manifest"
/* 内核自身导出表段（Tier 0 内核镜像内，非 .ko） */
#define KMOD_SEC_KERNEL_OUT     ".kout"

/* ------------------------------------------------------------------ */
/* 内核导出/私有契约命名空间约定                                       */
/* ------------------------------------------------------------------ */
/* 内核核心导出契约（Tier 0 固定，模块只能使用不能替换） */
#define KMOD_CONTRACT_KERNEL_MEM    "kernel.mem.v1"
#define KMOD_CONTRACT_KERNEL_LOG    "kernel.log.v1"
#define KMOD_CONTRACT_KERNEL_SYNC   "kernel.sync.v1"   /* 自旋锁/中断状态 (kmod_services.h) */
#define KMOD_CONTRACT_KERNEL_EPOCH  "kernel.epoch.v1"
#define KMOD_CONTRACT_KERNEL_SYSCALL "kernel.syscall.v1"
#define KMOD_CONTRACT_KERNEL_KLIB   "kernel.klib.v1"   /* klib 工具面 ops (C++ 链接工具) */
#define KMOD_CONTRACT_KERNEL_ALGO   "kernel.algo.v1"   /* klib 容器面 ops (hashmap/art/queue/fifo/list) */
/* 私有契约命名空间格式（仅做字符串规则校验，不做语义解释）：
 *   <vendor>.<area>.<vN>  例如 vendor.sched.v1 / acme.fs.v2
 * 保留前缀：kernel. / core. 只允许 Tier0 内核镜像自身声明。 */
#define KMOD_NS_SYSCALL_PREFIX  "syscall."

/* ------------------------------------------------------------------ */
/* syscall 治理边界（固定 entry 在 Tier 0；槽位是唯一固定契约面）      */
/* ------------------------------------------------------------------ */
#define KMOD_SYSCALL_MAX         512u
#define KMOD_SYSCALL_CORE_MAX     64u   /* 0..63 固定契约槽 */
#define KMOD_SYSCALL_DYNAMIC_MIN 128u   /* 128..511 动态命名空间槽 */
#define KMOD_SYSCALL_DYNAMIC_MAX 511u

/* 固定契约槽 flags (见内核 syscall contract 表；.ko 不可改) */
#define KMOD_SC_FIXED          (1u << 0)  /* 编号/参数/返回/错误码/语义固定 */
#define KMOD_SC_SWAPPABLE      (1u << 1)  /* 允许以治理事务替换"实现" */
#define KMOD_SC_NAMESPACED     (1u << 2)  /* 动态命名空间槽 */
#define KMOD_SC_KMOD           (1u << 3)  /* kmod 治理自省槽 */

/* kmod 固定契约 syscall：由 Tier0 内核直接实现，数字/语义固定 */
#define KMOD_SYSCALL_INFO        29u  /* SYSCALL_KMOD_INFO: 写模块表快照，返回项数 */
#define KMOD_SYSCALL_PROBE       30u  /* SYSCALL_KMOD_PROBE: 诊断探针槽（实现可替换） */
#define KMOD_SYSCALL_PROBE_MAGIC 0x4B4D4F44u

/* 座位(seat)：内核固定机制 + 模块提供策略/实现。座位名即契约命名空间。 */
#define KMOD_SEAT_RANDOM         "kernel.seat.random.v1"   /* 随机数提供者 */
#define KMOD_SEAT_MAX            16u

/* ------------------------------------------------------------------ */
/* 错误码（init/exit/quiesce/drain 回调与内核 API 共用）               */
/* ------------------------------------------------------------------ */
#define KMOD_OK                  0
#define KMOD_ERR_INVAL          (-1)
#define KMOD_ERR_NOMEM          (-2)
#define KMOD_ERR_FORMAT         (-3)
#define KMOD_ERR_UNRESOLVED     (-4)
#define KMOD_ERR_VERSION        (-5)
#define KMOD_ERR_CYCLE          (-6)
#define KMOD_ERR_CAP            (-7)
#define KMOD_ERR_TIER           (-8)
#define KMOD_ERR_STATE          (-9)
#define KMOD_ERR_EXISTS         (-10)
#define KMOD_ERR_NOTFOUND       (-11)
#define KMOD_ERR_BUSY           (-12)
#define KMOD_ERR_BUDGET         (-13)
#define KMOD_ERR_ROLLBACK       (-14)
#define KMOD_ERR_CONTRACT       (-15)
#define KMOD_ERR_RELOC          (-16)
#define KMOD_ERR_NAME           (-17)
#define KMOD_ERR_IMAGE          (-18)
#define KMOD_ERR_POLICY         (-19)
#define KMOD_ERR_DEPTH          (-20)

/* ------------------------------------------------------------------ */
/* 载荷上限（防御性边界；超限直接拒绝，分配前检查）                    */
/* ------------------------------------------------------------------ */
#define KMOD_NAME_MAX            48u
#define KMOD_VERSION_MAX         24u
#define KMOD_CONTRACT_MAX        48u
#define KMOD_SYMBOL_MAX          64u
#define KMOD_DESC_MAX            80u
#define KMOD_MAX_MODULES         64u
#define KMOD_MAX_IMAGES          64u
#define KMOD_MAX_IMPORTS         64u
#define KMOD_MAX_EXPORTS         128u
#define KMOD_MAX_MODULE_DEPS     32u
#define KMOD_MAX_MODULE_CONSUMERS 64u
#define KMOD_MAX_ELF_SIZE        (8u << 20)
#define KMOD_MAX_SECTIONS        64u
#define KMOD_INIT_BUDGET_MAX_MS  60000u
#define KMOD_QUIESCE_BUDGET_MAX_MS 10000u

/* ------------------------------------------------------------------ */
/* 线格式结构体（布局冻结；扩展走 abi_version 升级）                   */
/* ------------------------------------------------------------------ */

/* 模块清单：.kmod_manifest 段内唯一对象。
 * 注意：名称/版本/描述使用**内联字符数组**而非指针 —— 这样镜像池入库阶段
 * 不做重定位也能安全读取身份信息（.in/.out 中的地址仍由加载器统一回填）。 */
typedef struct kmod_manifest {
    uint32_t magic;                 /* KMOD_MANIFEST_MAGIC */
    uint16_t abi_version;           /* KMOD_ABI_VERSION */
    uint16_t tier;                  /* KMOD_TIER_*，必须是 POLICY 或 IMPL */
    uint32_t flags;                 /* KMOD_MF_* */
    char name[KMOD_NAME_MAX];       /* 命名空间模块名，如 "vendor.demo.hello" */
    char version[KMOD_VERSION_MAX]; /* "x.y.z" */
    char description[KMOD_DESC_MAX];/* 可空 */
    uint64_t required_caps;         /* 必须全部授予，否则拒绝加载 */
    uint64_t optional_caps;         /* 能授则授，否则记录审计 */
    int32_t (*init)(void);          /* 加载提交阶段调用，返回 KMOD_OK 才提交 */
    void (*exit_hook)(void);        /* 卸载/替换前调用（已 quiesce） */
    void (*quiesce)(void);          /* 停止接收新工作（配合 epoch 排空） */
    int32_t (*drain)(void);         /* 返回剩余 in-flight 数；0 = 已排空 */
    uint32_t init_budget_ms;        /* 看门狗预算；0 = 用内核默认 */
    uint32_t quiesce_budget_ms;     /* 静默/排空预算；0 = 用内核默认 */
    const char *const *requires;    /* 声明的私有契约需求（审计用，内核不解释） */
    uint32_t n_requires;
    const char *const *provides;    /* 声明的私有契约提供（审计用） */
    uint32_t n_provides;
} kmod_manifest_t;

/* 导出项：.out 段数组元素（按 8 字节对齐，sizeof 必须是段大小的整除因子） */
typedef struct kmod_export {
    const char *symbol;       /* 导出符号名（模块内唯一） */
    const char *contract;     /* 私有契约命名空间，可为空串；内核不做语义解释 */
    void *addr;               /* 符号地址（加载期重定位后有效） */
    uint32_t size;            /* CONTRACT/SEAT: ops 结构体字节数（布局校验）；FUNC: 0 */
    uint32_t abi_version;     /* 契约版本号 */
    uint32_t kind;            /* KMOD_SYM_* */
    uint32_t flags;           /* KMOD_EXF_* */
    uint32_t aux;             /* SYSCALL: 槽位号；SEAT: 保留 0；NAMESPACE: 0 */
    uint32_t reserved;
} kmod_export_t;

/* 导入项：.in 段数组元素
 * 语义：把 provider 模块（或 "kernel"）导出的 symbol 地址，
 *       回填到 **slot 指向的模块内指针槽**。
 *       provider/symbol/contract 同时构成依赖拓扑图的一条有向边：
 *       provider -> 当前模块。 */
typedef struct kmod_import {
    const char *provider;     /* "kernel" 或 provider 模块名 */
    const char *symbol;       /* 被导入符号名；DEP_EDGE 时为空串/空指针 */
    const char *contract;     /* 期望的私有契约命名空间（空串 = 不校验） */
    void **slot;              /* 回填地址槽（模块内指针变量的地址） */
    uint32_t abi_min;         /* 契约版本下界（0 = 不校验） */
    uint32_t abi_max;         /* 契约版本上界（0 = 不校验） */
    uint32_t expect_size;     /* 期望 ops 结构尺寸（0 = 不校验） */
    uint32_t flags;           /* KMOD_IMP_* */
} kmod_import_t;

/* 编译期尺寸自检：模块与内核用同一个头，布局不一致必须编不过 */
#define KMOD_ABI_CAT_(a, b) a##b
#define KMOD_ABI_CAT(a, b) KMOD_ABI_CAT_(a, b)
#define KMOD_ABI_STATIC_ASSERT(cond, msg) \
    typedef char KMOD_ABI_CAT(kmod_abi_static_assert_, __LINE__)[(cond) ? 1 : -1]

/* 内核侧辅助：把内联字符串安全拷进定长字段（模块侧不需要） */
#define KMOD_EXPORT_ENTRY(sym_, contract_, addr_, size_, abi_, kind_, flags_, aux_) \
    { sym_, contract_, (void *)(addr_), size_, abi_, kind_, flags_, aux_, 0u }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_ABI_H_ */
