//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_internal.h —— kmod 子系统内部接口（仅内核实现文件使用，非模块 ABI）
 * ============================================================================
 * 文件分工（全部位于 src/arch/x86_64/kmod/，仅 x86_64 构建；宿主测试显式编译）：
 *
 *   platform_kernel.cpp  kmod_platform.h 的内核实现（kmalloc/kprintf/PIT/spinlock）
 *   elf_loader.cpp       ET_REL 加载器：布局/拷贝/重定位/.in/.out/manifest 解析
 *   topology.cpp         纯图算法：依赖拓扑排序/环检测/版本/命名空间校验
 *   capability.cpp       能力策略：Tier0 保留位、STRICT/OPEN 策略、可信清单
 *   governance.cpp       事务治理：镜像池规划、全成功/全回滚、热替换、epoch、卸载
 *   syscall_registry.cpp 固定 syscall 边界：契约表、动态命名空间、治理哨兵/trampoline
 *   seat.cpp             Tier1 座位注册表：随机数座位 + 原子切换 + 回退
 *   kernel_exports.cpp   Tier0 内核导出表 (.kout)：模块可导入的内核符号
 *   selftest.cpp         引导期自检（串口可观测，QEMU 门禁用）
 *   kmod_core.cpp        全局注册表、启动流程、公共 API 转发、观测/打印
 * ==========================================================================*/
#ifndef _KMOD_INTERNAL_H_
#define _KMOD_INTERNAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kmod/kmod.h>
#include <kmod/kmod_platform.h>
#include <kmod/kmod_component.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* 全局注册表（定义在 kmod_core.cpp）                                  */
/* ================================================================== */
extern volatile int32_t g_kmod_lock;
extern kmod_stats_t g_kmod_stats;

/* 镜像池：引导期 .ko 原始 blob 的登记表（只解析清单，不做重定位） */
typedef struct kmod_image {
    char name[KMOD_NAME_MAX];
    char version[KMOD_VERSION_MAX];
    const uint8_t *data;      /* 指向 limine 模块内存或内核自带内嵌镜像 */
    uint64_t size;
    uint32_t tier;
    uint32_t flags;
    uint64_t caps_required;
    uint64_t caps_optional;
    uint32_t init_budget_ms;
    uint32_t quiesce_budget_ms;
    int32_t parse_ret;        /* KMOD_OK 表示清单可解析 */
    bool inspected;
    bool owned;               /* data 是否需要 kmod_free */
} kmod_image_t;

extern kmod_image_t g_kmod_images[KMOD_MAX_IMAGES];
extern uint32_t g_kmod_image_count;

/* 模块运行时节点（拓扑图顶点；在 governance.cpp 中维护） */
typedef struct kmod_elf_image {
    uint8_t *mem;                    /* 已重定位镜像基址（16 对齐） */
    uint64_t size;                   /* 镜像总字节数 */
    kmod_manifest_t *manifest;       /* 指向镜像内的清单副本 */
    kmod_export_t *exports;          /* .out 数组（镜像内） */
    uint32_t n_exports;
    kmod_import_t *imports;          /* .in 数组（镜像内） */
    uint32_t n_imports;
    void (**init_array)(void);       /* SHT_INIT_ARRAY（已重定位；可为 NULL） */
    uint32_t n_init_array;
    void (**fini_array)(void);       /* SHT_FINI_ARRAY（已重定位；可为 NULL） */
    uint32_t n_fini_array;
    uint32_t reloc_count;
    uint32_t section_count;
} kmod_elf_image_t;

/* 一条导入绑定（图边 + 回填结果，热替换重绑定用） */
typedef struct kmod_binding {
    uint32_t import_index;           /* 在 module->imports 中的下标 */
    int32_t provider;                /* provider 模块下标；-1 = kernel */
    void *resolved;                  /* 已回填地址（kernel 导出或 provider .out） */
    uint32_t provider_export_index;  /* provider .out 下标（kernel 时无意义） */
    bool optional;                   /* 缺失时保留 NULL */
    bool dep_edge_only;              /* 纯模块级依赖，无地址回填 */
} kmod_binding_t;

typedef struct kmod_module {
    char name[KMOD_NAME_MAX];
    char version[KMOD_VERSION_MAX];
    uint16_t state;                  /* KMOD_STATE_* */
    uint16_t tier;
    uint32_t flags;                  /* KMOD_MF_* */
    uint32_t internal_flags;         /* KMOD_MODF_*（Tier0 内部，不对外暴露） */
    uint64_t caps_required;
    uint64_t caps_optional;
    uint64_t caps_granted;
    uint64_t caps_denied;
    int32_t pool_index;              /* 镜像池下标；-1 = 内嵌/非池 */
    uint32_t generation;             /* 每次成功热替换 +1 */
    uint64_t load_seq;               /* 全序加载序号（审计/拓扑打印） */
    kmod_elf_image_t img;            /* 当前生效镜像（governance.cpp 拥有） */
    kmod_export_t *exports;          /* = img.exports（便于替换时原子更新） */
    uint32_t n_exports;
    kmod_import_t *imports;          /* = img.imports */
    uint32_t n_imports;
    kmod_binding_t bindings[KMOD_MAX_IMPORTS];
    uint32_t n_bindings;
    uint16_t deps[KMOD_MAX_MODULE_DEPS];          /* 本模块依赖的 provider 下标 */
    uint32_t n_deps;
    uint16_t consumers[KMOD_MAX_MODULE_CONSUMERS];/* 依赖本模块的模块下标 */
    uint32_t n_consumers;
    uint32_t refcount;               /* 外部持有者（消费者导入边，含 dep_edge） */
    volatile uint32_t epoch_active;  /* in-flight 调用 */
    uint64_t epoch_max_seen;         /* 观测峰值 */
    uint64_t gen_exports_hash;       /* 导出表指纹（热替换审计） */
} kmod_module_t;

/* 模块内部 flags：区分内建组件（内核镜像静态链接）与可加载 .ko。
 * 内建组件的 manifest/.in/.out 位于内核只读/静态数据，治理层不得 free 其
 * 镜像或导出数组；热替换成功后节点转为 .ko 背书（清 BUILTIN），回滚恢复。 */
#define KMOD_MODF_BUILTIN   (1u << 0)
#define KMOD_MODF_DEFERRED  (1u << 1)   /* 允许在 boot 图之后单独加载 */

extern kmod_module_t g_kmod_modules[KMOD_MAX_MODULES];
extern uint32_t g_kmod_module_count;

/* ================================================================== */
/* 内核导出表（kernel_exports.cpp，.kout 段）                          */
/* ================================================================== */
#define KMOD_KEXP_NAME_MAX 64
/* 内核导出项的完整元数据（除了名字/地址，还带契约/版本/尺寸/种类，供
 * governance 在 T14 "provider=kernel" 导入绑定时做与模块间导入同等的
 * contract/abi/size 机械校验；loader 的未定义符号解析只用 name/addr）。 */
typedef struct kmod_kernel_export {
    const char *name;
    void *addr;
    const char *contract;     /* 机制契约命名空间（kernel.*） */
    uint32_t abi_version;
    uint32_t size;            /* CONTRACT/DATA 的字节数；FUNC 为 0 */
    uint32_t kind;            /* KMOD_SYM_*（kmod_abi.h） */
} kmod_kernel_export_t;

void KmodKernelExports(const kmod_kernel_export_t **out, uint32_t *count);

/* 宿主测试专用：kernel_services.cpp 的宿主实现把 .kout 等价表暴露给
 * kernel_exports.cpp 的宿主分支合并（内核分支直接扫描 .kout 段，不需要本函数）。 */
void KmodKernelServiceExports(const kmod_export_t **out, uint32_t *count);

/* 内核导出声明宏：放在 kernel_exports.cpp / kernel_services.cpp（Tier0 固定）。
 * name 为模块 .in 中引用的符号名；fn/obj 为内核实际地址。 */
#define KMOD_KEXPORT(sym, contract_, abi_, kind_) \
    __attribute__((used, section(KMOD_SEC_KERNEL_OUT), aligned(8))) \
    kmod_export_t __kmod_kexport_##sym = { \
        #sym, contract_, (void *)&sym, 0u, (uint32_t)(abi_), kind_, 0u, 0u, 0u }

/* 导出 ops 契约（symbol = contract 名，size = sizeof(ops) 供 expect_size 校验） */
#define KMOD_KEXPORT_CONTRACT(contract_, ops_var_, abi_) \
    __attribute__((used, section(KMOD_SEC_KERNEL_OUT), aligned(8))) \
    kmod_export_t __kmod_kexport_contract_##ops_var_ = { \
        contract_, contract_, (void *)&ops_var_, (uint32_t)sizeof(ops_var_), \
        (uint32_t)(abi_), KMOD_SYM_CONTRACT, 0u, 0u, 0u }

/* 导出"别名函数/对象"：name 字符串与内核符号名可不同（如 kprintf_ -> printf_） */
#define KMOD_KEXPORT_AS(export_name_, sym_, contract_, abi_, kind_) \
    __attribute__((used, section(KMOD_SEC_KERNEL_OUT), aligned(8))) \
    kmod_export_t __kmod_kexport_as_##export_name_##_##sym_ = { \
        #export_name_, contract_, (void *)&sym_, 0u, (uint32_t)(abi_), kind_, 0u, 0u, 0u }

/* 导出数据对象（size 参与 expect_size 校验） */
#define KMOD_KEXPORT_DATA(export_name_, sym_, contract_, abi_, size_) \
    __attribute__((used, section(KMOD_SEC_KERNEL_OUT), aligned(8))) \
    kmod_export_t __kmod_kexport_data_##export_name_ = { \
        #export_name_, contract_, (void *)&sym_, (uint32_t)(size_), \
        (uint32_t)(abi_), KMOD_SYM_DATA, 0u, 0u, 0u }

/* ================================================================== */
/* ELF 加载器（elf_loader.cpp）                                        */
/* ================================================================== */
int32_t KmodElfInspect(const uint8_t *data, uint64_t size,
                       kmod_manifest_t *out_manifest, char *err, uint64_t errcap);
int32_t KmodElfLoadImage(const uint8_t *data, uint64_t size,
                         const kmod_kernel_export_t *kexp, uint32_t n_kexp,
                         kmod_elf_image_t *out, char *err, uint64_t errcap);
void KmodElfFreeImage(kmod_elf_image_t *img);
int32_t KmodElfValidateManifest(const kmod_manifest_t *m, char *err, uint64_t errcap);
/* 内建组件清单校验（允许 kernel./core. 保留前缀；其余与 .ko 同规则） */
int32_t KmodElfValidateBuiltinManifest(const kmod_manifest_t *m, char *err, uint64_t errcap);
const char *KmodElfErrorString(int32_t rc);
const char *KmodStateName(uint16_t state);

/* ================================================================== */
/* 拓扑 / 版本 / 命名（topology.cpp，纯函数，可宿主测试）              */
/* ================================================================== */
typedef struct kmod_topo_node {
    const char *name;
    const char *const *deps;    /* 依赖的 provider 名字（含 "kernel"） */
    uint32_t n_deps;
} kmod_topo_node_t;

/* Kahn 拓扑排序。成功：out_order[0..min(n,cap)-1] 为依赖先序（provider 在前），
 * 返回写入条数。失败返回 KMOD_ERR_CYCLE，cycle_path 写入 "a->b->a" 形式路径。 */
int32_t KmodTopoSort(const kmod_topo_node_t *nodes, uint32_t n_nodes,
                     uint32_t *out_order, uint32_t out_cap,
                     char *cycle_path, uint64_t cycle_cap);

/* "x.y.z" 语义版本（仅数字，缺省段按 0）。返回 KMOD_OK / KMOD_ERR_VERSION */
int32_t KmodVersionParse(const char *s, uint32_t *maj, uint32_t *min, uint32_t *pat);
int32_t KmodVersionCompare(const char *a, const char *b);   /* <0 / 0 / >0 */
/* 导入的 abi 区间校验：vmin<=vmax<=0(无上界) 且 vmin<=ver<=vmax（ver 为契约 abi） */
int32_t KmodVersionInRange(uint32_t ver, uint32_t vmin, uint32_t vmax);

/* 命名空间校验：
 *  - 模块名:  [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+  长度 <= KMOD_NAME_MAX-1
 *  - 契约名:  同模块名规则，额外允许 "kernel." 前缀（仅 Tier0 内核镜像）
 *  - 版本号:  "x.y.z"，每段 <= 65535 */
int32_t KmodNameValidate(const char *name);
/* kernel./core. 保留前缀（Tier0 内建组件/内核镜像可用；.ko 加载路径必须拒绝） */
bool KmodNameIsReserved(const char *name);
/* 内建组件名校验：允许 kernel./core. 前缀（仅供 Tier0 builtin 注册路径） */
int32_t KmodBuiltinNameValidate(const char *name);
int32_t KmodContractValidate(const char *contract);
int32_t KmodSegNameValidate(const char *seg, uint32_t max_len);   /* 段名合法性 */

/* ================================================================== */
/* 能力策略（capability.cpp，可宿主测试）                              */
/* ================================================================== */
#define KMOD_POLICY_STRICT   0u   /* 默认：仅可信清单内模块，能力按清单授予 */
#define KMOD_POLICY_OPEN     1u   /* 诊断：未知模块可加载，能力按声明授予（危险） */
#define KMOD_POLICY_DISABLED 2u   /* 关闭模块加载（Tier0 仍在线） */

typedef struct kmod_trusted_entry {
    const char *name;
    uint64_t caps;
} kmod_trusted_entry_t;

void KmodCapInit(void);
void KmodCapSetMode(uint32_t mode);
uint32_t KmodCapMode(void);
/* 能力授予：required 全授予才成功（否则 KMOD_ERR_CAP）；optional 能授则授。
 * 任何 KMOD_CAP_RESERVED_CORE / 未定义位出现在 required 或 optional 中一律拒绝。 */
int32_t KmodCapGrant(const char *module_name, uint64_t required, uint64_t optional,
                     uint64_t *granted_out, uint64_t *denied_out);
bool KmodCapIsReserved(uint64_t caps);
const char *KmodCapName(uint64_t bit);
int32_t KmodCapTrustAdd(const char *name, uint64_t mask);   /* 测试/扩展钩子 */
uint32_t KmodCapTrustedCount(void);
const kmod_trusted_entry_t *KmodCapTrustedAt(uint32_t index);

/* ================================================================== */
/* 座位注册表（seat.cpp）                                              */
/* ================================================================== */
typedef struct kmod_seat {
    char name[KMOD_CONTRACT_MAX];
    uint32_t abi_version;
    uint32_t ops_size;               /* 内核期望的 ops 结构尺寸（硬校验） */
    uint64_t allowed_caps;           /* 提供者必须被授予的能力（至少其一） */
    const void *kernel_ops;          /* Tier0 默认实现（可为 NULL） */
    int32_t provider;                /* 模块下标；-1 = 内核默认 */
    const void *module_ops;          /* 模块提供的 ops（原子读） */
    uint32_t generation;
    volatile int32_t lock;
} kmod_seat_t;

int32_t KmodSeatInit(void);
int32_t KmodSeatRegisterKernel(const char *name, uint32_t abi_version,
                               const void *ops, uint32_t ops_size,
                               uint64_t allowed_caps);
kmod_seat_t *KmodSeatFind(const char *name);
/* 提交阶段：扫描模块 exports 中的 KMOD_SYM_SEAT，校验并安装；失败返回错误码
 * 时已安装的部分必须回滚（由 governance 事务统一回滚）。 */
int32_t KmodSeatAttachModule(kmod_module_t *m);
void KmodSeatDetachModule(kmod_module_t *m);
const void *KmodSeatCurrent(const char *name);   /* 原子读取：模块提供者优先，否则内核默认 */
const kmod_seat_t *KmodSeatAt(uint32_t index);
uint32_t KmodSeatCount(void);
int32_t KmodSeatRollbackToKernel(const char *name); /* 热替换失败/卸载回退 */

/* ================================================================== */
/* syscall 治理（syscall_registry.cpp）                                */
/* ================================================================== */
#define KMOD_SYSCALL_CONTRACT_ENTRIES 64u
typedef struct kmod_syscall_contract {
    uint16_t nr;
    uint16_t flags;                  /* KMOD_SC_* */
    uint32_t abi_version;
    const char *name;                /* 固定契约名，如 "fopen" */
    const char *owner;               /* "kernel" 或动态槽命名空间 */
} kmod_syscall_contract_t;

typedef struct kmod_syscall_slot {
    uint16_t nr;
    uint16_t flags;
    int32_t module;                  /* 实现模块下标；-1 = 内核内建实现 */
    kmod_syscall_fn fn;              /* 当前实现（原子读） */
    uint32_t generation;
    volatile int32_t lock;
} kmod_syscall_slot_t;

int32_t KmodSyscallInit(void);
const kmod_syscall_contract_t *KmodSyscallContract(uint32_t nr);
uint32_t KmodSyscallContractCount(void);
/* 提交阶段：扫描 KMOD_SYM_SYSCALL / KMOD_SYM_NAMESPACE 导出，校验槽位/能力，
 * 安装到治理表；失败时已安装部分回滚。 */
int32_t KmodSyscallRegisterModule(kmod_module_t *m);
void KmodSyscallUnregisterModule(kmod_module_t *m);
/* 热替换用：模块导出表切换后重新发布 syscall 实现 */
int32_t KmodSyscallRepublishModule(kmod_module_t *m);
/* 固定 dispatch 钩子实现（syscall.cpp 调用） */
uint64_t KmodSyscallEntry(void *frame);
void *KmodSyscallSentinel(void);
const kmod_syscall_slot_t *KmodSyscallSlot(uint32_t nr);
/* SYSCALL_KMOD_INFO/PROBE 的 Tier0 实现与探针值 */
int32_t KmodSyscallCopyInfoToUser(void *ubuf, uint64_t usize, uint64_t flags);
uint64_t KmodSyscallProbeValue(void);
uint32_t KmodSyscallGovernedCount(void);

/* ================================================================== */
/* 内建组件管理器（builtin.cpp，Tier0 机制：整个内核组件化的入口）    */
/* ================================================================== */
#define KMOD_MAX_BUILTINS 64u
int32_t KmodBuiltinInit(void);
uint32_t KmodBuiltinCount(void);
const kmod_builtin_raw_t *KmodBuiltinAt(uint32_t index);
const kmod_builtin_raw_t *KmodBuiltinFind(const char *name);
/* 按内建组件拓扑图统一 bind/apply/init；启动序列的组件化入口。
 * 失败时本次已提交的内建组件逆序回滚，返回负错误码。 */
int32_t KmodBuiltinBootLoad(void);
/* 单独加载一个内建组件（含其内建依赖），用于延迟加载/测试。 */
int32_t KmodBuiltinLoad(const char *name);

/* governance.cpp 实现：把内建组件描述符落成模块节点（不 bind/apply/init，
 * 不做 pool/relocation；caps 视为内建信任、不做 STRICT 白名单校验）。 */
int32_t KmodModuleAllocBuiltin(const kmod_builtin_raw_t *raw, int32_t *index_out);
/* 内建组件条目解析（begin/end 含哨兵，内部按条目尺寸步进并校验魔数） */
uint32_t KmodBuiltinImportCount(const kmod_builtin_raw_t *raw);
uint32_t KmodBuiltinExportCount(const kmod_builtin_raw_t *raw);
const kmod_import_t *KmodBuiltinImports(const kmod_builtin_raw_t *raw);
const kmod_export_t *KmodBuiltinExports(const kmod_builtin_raw_t *raw);

/* ================================================================== */
/* 事务治理（governance.cpp）                                          */
/* ================================================================== */
int32_t KmodGovInit(void);

/* 镜像池操作（core 提供存储，gov 提供查找/选择语义） */
int32_t KmodPoolFind(const char *name, const char *version);   /* 返回下标/-ERR */
int32_t KmodPoolAdd(const uint8_t *data, uint64_t size, bool owned); /* 返回下标/-ERR */

/* 事务化加载：自动拉起依赖（拓扑先序）；任一环节失败 → 本次事务新加载的模块
 * 全部回滚（逆序），已有模块不受影响。version=NULL/"" 表示池内最高版本。
 * 返回 KMOD_OK 或负错误码。 */
int32_t KmodGovLoad(const char *name, const char *version, uint32_t flags);
/* 事务化卸载：refcount 必须为 0；quiesce/drain/exit 后释放；有消费者则 EBUSY。 */
int32_t KmodGovUnload(const char *name, uint32_t flags);
/* 事务化热替换：prepare 新版本镜像 → quiesce+drain 提供者与消费者 →
 * 原子切换导出表并重绑定 → 新 init → 成功则旧 exit/释放；失败全量回退。 */
int32_t KmodGovHotReplace(const char *name, const char *new_version, uint32_t flags);

/* 运行时模块表辅助（供 selftest/观测使用） */
kmod_module_t *KmodModuleFind(const char *name);
int32_t KmodModuleIndexOf(const char *name);
int32_t KmodModuleAllocFromImage(int32_t pool_index, uint32_t flags, int32_t *index_out);
void KmodModuleRelease(kmod_module_t *m);
/* 校验清单是否允许加载（Tier/命名/能力/策略），返回 KMOD_OK 或负错误码 */
int32_t KmodCheckLoadable(const kmod_manifest_t *m, uint32_t flags);
/* 提交阶段的图/绑定/导出/生命周期原语（governance 内部使用，selftest 不直接调） */
int32_t KmodBindImports(kmod_module_t *m);
void KmodUnbindAll(kmod_module_t *m);
int32_t KmodApplyExports(kmod_module_t *m);
void KmodRevertExports(kmod_module_t *m);
int32_t KmodCallInit(kmod_module_t *m);
void KmodCallExit(kmod_module_t *m);
int32_t KmodQuiesceAndDrain(kmod_module_t *m, uint32_t budget_ms);
int32_t KmodRebindConsumers(kmod_module_t *provider, const kmod_export_t *old_exports,
                            uint32_t n_old, const kmod_export_t *new_exports, uint32_t n_new);
uint64_t KmodExportsHash(const kmod_export_t *exports, uint32_t n);

/* epoch 内部实现（governance.cpp 导出；KmodEpochEnter/Exit 公共 API 包装它） */
int32_t KmodEpochEnterInternal(const char *name);
void KmodEpochExitInternal(const char *name);

/* 默认时间预算 */
#define KMOD_DEFAULT_INIT_BUDGET_MS     500u
#define KMOD_DEFAULT_QUIESCE_BUDGET_MS  200u

#ifdef __cplusplus
}
#endif

#endif /* _KMOD_INTERNAL_H_ */
