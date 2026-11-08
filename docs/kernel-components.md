# 内核组件化重构（kernel components）

> 目标：**整个内核的组件**都进入同一套私有契约型链接与治理机制，而不只是做一个
> 外部 `.ko` 加载器 + 演示模块。
>
> 机制细节见 [kernel-modules.md](kernel-modules.md)。本文档是组件清单、Tier 划分、
> 启动序列映射、迁移状态与验收标准的**唯一台账**（每完成一项更新此表）。

---

## 1. 组件模型

- **Tier 0 核心**：不可模块化的机制（引导、页表、中断、上下文切换、syscall 入口、
  模块加载器、回滚、能力注册表、组件句柄/生命周期）。
- **Tier 1 内建组件 (builtin)**：静态链接进内核镜像，但与 `.ko` 使用**完全相同**的
  `kmod_manifest_t` / `.in` / `.out` 线格式，由 `KCOMPONENT_BEGIN/END` 宏放入
  `.kbuiltin.*` 段；启动时组件管理器 `KmodBootComponents()` 扫描描述符、按统一拓扑图
  bind/apply/init。内建组件在机制上支持 `.ko` 接管（失败回滚恢复内建实现）；但当前
  `.ko` 名称校验禁止 `kernel./core.` 保留前缀，真实 37 个 `kernel.*` 组件尚不能被外部
  `.ko` 同名替换（见 §9.6）；已落地的可替换层是座位策略与固定 syscall 实现。
- **Tier 2 可加载 `.ko`**：外部实现/扩展/诊断，加载/热替换/卸载受治理。

组件 = manifest（身份/Tier/能力/生命周期/预算）+ imports（模块级依赖+私有契约）+ exports
（函数/数据/私有契约 ops/座位/syscall 实现）。内核不统一业务接口：组件间契约由组件自己
约定命名空间与结构布局，内核只做机械校验（字符串/版本/尺寸）。

## 2. Tier 划分原则

| 判据 | 划入 Tier 0（固定机制） | 划入 Tier 1+（可替换实现/策略） |
|---|---|---|
| 引导 | 早期串口、GDT/IDT、FPU/SIMD、PMM/VMM、SLAB/早期堆、栈与上下文切换、模块加载器自身 | ACPI/MADT、LAPIC/IOAPIC、PIT/HPET/RTC、SMP 上线、PCI 枚举、设备探测、根文件系统挂载、用户态进程创建 |
| 中断/异常 | 入口/分发/上下文保存 | 具体设备 IRQ handler（驱动组件） |
| syscall | 编号/参数/返回/错误码/语义/入口/治理表 | 各家族实现（proc/fs/mem/dev/time/net/random/sysinfo）、可 SWAPPABLE 槽实现 |
| 调度 | 队列/上下文切换/时钟/记账核心 | 选线程策略、quanta/反馈策略（座位） |
| 内存 | PMM/VMM/页表/slab 机制 | 分配/回收策略、SLUB 融合策略（座位，规划） |
| 存储 | VFS 契约/路径解析/页缓存机制 | FAT/ext4/lwext4 实现、块设备驱动、缓存淘汰策略 |
| 网络 | 设备抽象/收发路径机制（规划中） | lwIP 协议栈、网卡驱动 |
| 输入/显示 | 输入事件分发/帧缓冲内存映射机制 | PS2/USB HID 驱动、显示设备驱动 |
| 加解密/随机 | syscall 入口 | 随机源策略座位（`kernel.seat.random.v1`） |

## 3. 内建组件线格式与启动

```c
/* kernel/modules/builtin/x86_64/components/xxx.cpp —— 一个 TU 一个组件 */
#include <kmod/kmod_component.h>
KCOMPONENT_BEGIN(acpi, "kernel.arch.acpi", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, KMOD_CAP_MMIO, 0, "ACPI/MADT 表解析",
                 comp_acpi_init, comp_acpi_exit, 0, 0, 1000, 200)
KCOMPONENT_IMPORT_FN(acpi, kprintf_, int, (const char *, ...),
                     "kernel", "kprintf_", "kernel.log.v1")
KCOMPONENT_EXPORT_CONTRACT(acpi, "kernel.acpi.v1", acpi_ops, 1)
KCOMPONENT_END(acpi)
```

- 链接脚本把 `.kbuiltin.in.<ID>`/`.kbuiltin.out.<ID>`/`.kbuiltin.desc` 保留在 `.data`；
- `KmodBootComponents()`：扫描描述符 → provider 依赖边 → `KmodTopoSort` → 逐组件
  `KmodModuleAllocBuiltin → KmodBindImports → KmodApplyExports → KmodCallInit → READY`；
- 失败逆序回滚；环/缺失依赖/能力问题在启动日志中以 `[kmod]` 审计可见；
- 之后 `KmodBootLoad()` 加载 `KMOD_MF_BOOT` 的 `.ko`，它们可以导入内建组件的私有契约。
  内建组件节点级接管（`KmodHotReplace` 的 builtin→.ko takeover）代码路径已实现并有
  失败回滚测试；但 `.ko` 不得使用 `kernel./core.` 保留名，故真实 `kernel.*` 组件当前
  不可被外部 `.ko` 同名替换（见 §9.6）。

## 4. 启动序列组件化映射（x86_64）

旧 `x86_64_init` 的硬编码长链 → 组件拓扑（provider 先序）。下表 `ID` 即组件名。

| 阶段 | 组件名 | 依赖 | 初始化内容（迁移自 init.cpp） | 状态 |
|---|---|---|---|---|
| T0 | （Tier0 core，无组件） | — | Serial/GDT/IDT/FPU/SIMD/PMM/VMM/SLAB/打印 | ✅ 保留 |
| T1 | `kernel.arch.acpi` | core | ACPI::Init + MADT_Init | ⬜ 规划 |
| T1 | `kernel.arch.apic` | acpi | LAPIC::Init + IOAPIC::Init + RemapIRQ(0→32) + PIC 屏蔽 | ⬜ 规划 |
| T1 | `kernel.time.pit` | apic | PIT::InitPIT | ⬜ 规划 |
| T1 | `kernel.time.hpet` | core | HPET::InitHPET | ⬜ 规划 |
| T1 | `kernel.time.rtc` | apic | RTC::InitRTC | ⬜ 规划 |
| T1 | `kernel.arch.smp` | acpi,apic,time.*,fs.cache | smp_init | ⬜ 规划 |
| T1 | `kernel.fs.cache` | core | 文件缓存限额 + BSP cache init | ⬜ 规划 |
| T1 | `kernel.sched.core` | smp,time.* | Schedule::Init + InitCPUThread | ⬜ 规划 |
| T0 | syscall 入口 | — | syscall_init（固定入口/治理表） | ✅ 保留 |
| T1 | `kernel.syscall.*`（按家族） | sched.core | 各家族 syscall 实现注册（见 §5） | 🟡 进行中 |
| T1 | `kernel.dev.core` | sched.core | Dev::Init + InitFFMAN | ⬜ 规划 |
| T1 | `kernel.pci.core` | acpi | PCI 驱动注册表 + 枚举机制 | ⬜ 规划 |
| T1 | `kernel.driver.ahci/nvme/xhci/e1000` | pci.core | 驱动 probe 注册（PCI match→probe） | ⬜ 规划 |
| T1 | `kernel.input.ps2.{kbd,mouse}` | apic | PS/2 键鼠驱动 | ⬜ 规划 |
| T1 | `kernel.fs.vfs` | dev.core | VFS/fd 管理 | ⬜ 规划 |
| T1 | `kernel.driver.block.*` | pci.core | 块设备接口注册（AHCI/NVMe/USB MSC） | ⬜ 规划 |
| T1 | `kernel.fs.rootfs` | fs.vfs, driver.block.* | ext4_kernel_init 根挂载 | ⬜ 规划 |
| T1 | `kernel.display.fb` | arch.core | FrameBufferDevice::Init | ⬜ 规划 |
| T1 | `kernel.sched.install` | fs.rootfs | Schedule::Install + TSC 校准 | ⬜ 规划 |
| T1 | `kernel.net.lwip` | driver.e1000, sched.install | lwIP 线程（deferred） | ⬜ 规划 |
| T1 | `kernel.user.init` | fs.rootfs, sched.install, display.fb | 桌面/硬件进程创建 | ⬜ 规划 |
| T0 | 启动收尾 | 全部 | sti + IPI + idle | ✅ 保留 |

> 迁移原则：**先加组件壳（拓扑/生命周期/能力/审计），再逐类把跨组件调用契约化**；
> 每步都必须保持"可启动到桌面 + KMOD_SELFTEST PASS"不回归。

## 5. syscall 实现家族组件（Tier 1）

固定入口与编号/语义在 Tier 0；实现家族作为内建组件导出 `KMOD_SYM_SYSCALL`（aux=槽位），
由 `KmodSyscallRegisterModule` 发布到治理表（不再在 `syscall_init()` 里硬编码赋值）。

| 组件（规划名） | 槽位（现有编号） | 能力 | 状态 |
|---|---|---|---|
| `kernel.syscall.file` | 0-5 fopen/fwrite/fread/fclose/flseek/fsize | FS | ⬜ |
| `kernel.syscall.proc` | 6-9,11,12,13,19 thread_launch/gettid/getpid/exit/yield/load/launch/kill | SCHED | ⬜ |
| `kernel.syscall.mem` | 10,14,15,18 pmmap/mmap/munmap/arch_prctl | MEM | ⬜ |
| `kernel.syscall.info` | 16,25,26,29 sysinfo/dev_getinfo/dev_ioctl/kmod_info | DEV/LOG | ⬜ |
| `kernel.syscall.time` | 17,27,28 time/monotonic_ms/sleep_ms | TIME | ⬜ |
| `kernel.syscall.dev` | 21,24 dev_mmap/dbgsout | DEV | ⬜ |
| `kernel.syscall.random` | 20 getrandom | MEM(random) | ⬜ |
| `.ko` 动态命名空间 | 128..511 | SYSCALL_IMPL / SYSCALL_OVERRIDE | ✅ 机制+演示已验证 |

## 6. 契约清单（内核 Tier0 导出 `.kout` + 组件私有契约）

| 契约名 | 提供者 | 消费者 | 语义 | 状态 |
|---|---|---|---|---|
| `kernel.mem.v1` | Tier0 | 所有模块 | kmalloc/kfree（+ kcalloc/krealloc/kmalloc_aligned） | ✅ |
| `kernel.log.v1` | Tier0 | 所有模块 | kprintf_/printf_/KmodLog | ✅ |
| `kernel.klib.v1` | Tier0 | 所有模块 | klib 工具 ops（字符串/格式化/qsort/bitmap/kld/随机/panic）+ 直接符号导出（strlen/hashmap_*/art_* 等） | ✅ |
| `kernel.sync.v1` | Tier0 | 所有模块 | 自旋锁/中断状态 ops + 直接符号 spinlock_lock/unlock | ✅ |
| `kernel.algo.v1` | Tier0 | 所有模块 | hashmap/ART/Queue/FIFO/List 容器 ops | ✅ |
| `kernel.epoch.v1` | Tier0 | 座位/热替换参与者 | KmodEpochEnter/Exit | ✅ |
| `kernel.libc.v1` | Tier0 | `.ko` 编译器兜底 | memcpy/memset/memmove/memcmp | ✅ |
| `kernel.time.v1` | Tier0 | 模块 | KmodNowMs | ✅ |
| `kernel.syscall.v1` | Tier0 | syscall 实现组件 | 固定 dispatch ABI + 治理槽 | ✅ |
| `kernel.seat.random.v1` | 座位 | sys_getrandom | 随机源策略 | ✅ |
| `kernel.acpi.v1` | 组件 | 平台驱动 | 表查询/唤醒 | ✅ |
| `kernel.pci.v1` | 组件 | 设备驱动 | 驱动注册/match/probe | ✅ |
| `kernel.block.v1` | 组件 | 文件系统 | 块读写/设备枚举 | ✅（导出；fs 预检走 PartitionManager 故刻意不消费） |
| `kernel.vfs.v1` / `kernel.fs.ext4.v1` | 组件 | FS 实现/根挂载 | VFS 固定契约 + ext4 桥 | ✅ |
| `kernel.netdev.v1` | 组件 | 协议栈 | 网卡收发注册表 | ✅（导出；e1000/lwIP 胶水仍硬编码，路线图） |
| `kernel.sched.policy.v1` | 座位 | 调度核心 | quantum/weight 策略覆盖 | ✅（座位 `kernel.seat.sched.policy.v1`） |
| `kernel.alloc.policy.v1` | 座位 | 分配核心 | large-alloc 阈值/OOM 策略 | ✅（座位 `kernel.seat.alloc.policy.v1`） |
| `kernel.display.v1` / `kernel.input.v1` | 组件 | 用户态输入/显示 | 帧缓冲信息；input 为草案 | display ✅ / input ⬜ 草案 |

## 7. 迁移验收标准（每个组件）

1. **声明**：`KCOMPONENT_BEGIN` manifest 完整（名称/版本/Tier/flags/能力/预算），
   imports 显式声明 provider + 私有契约，exports 声明对外契约；
2. **拓扑**：启动日志出现该组件的 provider 先序初始化行（`[kmod] builtin ready: ...`），
   依赖环/缺失依赖会被拒绝而非静默乱序；
3. **生命周期**：init 幂等、exit/quiesce 可重入保护；组件失败不阻断无关组件的回滚语义；
4. **能力**：能力位与组件实际资源使用匹配（准入审计）；
5. **不回归**：内核可启动到既有行为（QEMU 串口出现 `KMOD_SELFTEST PASS`，桌面/网络/FS 冒烟通过）；
6. **可替换性证据**：已覆盖策略/实现层的 `.ko` 热替换正/负向（随机源、调度策略、分配策略、
   固定 syscall 探针）；**组件节点级** builtin→.ko takeover 目前只有宿主测试的失败回滚用例
   （真实 `kernel.*` 名称受 `.ko` 保留前缀校验保护，未开放同名替换，见 §9.6）。
   驱动/FS 组件节点的 `.ko` 接管仍需先开放受信替换机制。

## 8. 验证门禁

```bash
# 1) 宿主测试（加载器/拓扑/能力/治理/syscall/座位；ASAN 负向 + fuzz）
cd tests && make -B kmod && ./bin/kmod_test
cd tests && make -B kmod-asan && ./bin/kmod_test_asan

# 2) 模块契约自检 + 内核构建
make -C kernel modules-check
make -C kernel            # 用根 Makefile 入口（SS_BUILD_DIR 正确）

# 3) QEMU 无头串口门禁（组件拓扑/初始化顺序 + 自检）
pwsh -File res/scripts/test/kmod_smoke.ps1
#  断言: 无异常字样; [kmod] builtin ready 行数 >= N; KMOD_SELFTEST PASS; 桌面进程启动日志
```

## 9. 已知限制与风险

1. 渐进迁移中：部分子系统仍是"组件壳 + 直接函数调用"，跨组件调用尚未全部契约化；
   表中"状态"列如实标注，禁止把规划项写成已完成。
2. 低隔离内核态无内存隔离（见 kernel-modules.md §15）；组件化治理解决的是依赖/生命周期/
   回滚/审计，不是安全沙箱。
3. 启动顺序约束多（定时器/中断/堆可用性），组件依赖表必须显式，禁止靠链接顺序碰运气。
4. 内建组件热替换需要旧实现可被原子摘除；有状态组件必须实现 quiesce/drain 与状态迁移
   回调，否则只能冷替换（需重启）。
5. 大型第三方代码（lwIP、lwext4、FatFs）会长期以"组件包装内建代码"形式存在：组件化的是
   **生命周期/契约/配置边界**，不是逐函数改写为跨模块调用。
6. **内建组件同名替换尚未开放**：`KmodGovHotReplace` 的 builtin→.ko takeover 事务已实现
   （quiesce/原子切换/失败回滚恢复 BUILTIN），宿主测试用非保留名合成组件
   `vendor.host.comp` 覆盖失败回滚；但 `.ko` 名称校验（`KmodNameValidate`）禁止
   `kernel./core.` 保留前缀，而真实 37 个内建组件全部使用 `kernel.*` 名，因此
   **外部 `.ko` 无法同名接管它们**。若要开放，需要受信机制（白名单/签名 +
   专用 capability，或允许内建组件声明可替换的 vendor 别名），当前未实现。
   实际已可通过 `.ko` 替换的是：座位策略（random/sched/alloc，见 QEMU selftest）
   与固定 syscall 探针实现（座位/syscall 治理层，不改变组件节点）。

## 10. 迁移进度台账（实测快照，2026-11-06）

启动序列已从 `x86_64_init` 硬编码长链切换为**组件拓扑驱动**：Tier0 只保留
Serial/GDT/IDT/FPU/SIMD/PMM/VMM/SLAB/SLUB/SMEP/`syscall_init` 入口装配/
`KmodInit`，其余全部由 `KmodBootComponents()` 按 manifest 依赖初始化。
QEMU 实测：`[kmod] builtin components: 37 declared`、`builtin ready` 全量、
`kernel.fs.rootfs: mounted sata0 at /mp/`、`[user.init] desktop PROCESS`、
`[kmod] boot load: 2 requested, 2 ready, 0 failed`、**`KMOD_SELFTEST PASS 12/12`**、
`[kmod] kernel exports: 57 symbol(s) published`（含 3 个共享服务契约）、
桌面/DOOM/lwIP 正常运行、0 Exception/Panic、0 STALL WATCHDOG（连续两次完整冒烟）。

| 类别 | 已组件化内容（37 个内建组件 + 15 个 .ko 演示模块 + 3 个座位 + Tier0 共享服务） | 状态 |
|---|---|---|
| 平台/时间 | kernel.arch.acpi / arch.apic / **arch.irq** / time.pit / time.hpet / time.rtc / arch.smp / fs.cache | ✅ 实测 |
| 调度 | kernel.sched.core / kernel.sched.install + **kernel.sched.policy.eevdf**（座位 kernel.seat.sched.policy.v1：策略参数/quantum 可被 .ko 热替换，默认逐位不变） | ✅ 实测（10/10） |
| 分配器 | heap 核心（PMM/VMM/SLAB/SLUB）固定 + **kernel.alloc.policy**（座位 kernel.seat.alloc.policy.v1：large-alloc 阈值/OOM 策略冷路径可替换，默认逐位不变） | ✅ 实测（11/11） |
| 设备核心 | kernel.dev.core / kernel.pci.core / kernel.pci.enumerate / kernel.block.core | ✅ 实测 |
| 驱动 | kernel.driver.ahci/nvme/xhci/e1000、kernel.input.ps2.kbd/mouse（两阶段：注册 match → 统一枚举 probe） | ✅ 实测 |
| 文件系统 | kernel.fs.vfs / fs.ext4 / fs.fatfs / fs.rootfs（VFS 固定契约 + 实现组件；根挂载候选探测） | ✅ 实测 |
| 网络 | kernel.net.netdev（设备注册表）/ kernel.net.lwip（协议栈 deferred 线程） | ✅ 实测 |
| 显示 | kernel.display.fb（帧缓冲设备组件） | ✅ 实测 |
| 用户态 | kernel.user.init（desktop/hw 进程创建；sti/IPI 仍由 Tier0 收尾） | ✅ 实测 |
| syscall 实现 | kernel.syscall.{file,proc,mem,info,time,dev,random}（27 个固定槽注册；入口/语义固定） | ✅ 实测 |
| 可加载扩展 | vendor.demo.hello/greeter（模块级依赖+私有契约）、badinit/badcap/cycle（事务/能力/环负向）、prng（随机座位热替换）、syscall（动态槽+固定槽覆写热替换）、sched（调度策略座位热替换）、alloc（分配策略座位热替换）、**klib（Tier0 共享服务：直接符号 + 契约 ops）** | ✅ 实测（selftest 12/12） |
| 共享服务 | **kernel.klib.v1 / kernel.sync.v1 / kernel.algo.v1** + klib C 链接函数直接符号导出（57 项内核导出）；内建组件静态共享、`.ko` 经 ELF 符号解析或 ops 契约共享 | ✅ 实测（宿主 + QEMU） |
| 座位 | kernel.seat.random.v1（随机源策略）、**kernel.seat.sched.policy.v1（调度策略）**、**kernel.seat.alloc.policy.v1（分配策略）**：Tier0 机制 + 模块提供者 + 失败回退 | ✅ 实测（含 .ko 热替换/回滚） |

### 10.1 迁移中发现并修复的 Tier0 问题（必须记录）

1. **IRQ0 解除屏蔽早于 PIT handler 注册**：旧 init 顺序是 `PIT::InitPIT()` -> ... ->
   `smp_init()` -> `IOAPIC::RemapIRQ(IRQ0->vec32, unmask)`。组件化若把 RemapIRQ 放进
   `arch.apic` 就会在 `time.pit` 之前打开 IRQ0：引导期进入 `(PANIC)Uncaught IRQ`
   打印路径，而此时 kprintf 正持有 `ptf_lock`，异常处理再入 kprintf -> 自死锁
   （现象：串口随机停在某个组件日志中间、无异常输出）。
   修复：新增内建组件 `kernel.arch.irq`（deps: arch.apic + time.pit + arch.smp）专门做
   RemapIRQ；`x86_64_init` 开头显式 `cli`，尾部 sti+IPI 才上线外部中断。
2. **引导栈深度**：组件图的 component-init 链路比旧硬编码 init 深（拓扑管理 +
   逐组件日志 + flanterm/serial 输出路径）。`start.S` 增加显式 64 KiB `__boot_stack`
   （.bss），不再依赖 bootloader 提供的栈大小。
3. **latent ABI bug**：`kernel/include/partition/mbrgpt.h` 的
   `GetPartitionStart/Size/End` 声明缺 `&`（定义是引用参数），此前因调用方被
   `--gc-sections` 回收而未暴露；`fs_fatfs` 组件引用 `IdentifyFat` 后链接期暴露。
   已把头声明修正为引用。
4. **异常路径打印可重入死锁（pre-existing，未根治）**：任何在已持 `ptf_lock` 的
   kprintf 内触发异常的处理路径都会自死锁。本次通过 IRQ 顺序 + 引导期 cli 规避；
   生产化建议给 panic 输出加 trylock/无锁串口旁路（列入路线图，见 §11 风险）。
5. **klib 直接符号导出暴露 latent 链接缺口**：`str.c` 的 `atoi()` 使用 `isdigit` 宏
   却未 include `klib/ctype.h`；该函数此前被 `--gc-sections` 回收所以未暴露，加入
   `kernel.klib.v1` 直接符号导出后链接报 `undefined symbol: isdigit`。修复：`str.c`
   补 include。说明"扩大模块可见导出面"本身会激活内核里长期未执行路径，导出前需
   以 clean build + 宿主/QEMU 双门禁复核。

### 10.2 验收判据与命令

```bash
make -C kernel modules-check        # .ko 契约段/重定位自检
make -C kernel                      # 组件图 + 内核链接零错误
cd tests && make -B kmod && ./bin/kmod_test        # 宿主门禁 6/6 PASS（含 ASAN 变体）
pwsh -File res/scripts/test/kmod_smoke.ps1          # QEMU 无头串口门禁：KMOD_SELFTEST PASS 12/12
```

> 自检在 **sti 之前单线程**执行（`x86_64_init` 内同步调用 `KmodSelfTest`）：
> 组件治理链路（加载/热替换/卸载）与桌面/文件缓存/定时器抢占并发交织会放大
> 既有 fs-cache/VMM 锁序问题；单线程执行让模块治理错误可确定性暴露。
> QEMU 负载高时使用 `-WaitSeconds 180~200`。

## 11. 路线图（剩余工作，按优先级）

1. ~~**调度策略座位**~~：已完成 `kernel.seat.sched.policy.v1` + kernel.sched.policy.eevdf +
   vendor.demo.sched .ko 热替换（selftest 覆盖）。
2. ~~**分配策略座位**~~：已完成 `kernel.seat.alloc.policy.v1` + kernel.alloc.policy（heap 冷路径）
   + vendor.demo.alloc .ko 热替换（selftest 覆盖）。
3. ~~**Tier0 共享服务层**~~：已完成 klib C 链接函数的直接符号导出（`.kout`）、
   `kernel.klib.v1` / `kernel.sync.v1` / `kernel.algo.v1` 契约 ops、kernel 导入
   contract/abi/size 强校验，以及 `vendor.demo.klib` 端到端自检（12/12）。
   后续扩展：更多 klib API（如 cstr 转换的线程安全版本）需新契约版本 v2。
4. 有状态组件的热替换状态迁移：`fs.fatfs`（纯注册表）作为第一个 FS 热替换 target；
   rootfs/net.lwip/display/user.init 属有状态不可回卷，标注冷替换。
5. panic/异常输出无锁旁路（见 §10.1-4）。
6. 契约头字段命名统一（B4 用 version/size，B1/B3 用 abi_version/ops_size；
   布局独立不影响兼容，建议下一 ABI 版本统一）。
7. PCI 驱动注册槽上限 16（旧核心 32）与 .ko 驱动热插拔支持。
8. 既有并发风险（pre-existing，非 kmod 引入）：file_cache_tick 在 PIT IRQ 上下文持
   fc 锁做较重工作，与 VMM/heap 锁存在跨核 AB-BA 窗口（QEMU 高负载下概率性
   STALL WATCHDOG）。缓解：kmod 自检移到 sti 前单线程；根治方向是把 fc tick 的
   重活下沉到 idle/softirq 上下文并统一 irqsave 锁序。

## 12. 目录迁移台账（`kernel/src` → `kernel/modules`，全模块化重构）

**目标**：`kernel/src` 只保留 Tier0 固定核心；所有组件实现（驱动/文件系统/网络/
分区/平台/syscall 家族）迁入 `kernel/modules`，由同一套 KCOMPONENT/manifest/
拓扑/能力/生命周期治理管辖。

| 迁移项 | 结果 |
|---|---|
| Tier0 保留在 `kernel/src` | 72 个源文件：`arch/<arch>/`（start/init、interrupt、vmm、cpu、simd、schedule 核心+syscall 入口、kmod 加载/治理/共享服务）、`mem/`、`klib/`、`elf/`、`flanterm/`、`print/`、`main.cpp` |
| Tier1 内建组件实现 | 197 个源文件 → `kernel/modules/builtin/`：`common/`（acpi、drivers、fs、net、partition、smbios；全架构）与 `x86_64/`（dev/pci、hpet、lapic/ioapic/pit/rtc/smp、syscall 实现家族、37 个组件壳按 13 类分子目录） |
| Tier2 可加载演示 | 15 个 `.cpp` → `kernel/modules/loadable/`（原 `kernel/modules/*.cpp`），`make -C kernel modules` 通配构建 |
| 构建系统 | `kernel/Makefile` 新增 `find_module_sources`：`modules/builtin/common` + `modules/builtin/$(KARCH)` 递归发现 `.c/.cpp/.S/.asm`，对象落 `obj-$(KARCH)/modules/<rel>`；`modules/loadable/*.cpp` 走独立 `.ko` 规则 |
| 测试/文档路径 | `tests/Makefile` FC_SRC、`tests/kmod/Makefile` 核心源清单、`res/scripts/test/analyze.sh`、`docs/*.md` 路径全部更新；新增 `kernel/modules/README.md` 目录说明 |
| 一致性校验 | 迁移前后源文件总数守恒（268 = 71 + 197；Tier0 后新增 1 个共享服务文件）；clean build + 宿主 6/6(+ASAN) + `modules-check` 15/15 + QEMU `KMOD_SMOKE PASS`（37 builtin ready、`KMOD_SELFTEST PASS 12/12`、0 STALL） |
