# 内核组件化重构（kernel components）

> 目标：整个内核的组件都进入同一套私有契约型链接与治理机制，而不只是做一个
> 外部 `.ko` 加载器 + 演示模块。
>
> 机制细节见 [kernel-modules.md](kernel-modules.md)。本文档是组件清单、Tier 划分、
> 启动序列映射、迁移状态与验收标准的唯一台账（每完成一项更新此表）。

---

## 1. 组件模型

- Tier 0 核心：不可模块化的机制（引导、页表、中断、上下文切换、syscall 入口、
  模块加载器、回滚、能力注册表、组件句柄/生命周期）。
- **Tier 0 核心 + 正在收缩的共享实现库**：不可模块化的机制（引导、页表、中断、
  上下文切换、syscall 入口、模块加载器、回滚、能力注册表）保留；driver/FS/net/sched
  的实现代码按 `kernel/modules/compose/*.sources` 逐组件迁入各自 `.ko`。
  尚未迁移的实现函数通过自动生成的 `kernel.component.v1` 导出表暴露给组件 `.ko`；
  已迁移组件之间走跨 `.ko` 的 `.out` 符号表 + `.kmod_dep_names` provider 先序。
- **DynModule（组件 .ko）**：40 个组件壳用 `-DKMOD_COMPONENT_AS_KO=1` 把
  `KCOMPONENT_*` 映射到 `.kmod_manifest`/`.in`/`.out`，各自编译成独立 ET_REL
  `.ko`；实现文件由 `modules/compose/<component>.sources` 追加进同一个 `.ko`。
  它们与 demo `.ko` 一起打进 **modules.saf**，由 Limine 单文件加载。
  内核镜像里 **不再有 .kbuiltin 静态组件表**。已迁入实现的组件：AHCI、NVMe、
  xHCI/USB、PS/2 键盘/鼠标（见 [module-implementation-migration.md](module-implementation-migration.md)）。
- **resident 核心节点**：`kernel.mem.pmm/vmm/heap`、`kernel.arch.syscall` 只是
  模块表里的 READY 节点（无代码镜像），供 `KdllInject` 注入/审计。
- 所有组件统一由 `KdllInject` 按名注入（`kernel.dynlink.v1`）；Tier 1/Tier 2
  的区分已删除，详见 [kernel-dynlink.md](kernel-dynlink.md)。

组件 = manifest（身份/Tier/能力/生命周期/预算）+ imports（模块级依赖+私有契约）+ exports
（函数/数据/私有契约 ops/座位/syscall 实现）。内核不统一业务接口：组件间契约由组件自己
约定命名空间与结构布局，内核只做机械校验（字符串/版本/尺寸）。

## 2. Tier 划分原则

| 判据 | 划入 Tier 0（固定机制） | 划入 DynModule（可替换实现/策略） |
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

## 3. 组件 .ko 线格式与启动（modules.saf）

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

- 组件壳源码不变；构建用 `-DKMOD_COMPONENT_AS_KO=1` 把 `KCOMPONENT_BEGIN` 映射为
  `.kmod_manifest` + `.in`/`.out`，生成独立 `.ko`；
- `make -C kernel modules` 产出 40 个组件 `.ko`；`modules-saf` 把 40 组件 +
  18 demo 打进一个 `bin-x86_64/modules.saf`；
- 启动：Limine 只加载 `boot():/boot/modules.saf` → `KmodSafIngest` 解析入池
  （`kernel.*` 标记 trusted）→ `KmodBootLoad` 按 `.in` 依赖拓扑加载 43 个 BOOT 模块；
- 失败逆序回滚；环/缺失依赖/能力问题在启动日志中以 `[kmod]` 审计可见；
- trusted 组件 `.ko` 不允许外部同名热替换/卸载（§9.6 的限制仍然适用）。

## 4. 启动序列组件化映射（x86_64）

旧 `x86_64_init` 的硬编码长链 → 组件拓扑（provider 先序）。下表 `ID` 即组件名。

| 阶段 | 组件名 | 依赖 | 初始化内容（迁移自 init.cpp） | 状态 |
|---|---|---|---|---|
| T0 | （Tier0 core，无组件） | — | Serial/GDT/IDT/FPU/SIMD/PMM/VMM/SLAB/打印 | 保留 |
| T1 | `kernel.arch.acpi` | core | ACPI::Init/FindTable + MADT_Init | **解析实现已迁入 .ko**；mcfg/madt 全局量留 Tier0 |
| T1 | `kernel.arch.apic` | acpi,time.hpet | LAPIC + IOAPIC 实现 + RemapIRQ + PIC 屏蔽 | **LAPIC/IOAPIC 已迁入 .ko**；IDT 仍 Tier0 |
| T1 | `kernel.time.pit` | apic | PIT::InitPIT | 规划 |
| T1 | `kernel.time.hpet` | acpi | HPET::InitHPET/Available/GetTimeNS/SleepNS | **实现已迁入 .ko**；Tier0 跳板 |
| T1 | `kernel.time.rtc` | apic | RTC::InitRTC/read/update/ToUnixTime | **实现已迁入 .ko**；RTC 字段/跳板留 Tier0 |
| T1 | `kernel.arch.smp` | acpi,apic,time.*,fs.cache | smp_init | 规划 |
| T1 | `kernel.fs.cache` | core | 文件缓存限额 + BSP cache init | 规划 |
| T1 | `kernel.sched.core` | smp,time.* | Schedule::Init + InitCPUThread | 规划 |
| T1 | `kernel.sched.policy.eevdf` | sched.core | 默认 EEVDF 策略座位 + sched_bench | **sched_bench 已迁入 .ko**；策略默认实现 |
| T0 | syscall 入口 | — | syscall_init（固定入口/治理表） | 保留 |
| T1 | `kernel.syscall.*`（按家族） | sched.core | 各家族 syscall 实现注册（见 §5） | partial 进行中 |
| T1 | `kernel.dev.core` | sched.core | Dev::Init + InitFFMAN + mount table | **Dev 注册表与挂载点表已迁入 .ko**；fd_manager 仍 Tier0 |
| T1 | `kernel.pci.core` | acpi | PCI 驱动注册表 + 枚举机制 + 名称表 | **名称表已迁入 .ko**；PCI 配置/枚举仍在 Tier0 |
| T1 | `kernel.driver.ahci/nvme/xhci` | pci.core | 驱动 probe 注册（PCI match→probe） | **实现已迁入 .ko** |
| T1 | `kernel.driver.e1000` | pci.core | 驱动 probe 注册（PCI match→probe） | **实现已迁入 .ko**（含 lwIP 胶水；DHCP/ping 通过） |
| T1 | `kernel.input.ps2.{kbd,mouse}` | apic | PS/2 键鼠驱动 | **实现已迁入 .ko**（kbd 依赖 xhci HID 符号） |
| T1 | `kernel.fs.vfs` | dev.core | VFS registry + fd_alloc/get/free | **VFS + fd 操作已迁入 .ko** |
| T1 | `kernel.fs.fdcore` | 无 | fd_manager init/destroy/dup | **实现已迁入 .ko**；sched.core 依赖它先加载 |
| T1 | `kernel.fs.fccore` | 无 | file cache core + idle/fsync/tick | **实现已迁入 .ko**（112 KB） |
| T1 | `kernel.fs.ext4` | fs.vfs,block.core | lwext4 mount/umount/stats + 目录 API | **实现已迁入 .ko**（根盘挂载通过） |
| T1 | `kernel.fs.fatfs` | fs.vfs,block.core,fs.ext4 | FatFs 完整实现（ff/diskio/identfat） | **全部迁入 .ko**（297 KB；挂载路径仍为 identify-only） |
| T1 | `kernel.driver.block.*` | pci.core | 块设备接口注册（AHCI/NVMe/USB MSC） | 规划 |
| T1 | `kernel.block.core` | dev.core,pci.core | PartitionManager + block 契约 | **PartitionManager 已迁入 .ko** |
| T1 | `kernel.fs.rootfs` | fs.vfs, driver.block.* | ext4_kernel_init 根挂载 | 规划 |
| T1 | `kernel.display.fb` | dev.core | FrameBufferDevice::Init | **实现已迁入 .ko** |
| T1 | `kernel.sched.install` | fs.rootfs | Schedule::Install + TSC 校准 | 规划 |
| T1 | `kernel.net.lwip` | driver.e1000, sched.install | lwIP 协议栈 + e1000 胶水（deferred） | **实现已迁入 .ko**（DHCP/ping 通过） |
| T1 | `kernel.user.init` | fs.rootfs, sched.install, display.fb | 桌面/硬件进程创建 | 规划 |
| T0 | 启动收尾 | 全部 | sti + IPI + idle | 保留 |

> 迁移原则：先加组件壳（拓扑/生命周期/能力/审计），再逐类把跨组件调用契约化；
> 每步都必须保持"可启动到桌面 + KMOD_SELFTEST PASS"不回归。

## 5. syscall 实现家族组件（组件 .ko，trusted）

固定入口与编号/语义在 Tier 0；实现家族作为内建组件导出 `KMOD_SYM_SYSCALL`（aux=槽位），
由 `KmodSyscallRegisterModule` 发布到治理表（不再在 `syscall_init()` 里硬编码赋值）。

| 组件（规划名） | 槽位（现有编号） | 能力 | 状态 |
|---|---|---|---|
| `kernel.syscall.file` | 0-5,22,23 fopen/fwrite/fread/fclose/flseek/fsize + mkdir + readdir | FS | **实现已迁入 .ko**（走 ext4 目录 API） |
| `kernel.syscall.proc` | 6-9,11,12,13,19,31 thread_launch/gettid/getpid/exit/yield/load/launch/kill/child_event | SCHED | **实现已迁入 .ko**（exec.cpp/task.cpp；elf_load/NOT_RUNQ_P 留 Tier0） |
| `kernel.syscall.mem` | 10,14,15,18 pmmap/mmap/munmap/arch_prctl | MEM | **全部实现已迁入 .ko**（pmmap 拆到 pmmap.cpp，cpu.cpp 含 arch_prctl） |
| `kernel.syscall.info` | 16,25,26,21 sysinfo/dev_getinfo/dev_ioctl/dev_mmap | DEV/LOG | **4 槽全部迁入 .ko**；sysinfo 页 init/refresh 留内核 |
| `kernel.syscall.time` | 17,27,28 time/monotonic_ms/sleep_ms | TIME | **实现已迁入 .ko**（mktime 收进 Tier0 klib） |
| `kernel.syscall.dev` | 24 dbgsout | DEV | **dbgout 已迁入 .ko** |
| `kernel.syscall.random` | 20 getrandom | MEM(random) | **实现已迁入 .ko** |
| `.ko` 动态命名空间 | 128..511 | SYSCALL_IMPL / SYSCALL_OVERRIDE | 机制+演示已验证 |

## 6. 契约清单（内核 Tier0 导出 `.kout` + 组件私有契约）

| 契约名 | 提供者 | 消费者 | 语义 | 状态 |
|---|---|---|---|---|
| `kernel.mem.v1` | Tier0 | 所有模块 | kmalloc/kfree（+ kcalloc/krealloc/kmalloc_aligned） | 已就绪 |
| `kernel.log.v1` | Tier0 | 所有模块 | kprintf_/printf_/KmodLog | 已就绪 |
| `kernel.klib.v1` | Tier0 | 所有模块 | klib 工具 ops（字符串/格式化/qsort/bitmap/kld/随机/panic）+ 直接符号导出（strlen/hashmap_*/art_* 等） | 已就绪 |
| `kernel.sync.v1` | Tier0 | 所有模块 | 自旋锁/中断状态 ops + 直接符号 spinlock_lock/unlock | 已就绪 |
| `kernel.algo.v1` | Tier0 | 所有模块 | hashmap/ART/Queue/FIFO/List 容器 ops | 已就绪 |
| `kernel.epoch.v1` | Tier0 | 座位/热替换参与者 | KmodEpochEnter/Exit | 已就绪 |
| `kernel.libc.v1` | Tier0 | `.ko` 编译器兜底 | memcpy/memset/memmove/memcmp | 已就绪 |
| `kernel.time.v1` | Tier0 | 模块 | KmodNowMs | 已就绪 |
| `kernel.syscall.v1` | Tier0 | syscall 实现组件 | 固定 dispatch ABI + 治理槽 | 已就绪 |
| `kernel.seat.random.v1` | 座位 | sys_getrandom | 随机源策略 | 已就绪 |
| `kernel.acpi.v1` | 组件 | 平台驱动 | 表查询/唤醒 | 已就绪 |
| `kernel.pci.v1` | 组件 | 设备驱动 | 驱动注册/match/probe | 已就绪 |
| `kernel.block.v1` | 组件 | 文件系统 | 块读写/设备枚举 |（导出；fs 预检走 PartitionManager 故刻意不消费） |
| `kernel.vfs.v1` / `kernel.fs.ext4.v1` | 组件 | FS 实现/根挂载 | VFS 固定契约 + ext4 桥 | 已就绪 |
| `kernel.netdev.v1` | 组件 | 协议栈 | 网卡收发注册表 |（导出；e1000/lwIP 胶水仍硬编码，路线图） |
| `kernel.sched.policy.v1` | 座位 | 调度核心 | quantum/weight 策略覆盖 |（座位 `kernel.seat.sched.policy.v1`） |
| `kernel.alloc.policy.v1` | 座位 | 分配核心 | large-alloc 阈值/OOM 策略 |（座位 `kernel.seat.alloc.policy.v1`） |
| `kernel.display.v1` / `kernel.input.v1` | 组件 | 用户态输入/显示 | 帧缓冲信息；input 为草案 | display 已就绪 / input 草案 |

## 7. 迁移验收标准（每个组件）

1. 声明：`KCOMPONENT_BEGIN` manifest 完整（名称/版本/Tier/flags/能力/预算），
   imports 显式声明 provider + 私有契约，exports 声明对外契约；
2. 拓扑：启动日志出现该组件的 provider 先序初始化行（`[kmod] loaded kernel.xxx: ...`），
   依赖环/缺失依赖会被拒绝而非静默乱序；
3. 生命周期：init 幂等、exit/quiesce 可重入保护；组件失败不阻断无关组件的回滚语义；
4. 能力：能力位与组件实际资源使用匹配（准入审计）；
5. 不回归：内核可启动到既有行为（QEMU 串口出现 `KMOD_SELFTEST PASS`，桌面/网络/FS 冒烟通过）；
6. 可替换性证据：已覆盖策略/实现层的 `.ko` 热替换正/负向（随机源、调度策略、分配策略、
   固定 syscall 探针）；组件节点级 builtin→.ko takeover 目前只有宿主测试的失败回滚用例
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
#  断言: 无异常字样; [kmod] loaded kernel. 行数 >= N; KMOD_SELFTEST PASS; 桌面进程启动日志
```

## 9. 已知限制与风险

1. 渐进迁移中：部分子系统仍是"组件壳 + 直接函数调用"，跨组件调用尚未全部契约化；
   表中"状态"列只写实际状态，规划项不能写成已完成。
2. 低隔离内核态无内存隔离（见 kernel-modules.md §15）；组件化治理解决的是依赖/生命周期/
   回滚/审计，不是安全沙箱。
3. 启动顺序约束多（定时器/中断/堆可用性），组件依赖表必须显式，禁止靠链接顺序碰运气。
4. 内建组件热替换需要旧实现可被原子摘除；有状态组件必须实现 quiesce/drain 与状态迁移
   回调，否则只能冷替换（需重启）。
5. 大型第三方代码（lwIP、lwext4、FatFs）会长期以"组件包装 kernel 共享实现"形式存在：
   组件化的是生命周期/契约/配置边界，不是逐函数改写为跨模块调用。
6. trusted 组件 .ko 同名替换尚未开放：`KmodGovHotReplace` 的 builtin→.ko takeover
   事务仍保留（宿主测试用非保留名合成组件 `vendor.host.comp` 验证失败回滚），
   但 `KmodGovUnload` 对 `KMOD_MODF_TRUSTED` 返回 POLICY。外部 .ko 也无法使用
   `kernel.*` 名（`KmodNameValidate` 拒绝保留前缀）；只有 modules.saf 入池的
   kernel.* 镜像会被标记 trusted。若要开放，需要受信机制（白名单/签名 +
   专用 capability），当前未实现。
   实际已可通过 `.ko` 替换的是：座位策略（random/sched/alloc，见 QEMU selftest）
   与固定 syscall 探针实现（座位/syscall 治理层，不改变组件节点）。

## 10. 迁移进度台账（实测快照，2026-11-06）

启动序列已从 `x86_64_init` 硬编码长链切换为组件拓扑驱动：Tier0 只保留
Serial/GDT/IDT/FPU/SIMD/PMM/VMM/SLAB/SLUB/SMEP/`syscall_init` 入口装配/
`KmodInit`，其余全部由 `KmodBootIngest()` 解析 modules.saf、`KmodBootLoad()` 按 manifest 依赖初始化。
QEMU 实测：`[kmod] saf: 58 .ko accepted`、`loaded kernel.*` 40 条、
`kernel.fs.rootfs: mounted sata0 at /mp/`、`[user.init] desktop PROCESS`、
`[kmod] boot load: 43 requested, 43 ready, 0 failed`、`KMOD_SELFTEST PASS 14/14`、
`[kmod] kernel exports: 260 symbol(s) published`（含 197 个 kernel.component.v1）、
桌面/DOOM/lwIP 正常运行、0 Exception/Panic、0 STALL WATCHDOG（连续两次完整冒烟）。

| 类别 | 已组件化内容（40 个组件 .ko + 18 个 demo .ko + 3 个座位 + 共享实现库/服务） | 状态 |
|---|---|---|
| 平台/时间 | kernel.arch.acpi / arch.apic / **arch.irq** / time.pit / time.hpet / time.rtc / arch.smp / fs.cache | 实测 |
| 调度 | kernel.sched.core / kernel.sched.install + **kernel.sched.policy.eevdf**（座位 kernel.seat.sched.policy.v1：策略参数/quantum 可被 .ko 热替换，默认逐位不变） | 实测（10/10） |
| 分配器 | heap 核心（PMM/VMM/SLAB/SLUB）固定 + **kernel.alloc.policy**（座位 kernel.seat.alloc.policy.v1：large-alloc 阈值/OOM 策略冷路径可替换，默认逐位不变） | 实测（11/11） |
| 设备核心 | kernel.dev.core / kernel.pci.core / kernel.pci.enumerate / kernel.block.core | 实测 |
| 驱动 | kernel.driver.ahci/nvme/xhci/e1000、kernel.input.ps2.kbd/mouse（两阶段：注册 match → 统一枚举 probe） | 实测 |
| 文件系统 | kernel.fs.vfs / fs.ext4 / fs.fatfs / fs.rootfs（VFS 固定契约 + 实现组件；根挂载候选探测） | 实测 |
| 网络 | kernel.net.netdev（设备注册表）/ kernel.net.lwip（协议栈 deferred 线程） | 实测 |
| 显示 | kernel.display.fb（帧缓冲设备组件） | 实测 |
| 用户态 | kernel.user.init（desktop/hw 进程创建；sti/IPI 仍由 Tier0 收尾） | 实测 |
| syscall 实现 | kernel.syscall.{file,proc,mem,info,time,dev,random}（27 个固定槽注册；入口/语义固定） | 实测 |
| DOS 子系统 | **kernel.arch.dosvm.ko**（原 Tier1 实现 + 原 Tier2 门面合并；syscall 210..214；init 注入 pmm/acpi） | 实测（DOSVM trip PASS） |
| 统一注入 | **kernel.dynlink.v1** + 4 个 resident 核心服务 + `vendor.demo.inject/late`（LoadLibrary/GetProcAddress/依赖自动注入/eject） | 实测（selftest 14/14） |
| 可加载扩展 | vendor.demo.hello/greeter（模块级依赖+私有契约）、badinit/badcap/cycle（事务/能力/环负向）、prng（随机座位热替换）、syscall（动态槽+固定槽覆写热替换）、sched（调度策略座位热替换）、alloc（分配策略座位热替换）、**klib（Tier0 共享服务：直接符号 + 契约 ops）** | 实测（selftest 14/14） |
| 共享服务 | **kernel.klib.v1 / kernel.sync.v1 / kernel.algo.v1** + klib C 链接函数直接符号导出 + **197 项 kernel.component.v1**（组件 .ko 解析 kernel 实现）；共 260 项内核导出 | 实测（宿主 + QEMU） |
| 座位 | kernel.seat.random.v1（随机源策略）、**kernel.seat.sched.policy.v1（调度策略）**、**kernel.seat.alloc.policy.v1（分配策略）**：Tier0 机制 + 模块提供者 + 失败回退 | 实测（含 .ko 热替换/回滚） |

### 10.1 迁移中发现并修复的 Tier0 问题（必须记录）

1. IRQ0 解除屏蔽早于 PIT handler 注册：旧 init 顺序是 `PIT::InitPIT()` -> ... ->
   `smp_init()` -> `IOAPIC::RemapIRQ(IRQ0->vec32, unmask)`。组件化若把 RemapIRQ 放进
   `arch.apic` 就会在 `time.pit` 之前打开 IRQ0：引导期进入 `(PANIC)Uncaught IRQ`
   打印路径，而此时 kprintf 正持有 `ptf_lock`，异常处理再入 kprintf -> 自死锁
   （现象：串口随机停在某个组件日志中间、无异常输出）。
   修复：新增内建组件 `kernel.arch.irq`（deps: arch.apic + time.pit + arch.smp）专门做
   RemapIRQ；`x86_64_init` 开头显式 `cli`，尾部 sti+IPI 才上线外部中断。
2. 引导栈深度：组件图的 component-init 链路比旧硬编码 init 深（拓扑管理 +
   逐组件日志 + flanterm/serial 输出路径）。`start.S` 增加显式 64 KiB `__boot_stack`
   （.bss），不再依赖 bootloader 提供的栈大小。
3. latent ABI bug：`kernel/include/partition/mbrgpt.h` 的
   `GetPartitionStart/Size/End` 声明缺 `&`（定义是引用参数），此前因调用方被
   `--gc-sections` 回收而未暴露；`fs_fatfs` 组件引用 `IdentifyFat` 后链接期暴露。
   已把头声明修正为引用。
4. 异常路径打印可重入死锁（pre-existing，未根治）：任何在已持 `ptf_lock` 的
   kprintf 内触发异常的处理路径都会自死锁。本次通过 IRQ 顺序 + 引导期 cli 规避；
   生产化建议给 panic 输出加 trylock/无锁串口旁路（列入路线图，见 §11 风险）。
5. klib 直接符号导出暴露 latent 链接缺口：`str.c` 的 `atoi()` 使用 `isdigit` 宏
   却未 include `klib/ctype.h`；该函数此前被 `--gc-sections` 回收所以未暴露，加入
   `kernel.klib.v1` 直接符号导出后链接报 `undefined symbol: isdigit`。修复：`str.c`
   补 include。说明"扩大模块可见导出面"本身会激活内核里长期未执行路径，导出前需
   以 clean build + 宿主/QEMU 双门禁复核。

### 10.2 验收判据与命令

```bash
make -C kernel modules-check        # .ko 契约段/重定位自检
make -C kernel                      # 组件图 + 内核链接零错误
cd tests && make -B kmod && ./bin/kmod_test        # 宿主门禁 7/7 PASS（含 ASAN 变体）
pwsh -File res/scripts/test/kmod_smoke.ps1          # QEMU 无头串口门禁：KMOD_SELFTEST PASS 14/14
```

> 自检在 sti 之前单线程执行（`x86_64_init` 内同步调用 `KmodSelfTest`）：
> 组件治理链路（加载/热替换/卸载）与桌面/文件缓存/定时器抢占并发交织会放大
> 既有 fs-cache/VMM 锁序问题；单线程执行让模块治理错误可确定性暴露。
> QEMU 负载高时使用 `-WaitSeconds 180~200`。

## 11. 路线图（剩余工作，按优先级）

1. ~~调度策略座位~~：已完成 `kernel.seat.sched.policy.v1` + kernel.sched.policy.eevdf +
   vendor.demo.sched .ko 热替换（selftest 覆盖）。
2. ~~分配策略座位~~：已完成 `kernel.seat.alloc.policy.v1` + kernel.alloc.policy（heap 冷路径）
   + vendor.demo.alloc .ko 热替换（selftest 覆盖）。
3. ~~Tier0 共享服务层~~：已完成 klib C 链接函数的直接符号导出（`.kout`）、
   `kernel.klib.v1` / `kernel.sync.v1` / `kernel.algo.v1` 契约 ops、kernel 导入
   contract/abi/size 强校验，以及 `vendor.demo.klib` 端到端自检（14/14）。
   后续扩展：更多 klib API（如 cstr 转换的线程安全版本）需新契约版本 v2。
4. 有状态组件的热替换状态迁移：`fs.fatfs`（纯注册表）作为第一个 FS 热替换 target；
   rootfs/net.lwip/display/user.init 属有状态不可回卷，标注冷替换。
5. panic/异常输出无锁旁路（见 §10.1-4）。
6. 契约头字段命名统一（B4 用 version/size，B1/B3 用 abi_version/ops_size；
   布局独立不影响兼容，建议下一 ABI 版本统一）。
7. PCI 驱动注册槽上限 16（旧核心 32）与 .ko 驱动热插拔支持。
8. 已修复（原 pre-existing 并发风险）：`file_cache_tick` 过去在 PIT IRQ 上下文
   持 fc 锁做较重工作，会与 idle 上下文里持同一把锁的 `file_cache_idle_handler`
   同核自死锁（并存在与 VMM/heap 锁的跨核 AB-BA 窗口）。现在 IRQ 路径只做
   `clock` 原子自增与均值更新、不取锁；LRU/ART 衰减下沉到
   `file_cache_maybe_decay()`，由 get/put/idle 在进程上下文推进。
   仍建议后续给 fc 锁统一 irqsave 语义并做锁序审计。

## 12. 目录迁移台账（`kernel/src` → `kernel/modules`，全模块化重构）

目标：`kernel/src` 只保留 Tier0 固定核心；所有组件实现（驱动/文件系统/网络/
分区/平台/syscall 家族）迁入 `kernel/modules`，由同一套 KCOMPONENT/manifest/
拓扑/能力/生命周期治理管辖。

| 迁移项 | 结果 |
|---|---|
| Tier0 保留在 `kernel/src` | 73 个源文件：`arch/<arch>/`（start/init、interrupt、vmm、cpu、simd、schedule 核心+syscall 入口、kmod 加载/治理/共享服务）、`mem/`、`klib/`、`elf/`、`flanterm/`、`print/`、`main.cpp` |
| 组件/实现分界 | 197 个源文件 → `kernel/modules/builtin/`：实现（common/ + x86_64/ 非 components 目录）编入 kernel 共享实现库；40 个组件壳（components/ + dosvm/）编译成独立 .ko，通过 kernel.component.v1 调用实现 |
| relocatable DynModule | 18 个 demo/扩展 `.cpp` → `kernel/modules/loadable/`；与 40 组件 .ko 一起由 `modules-saf` 打进 `modules.saf` |
| 构建系统 | `kernel/Makefile` 新增 `find_module_sources`：`modules/builtin/common` + `modules/builtin/$(KARCH)` 递归发现 `.c/.cpp/.S/.asm`，对象落 `obj-$(KARCH)/modules/<rel>`；`modules/loadable/*.cpp` 走独立 `.ko` 规则 |
| 测试/文档路径 | `tests/Makefile` FC_SRC、`tests/kmod/Makefile` 核心源清单、`res/scripts/test/analyze.sh`、`docs/*.md` 路径全部更新；新增 `kernel/modules/README.md` 目录说明 |
| 一致性校验 | 迁移前后源文件总数守恒（268 = 71 + 197；Tier0 后新增 1 个共享服务文件）；clean build + 宿主 7/7(+ASAN) + `modules-check` 58/58 + QEMU `KMOD_SMOKE PASS`（saf 58、43/43 boot load、`KMOD_SELFTEST PASS 14/14`、0 STALL） |

## 13. ELF 镜像读取：短读 + 端口寄存器竞态导致的偶发启动卡死（2026-11）

### 13.1 现象

概率性（约 1/5 ~ 1/10）启动时停在壁纸，`[perf] desktop ready in` 永远不出现，
串口日志里同时出现：

```
[KERR] AHCI: port 0 recovered (CI 0x1 -> 0, IS was 0x40000000, attempts 1)
[INFO] [dynlink] DT_NEEDED 'libsky.so' -> /mp/libsky.so (1032200 bytes)
[KERR] [dynlink] libsky.so: bad ELF header
[KERR] [dynlink] prepare failed: -22
[user.init] WARN: desktop.elf thread creation failed (PID 6)
```

`IS was 0x40000000` 是 `PxIS.TFES`（Task File Error），即端口报了一次命令错误。

### 13.2 根因（两层，缺一不可）

1. 调用方丢掉了"读了几个字节"这个事实。
   `sys_load()`（`kernel/modules/builtin/x86_64/syscall/exec.cpp`）、
   `Schedule::NewThread()`（`kernel/src/arch/x86_64/schedule/task.cpp`）和
   `dyn_read_needed()`（`kernel/src/elf/elf_dyn.cpp`）都是
   `MP->FSOPS->read(fd, buf, fsize, 0);` —— 既不检查返回码，也不检查
   `*rcnt`。而 `ext4_fread()` 的语义是"尽力读"：某个块 I/O 失败时它
   `goto Finish` 提前返回（返回错误码，`*rcnt` 只包含已经拷好的部分）。
   于是 `kmalloc` 出来未初始化的剩余部分被当成 ELF 头/程序头解析 ——
   `bad ELF header` 就是这么来的。libsky.so 是启动期最大的单次读
   （1,032,200 B ≈ 2017 扇区，要发 252 条 8 扇区命令），命中的概率最高。

2. AHCI 端口并发时互相踩寄存器。
   `Port::Read/Write` 在入口写 `hbaPort->interruptStatus = -1`（清端口级
   PxIS），发命令时写 `hbaPort->commandIssue = 1 << slot`（赋值，不是
   置位）。`PxIS/PxCI` 是端口级共享寄存器：两笔命令同时在飞时，后一笔
   会把前一笔的在飞位清掉（硬件判定该命令被取消 → 报 TFES），也会把前一笔
   的错误状态清掉（前一笔的等待者看不到 TFES，只能耗到 5 秒超时）。
   4 核下桌面加载期确实存在并发读（进程加载 + 字体/主题文件读取）。

另外 `Port::Read` 在 "taskFileData 一直 BUSY/DRQ" 的兜底分支里
`return false` 时没有释放 `slot_lock` —— 该端口之后所有 I/O 都会在同一
把锁上死等，等同于文件系统永久停摆。

### 13.3 修复

| 位置 | 修改 |
|---|---|
| 新增 `kernel/src/elf/elf_image.cpp` + `kernel/include/elf/elf_image.h` | `kelf_read_file()`：校验 `rc == 0 && rcnt == fsize`，否则**整文件重读**（最多 4 次，每次重读前清零缓冲区，失败打印 路径/次数/rc/实读/期望）；`kelf_validate_image()`：校验 e_ident 魔数、ELFCLASS64、EM_X86_64、e_type、程序头表边界、每个 `PT_LOAD` 的 `[p_offset, p_offset+p_filesz)` 都在文件内 |
| `exec.cpp` / `task.cpp` / `elf_dyn.cpp` | 三处加载器统一改走 `kelf_read_file()` + `kelf_validate_image()`；`elf_dyn_prepare()` 里的 .so 头检查再叠加一次公共校验 |
| `ahci.cpp` + `ahci.h` | 新增端口级 `io_lock`，把"选槽 → 填表 → 写 PxCI → 轮询完成"整段串行化（PxIS/PxCI/PRDT 都是端口级共享状态）；`PxCI` 改置位写 `|= 1<<slot`；两条早退路径补 `slot_lock` 解锁 |
| `programs/desktop/loader.cpp` | `SpawnWindowedApp()` 的就绪等待每 64 次探一次进程存活，客户端已死立即放弃等待并打日志；即使没等到 `CLIENT_READY` 也照常挂载（窗口纸面已由 WM 预涂），单个客户端失败不再拖住整个桌面 |

### 13.4 验证

`res/scripts/test/boot_stability_5x.py`：连续 5 次冷启动，要求每次都出现
`[perf] desktop ready in`，且日志里没有 `bad ELF header` / `prepare failed` /
`[elfimg] short read` / panic / STALL。

```
BOOT STABILITY (5 runs)
  PASS boot #1  ready in 2256ms, 5.2s wall
  PASS boot #2  ready in 2318ms, 5.2s wall
  PASS boot #3  ready in 2120ms, 5.7s wall
  PASS boot #4  ready in 2549ms, 5.7s wall
  PASS boot #5  ready in 2229ms, 5.2s wall
BOOT STABILITY: PASS (5/5 ready)
```

`start_explorer_smoke.py`（菜单/Explorer/键盘/鼠标）同为 PASS。桌面输入延迟
与鼠标投递的实测数据见 `docs/desktop-features.md` 第 12~14 节。


### 13.5 追加：VMA 哨兵页 UAF（QA 复核发现，1/16 次整机 halt）

`CleanPM()` 原来 `PMM::Free(PHYSICAL(pm->vma_head))` —— 但哨兵来自 `vma.cpp`
的按页切分节点池（一页 ≈ 50 个节点分给 50 个 pagemap），整页释放会让别的
pagemap 正在用的 VMA 节点被页表/用户页覆写，下一次 `rb_insert` 在 CR2=0x8 上
内核 #PF → halt。修复：新增 `VMM::VMA::ReleaseSentinel()` 只归还节点；
`sys_sysinfo()` 的 VMA 操作补 `pm->vma_lock`（VMA 树的 rb 内部锁在本内核里是
空操作，唯一同步手段就是这把锁）；`rb_insert_raw()` 对"parent 红但无祖父"
做了收敛防御，坏树不再打死整机。

详细根因/修复/验证（7 次冒烟 + 新 `client_churn_gate.py` 10/15 轮窗口开关扰动
+ 5 次启动）见 `docs/desktop-features.md` 第 14 节。
