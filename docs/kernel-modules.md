# 内核模块机制：私有契约型可加载组件链接与治理 (kmod)

> 一句话：**统一边界，放开实现；管住回滚，放开私有契约。**
>
> 本文档描述 Tier 0 固定机制（加载器/拓扑/能力/事务/回滚/syscall 边界/座位），以及
> `.ko` 与**内建组件**共用的线格式。整个内核组件的迁移清单与 Tier 划分见
> [kernel-components.md](kernel-components.md)。

---

## 1. 总体定位

这不是微内核，也不是"所有东西都可任意热替换"的全模块内核，而是一套：

> 运行在内核态、面向宏内核增强的**私有契约型可加载模块链接与治理机制**。

- 系统调用对外契约固定；
- 内核内部组件实现可插拔；
- 组件之间私有接口自治（不强制全局统一接口）；
- 内核只负责依赖加载、能力治理、回填安全、失败回滚；
- 在低隔离的内核态环境下，用治理层补足开放生态的可控性。

## 2. 分层与 Tier

| 层 | 内容 | 可否热替换 | 代码位置 |
|---|---|---|---|
| **Tier 0 核心** | 启动、页表、中断、上下文切换、syscall 入口、模块加载器、回滚机制、能力注册表、组件句柄/生命周期 | ❌ 固定 | `kernel/src/arch/x86_64/kmod/`、arch 核心 |
| **Tier 1 内建组件** | 静态链接进内核镜像、但走同一 ABI/图/治理的组件：syscall 实现家族、驱动、文件系统、协议栈、调度策略、分配器策略 | ✅ 可被 `.ko` 热替换，回滚恢复内建实现 | `kernel/src/components/`(规划)、`kernel/src/arch/x86_64/kmod/builtin.cpp` |
| **Tier 2 可加载 `.ko`** | 外部可加载实现/扩展/诊断 | ✅ 加载/热替换/卸载（受治理） | `kernel/modules/`、任意外部 `.ko` |

Tier 0 与 Tier 1 的边界是**机制 vs 策略**：

- 调度器核心（队列/上下文切换/时钟）固定，调度**策略**可由座位替换；
- VFS 契约固定，文件系统**实现**是组件；
- syscall 入口/编号/语义固定，**实现**可注册；
- PCI 枚举/中断分发固定，**驱动**是组件；
- 分配页表/slab 机制固定，**分配策略/回收策略**可由座位替换。

## 3. 线格式（模块 ABI v1）

头文件：`kernel/include/kmod/kmod_abi.h`（唯一边界）、`kmod_module.h`（`.ko` 作者宏）、
`kmod_component.h`（内建组件宏）。

### 3.1 段布局（`.ko`，ET_REL/ELF64/x86_64）

| ELF 段 | 内容 | 说明 |
|---|---|---|
| `.in` | `kmod_import_t[]` | 导入声明：`{provider 组件名, symbol, contract 私有契约名, slot 回填地址, abi_min, abi_max, expect_size, flags}` |
| `.out` | `kmod_export_t[]` | 导出：函数/数据/私有契约 ops/座位/syscall 实现 |
| `.kmod_manifest` | `kmod_manifest_t`（256B） | 名称/版本/Tier/flags/能力/生命周期回调/预算 |

`kmod_import_t.slot` 指向模块内指针变量；加载器把 provider 导出的地址写进去，模块代码
经该槽调用。`provider="kernel"` 表示 Tier 0 内核导出（`.kout` 表）。

### 3.2 manifest（节选）

```c
typedef struct kmod_manifest {
    uint32_t magic;  uint16_t abi_version;  uint16_t tier;  uint32_t flags;
    char name[48];            /* 命名空间名，如 vendor.demo.hello；kernel.* 保留给内建 */
    char version[24];         /* 语义版本 x.y.z */
    char description[80];
    uint64_t required_caps, optional_caps;
    int32_t (*init)(void);  void (*exit_hook)(void);
    void (*quiesce)(void);  int32_t (*drain)(void);
    uint32_t init_budget_ms, quiesce_budget_ms;   /* 看门狗预算 */
    ...
} kmod_manifest_t;
```

### 3.3 加载器支持的重定位（x86_64）

`R_X86_64_NONE/64/PC32/PLT32/32/32S`。`.ko` 用 `-fno-pic -fno-pie -mcmodel=large`
编译（`-mcmodel=kernel` 会生成 32 位绝对地址，模块在 HHDM 高地址会溢出）。未知类型
整体拒绝（`KMOD_ERR_RELOC`），绝不把未重定位的槽放进内核。

### 3.4 内建组件（builtin）线格式

同一套 `kmod_manifest_t/kmod_import_t/kmod_export_t`，由 `KCOMPONENT_BEGIN/END` 宏放入
`.kbuiltin.in.<ID>` / `.kbuiltin.out.<ID>` / `.kbuiltin.desc` 段；链接后
`builtin.cpp` 扫描描述符表，按拓扑图执行 bind/apply/init。内建组件是 Tier 0 构建产物，
能力策略视为可信（不做 STRICT 白名单），但同样进入拓扑图、引用计数、epoch、热替换。

## 4. 私有契约（模块间自治）

模块之间自行约定函数、槽位、布局、版本；内核只做**机械校验**（不解释语义）：

- 契约名命名空间：`<vendor>.<area>.<vN>`，如 `vendor.sched.v1`；`kernel.*`/`core.*` 保留；
- 导出 `kind=KMOD_SYM_CONTRACT` 声明 ops 结构体地址 + `size` + `abi_version`；
- 导入方用 `KMOD_IMPORT_EX(..., abi_min, abi_max, expect_size)` 给出可接受范围；
- 加载期校验：契约字符串相等、版本落在区间、`provider.size == expect_size`（结构布局漂移
  在加载期暴露，而不是运行时踩坏内存）；失败则整个加载事务回滚。

## 4.1 Tier0 共享服务：klib 等公共工具共享给每个模块

Tier0 的 klib 公共工具以**两种方式**共享给所有模块（内建组件与可加载 `.ko` 一致）：

1. **直接符号共享（无需 `.in`）**：klib 中 C 链接函数以真实 ELF 符号名进入内核
   `.kout` 导出表；模块直接声明/调用即可，加载器按未定义符号名解析到内核地址：

   ```c
   extern "C" size_t strlen(const char *);          /* 直接调用 */
   extern "C" int32_t snprintf_(char *, size_t, const char *, ...);
   ```
   覆盖：`strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr/strtok/atoi`、
   `printf_/sprintf_/snprintf_/vsnprintf_/vprintf_/fctprintf`、
   `hashmap_*`、`art_*`、`spinlock_lock/unlock`、`kcalloc/krealloc/kmalloc_aligned`
   （`kernel.mem.v1` 追加符号；既有 kmalloc/kfree 不变）。

2. **契约 ops 共享（可批量、含 C++ 链接工具）**：模块用
   `KMOD_IMPORT_KLIB/SYNC/ALGO` 导入版本化结构体，治理层做
   contract 相等 + abi 区间 + `expect_size == sizeof(ops)` 强校验：

   | 契约 | 内容 |
   |---|---|
   | `kernel.klib.v1` | 字符串/`sprintf_`/`snprintf_`/qsort/bitmap/kld16-32-64/随机/panic |
   | `kernel.sync.v1` | `spinlock_lock/unlock`、`irq_save/irq_restore/irq_state` |
   | `kernel.algo.v1` | hashmap 子集、ART、Queue/FIFO/List（不透明句柄，布局内核私有） |

   ```c
   #include <kmod/kmod_module.h>
   KMOD_IMPORT_KLIB(klib);
   KMOD_IMPORT_SYNC(sync);
   static int32_t init(void) {
       uint8_t bm[4] = {0};
       klib->bitmap_set(bm, 3);
       uint64_t f = sync->irq_save();
       sync->irq_restore(f);
       return KMOD_OK;
   }
   ```

**ABI 规则**：ops 结构只允许末尾追加字段；追加必须升 ABI 版本并保留旧版本导出
（`kernel.klib.v2` ...），模块用 `abi_min/abi_max` + 精确 `expect_size` 锁定布局。
**明确不共享**：转置序旧 API `_memcpy/_memset/_memmove/_memcmp`、静态缓冲
`to_string/ConvertHexToString`（非线程安全）、header-only 模板（rbtree 等由模块自行
编译头文件）、渲染器/随机后端内部实现。内建组件因同镜像静态链接可直接调用 klib；
它们同样可以使用上述导入宏/契约（不强制迁移）。

## 5. 能力（capability）与信任模型

能力位（`KMOD_CAP_*`）覆盖：内存分配/DMA/日志/IRQ/MMIO/设备/块设备/FS/网络/调度策略/
分配策略/syscall 实现/syscall 覆写/座位提供者/热替换/加载/卸载/诊断。

- 策略模式：`STRICT`（默认，仅内置可信清单 + 能力=清单∩声明）、`OPEN`（诊断，未知模块
  按声明授予）、`DISABLED`（拒绝一切加载）；
- `KMOD_CAP_RESERVED_CORE` 与未定义高位：任何 `.ko` 声明即拒绝；
- **诚实声明**：这是低隔离内核态，能力位是治理/审计边界，**不是内存隔离沙箱**。
  模块代码与内核同地址空间；治理层通过"最小导出面 + 加载期机械校验 + 事务回滚 +
  引用计数 + 看门狗"降低风险，但无法阻止恶意模块越界写内核内存（与 Linux 内核模块同级问题）。

## 6. syscall 边界（固定入口，实现可注册）

- 编号 0..63 为固定契约槽（`syscalln.h` + `syscall_registry.cpp` 契约表）：参数/返回/错误码/语义固定；
- 固定槽分三类：`FIXED`（不可替换实现，如文件 IO）、`FIXED|SWAPPABLE`（语义固定但实现允许
  受治理热替换，如诊断探针 30）、`FIXED|KMOD`（kmod 自省 29）；
- 128..511 为动态命名空间槽：`CAP_SYSCALL_IMPL` 模块注册，未绑定返回 `-ENOSYS`，重复槽拒绝；
- 覆写固定槽实现需要 `CAP_SYSCALL_OVERRIDE` 且槽位带 `SWAPPABLE`；
- Tier 0 dispatch 只看 `syscall_lists[slot]`：治理槽统一指向**哨兵**，由
  `KmodSyscallEntry(frame)` 查治理表、做 epoch 保护、返回 `-EAGAIN`（模块静默中）或
  `-ENOSYS`（无实现）。入口/编号/语义永不因模块而变。

## 7. 座位（seat）：开放策略，不开放核心机制

座位是 Tier 0 固定机制的"策略插槽"，内核定义 ops 结构（ABI 尺寸硬校验），模块实现：

```c
/* kernel.seat.random.v1 —— 随机数来源；sys_getrandom 消费此座位 */
typedef struct kmod_seat_random_ops {
    uint32_t abi_version; uint32_t ops_size;
    uint64_t (*u64)(void);
    void (*fill)(uint8_t *buf, uint64_t len);
} kmod_seat_random_ops_t;
```

模块 `KEXPORT_SEAT(KMOD_SEAT_RANDOM, ops, 1)` 提供实现；内核调用点
`KmodSeatCurrent(name)` 原子读取"模块实现优先，否则 Tier 0 默认"，热替换/卸载由治理事务
保障 epoch 排空与失败回退。**已落地座位**：`kernel.seat.random.v1`（随机源）、
`kernel.seat.sched.policy.v1`（调度策略：base/dynamic/eligibility quantum 覆盖）、
`kernel.seat.alloc.policy.v1`（分配策略：large-alloc 阈值 / OOM 决策，仅冷路径）。

## 8. 拓扑图与依赖加载

- 图顶点 = 组件（内建/Tier0 kernel 节点/`.ko`）；边 = `.in` 中的 `provider` 声明（模块级依赖）；
- 规划：从目标镜像开始，按 provider 名递归求依赖闭包（镜像池允许多版本，选最高满足版本）；
- `KmodTopoSort`（Kahn）给出 provider 先序；环 -> `KMOD_ERR_CYCLE` + 打印 `a->b->a` 路径，
  不产生任何半加载残留；
- 运行时同一张图维护 `deps[]/consumers[]`，用于引用计数、卸载校验与热替换重绑定；
- 未声明 `.in` 的隐式依赖不允许：模块代码只能通过导入槽/内核导出表引用外部符号。

## 9. 事务化加载（全成功或全回滚）

```
plan(镜像池闭包+拓扑序) -> prepare(每个镜像: 布局/拷贝/重定位/清单与契约校验)
  -> commit(逐组件: 分配节点 -> bind 导入槽/建图边 -> 注册导出/座位/syscall
            -> 运行 init_array -> 调 manifest->init -> READY)
  -> 任一步失败: 逆序回滚(exit/fini -> 撤销导出注册 -> 解绑边 -> 释放镜像/节点)
```

约束：init/exit/quiesce/drain 回调**不得在持治理锁时调用**（避免模块内再入公共 API 死锁）；
init 有看门狗预算（默认 500ms），超预算即失败回滚（`stats.budget_exceeded`）。

## 10. 热替换（quiesce / 状态迁移 / 原子切换 / 看门狗 / 失败回退）

1. prepare：加载新版镜像并校验（旧实现保持在线）；
2. `state=REPLACING`：调用 quiesce 回调（停止接收新工作），等待 `epoch_active==0` 或
   `drain()` 返回 0，预算是 `quiesce_budget_ms`（默认 200ms）；
3. 原子切换：导出表指针/版本/代次一次性替换；重绑定所有消费者的导入槽（旧地址 -> 新地址）；
   重新发布座位/syscall 实现；
4. 新实现 init（含 init_array）；成功 -> 旧实现 exit/fini 后释放，`generation++`，`READY`；
5. 任一步失败 -> 恢复旧导出表/槽位/座位/syscall 注册，`READY`，`stats.rollbacks++`；
6. 消费者若缓存了函数指针必须用 `KmodEpochEnter(provider)/Exit` 包住使用区间，否则替换期间
   的旧指针可能指向已卸载镜像（治理层以 epoch + 看门狗兜底；这是协作式 quiesce，不是抢占式）。

内建组件被 `.ko` 热替换时，旧实现是内核静态数据：成功替换后模块节点转为 `.ko` 背书；
失败/卸载回滚恢复内建实现（静态内存永不释放）。

## 11. 卸载与引用计数

- 硬门禁：`refcount==0`（无消费者导入边）且 `manifest.flags & KMOD_MF_UNLOADABLE`；
- 有消费者 -> `KMOD_ERR_BUSY`；未声明可卸载 -> `KMOD_ERR_POLICY`；Tier 0 内建核心永不卸载；
- 卸载流程：quiesce + epoch 排空 -> 撤销座位/syscall 导出 -> exit/fini -> 解绑/释放；
- 正在执行的 syscall/座位调用通过 epoch 排空；看门狗超时 -> 放弃卸载并恢复 READY。

## 12. 观测面

- 串口日志统一前缀 `[kmod]`（池/拓扑/事务/回滚/热替换/座位/syscall 逐条可审计）；
- `KmodDump()`：模块表（state/tier/caps/refcount/epoch/deps/consumers）、图边、统计计数；
- syscall 29 `SYSCALL_KMOD_INFO`：用户态只读快照（`kmod_sysinfo_t` + records）；
- 统计：loads/unloads/hotswaps/rollbacks/reject_cap/cycle/version/format/reloc/name/policy/
  busy/seat_swaps/syscall_binds/syscall_overrides/epoch_waits/budget_exceeded/selftest 通过数。

## 13. 构建与使用

```bash
# 内核模块（.ko）
make -C kernel modules          # -> kernel/bin-x86_64/modules/*.ko
make -C kernel modules-check    # readelf 契约自检
# ISO 构建会把 .ko 拷到 boot():/boot/modules/，res/limine.conf 逐项 module_path 声明
# 内核启动: KmodInit -> KmodBootComponents(内建图) -> KmodBootIngest -> KmodBootLoad(BIOS/BOOT flag)
```

`.ko` 作者最小示例：

```c
#include <kmod/kmod_module.h>
KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_", "kernel.log.v1");
KMOD_IMPORT_KLIB(klib);   /* 或直接用 extern "C" 调用 strlen/snprintf_ 等直接符号 */
static int32_t my_init(void)
{
    char buf[32];
    klib->snprintf_(buf, sizeof(buf), "strlen=%u", (unsigned)klib->strlen("x"));
    kprintf_("hello .ko: %s\n", buf);
    return KMOD_OK;
}
static void my_exit(void) {}
KMOD_EXPORT_FN(my_fn, "vendor.me.v1", 1);
KMOD_MANIFEST("vendor.demo.me", "1.0.0", KMOD_TIER_IMPL, KMOD_MF_BOOT|KMOD_MF_UNLOADABLE,
              KMOD_CAP_LOG, my_init, my_exit);
```

## 14. 测试与验证

| 层 | 内容 |
|---|---|
| 宿主测试 `tests/kmod` | 加载器正/负向 + 2000 次随机 fuzz(ASAN)、拓扑 DAG/环/版本/命名、能力策略、syscall/座位、治理事务（见 `tests/kmod/`） |
| 内核自检 | 启动期（**sti 之前单线程**）跑 `KMOD_SELFTEST` 共 12 个用例：引导拓扑/引用计数/依赖自动拉起/环拒绝/能力拒绝/init 失败回滚/随机座位热替换与失败回退/syscall 命名空间与覆写/卸载清理/调度策略座位/分配策略座位/**Tier0 共享服务（klib/sync 直接符号 + 契约 ops）**；串口输出 `KMOD_SELFTEST PASS 12/12` |
| QEMU 无头门禁 | `res/scripts/test/kmod_smoke.ps1`：启动 ISO + serial 日志，断言 PASS 行与无异常 |
| 构建门禁 | `make -C kernel modules-check` + 内核构建零错误 + 宿主 suite 全绿 |

## 15. 已知限制（如实声明）

1. **无内存隔离**：模块与内核同地址空间；HHDM 直接映射为 RW 且未置 NX，模块镜像可执行。
   治理层降低误用风险，不构成安全沙箱（与 Linux 模块同一信任级）。W^X 因 HHDM 别名暂不可强制。
2. quiesce 是**协作式**的：模块必须按契约在消费导入槽时进入 epoch；恶意/错误模块可绕过。
3. `.ko` 只支持 x86_64 ET_REL 与有限重定位集；其他架构需补对应 `arch/<arch>/kmod/` 重定位后端。
4. `.ko` 不得调用未声明的外部符号；编译器生成的 `mem*` 引用由 `kernel.libc.v1` 兜底。
5. 能力策略表内置在 Tier 0 代码中（可信清单），尚无签名/加密校验链；生产化需要镜像签名绑定。
6. 热替换的"状态迁移"由模块 `quiesce/drain` 自行实现；内核只提供时序与回退，不迁移业务状态。
7. 既有并发风险（pre-existing，非 kmod 引入）：`file_cache_tick` 在 PIT IRQ 上下文持文件
   缓存锁做较重工作，与 VMM/heap 锁存在跨核 AB-BA 窗口（高负载 QEMU 下概率性 STALL
   WATCHDOG）。kmod 的缓解是把自检移到 sti 前单线程执行；根治方向见 kernel-components.md §11。
8. 异常处理路径若在已持 kprintf `ptf_lock` 时再次打印会自死锁（pre-existing）；本次通过
   IRQ 顺序 + 引导期 `cli` 规避，生产化建议 panic 输出走无锁串口旁路。
