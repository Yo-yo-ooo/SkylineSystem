# 整体架构与启动流程

## 1. 分层

```
┌─────────────────────────────────────────────────────────┐
│  用户态 ELF (desktop.elf / hw.elf / helloworld)          │
│  programs/  —— 窗口管理器、控制台、合成器 (MIT)           │
├─────────────────────────────────────────────────────────┤
│  lib/  —— 自由 standing libc (printf/string/malloc/TTF)   │
│  系统调用边界 (syscalln.h, 共 17 号)                     │
├─────────────────────────────────────────────────────────┤
│  内核 (GPL-2.0-only)                                     │
│  ├─ 调度   schedule/   3EVDF、线程、信号、定时器          │
│  ├─ 内存   vmm/ mem/   五级页表、PMM、SLUB、VMA          │
│  ├─ 并发   smp/        per-CPU、AP bring-up、IPI         │
│  ├─ 中断   interrupt/  GDT/IDT/ISR、LAPIC/IOAPIC/PIC     │
│  ├─ 文件   fs/         VFS(fd/fc)、SAF、lwext4、FAT      │
│  ├─ 驱动   drivers/    NVMe/AHCI/ATA、USB(xHCI)、PS/2、FB│
│  ├─ 网络   net/        lwip 移植 (骨架阶段)               │
│  └─ 数据结构 klib/algorithm/  ART 基数树、RBTree、hashmap │
├─────────────────────────────────────────────────────────┤
│  Limine 引导协议 (BIOS + UEFI) → 进入 64-bit 直接执行     │
└─────────────────────────────────────────────────────────┘
```

用户态与内核态跑在**独立地址空间**，只通过系统调用通信，因此内核 GPLv2 不传染用户态 MIT 代码。

## 2. 目录布局（自研部分）

| 路径 | 职责 |
|---|---|
| `kernel/src/arch/x86_64/` | 架构相关：`schedule/`、`vmm/`、`smp/`、`interrupt/`、`lapic/`、`ioapic/`、`pci/`、`drivers/` |
| `kernel/src/mem/` | `pmm.cpp`（物理页）、`heap.cpp` / `new.cpp` / `new2.cpp`（SLUB） |
| `kernel/src/klib/algorithm/` | `art.c`（自适应基数树）、`hashmap.c`、`rbtree`、`queue` |
| `kernel/src/fs/` | `fc.cpp`（文件缓存）、`fd.cpp`（文件描述符）、`saf/`、`fatfs/`、`lwext4/` |
| `kernel/include/` | 与 `src` 镜像的头文件树 |
| `programs/` | 用户态：`desktop/`（WM+合成器）、`helloworld*/` |
| `lib/` | 用户态 libc（`stdc/`、`graphic/`、`base/`） |
| `ablib/` | 手写高频 libc 原语（memcpy/memset，AVX/AVX2 分发） |
| `res/scripts/` | QEMU 启动脚本（Linux / WSL / Windows） |

> `lwext4`、`fatfs`、`lwip`、`flanterm`、`limine-protocol` 是第三方移植，不属于自研。

## 3. 启动时序

入口 `x86_64_init()`（`init.cpp`）在 BSP 上按固定顺序执行，每步用 `InitFunc(name, expr)` 打点：

```text
Serial 输出就位
  → SSE
  → GDT / 写 KERNEL_GS_BASE / IA32_GS_MSR（per-CPU 指针）
  → IDT
  → FPU（不支持则 hcf）
  → PMM（物理页）
  → VMM（内核页表、五级映射）
  → SLAB → SLUB kmalloc（16..1024 B）→ SLUB 自检
  → ACPI / MADT
  → 屏蔽 8259 PIC（outb 0xff 到 0x21/0xa1），使能 ICMR
  → LAPIC / IOAPIC
  → PIT & RTC / HPET
  → BSP per-CPU 文件缓存 file_cache
  → smp_init()          ← 拉起所有 AP（见 smp.md）
  → IOAPIC::RemapIRQ(0→vec32)   ← 把 PIT GSI0 重定向到向量 32
  → RTC
  → simd_cpu_init(0)    ← XSave/AVX 特性探测
  → enable_smep_smap()
  → Schedule::Init()
  → InitCPUThread()     ← 造出 init 线程作为一切进程的祖先
  → syscall_init()      ← 挂 MSR_LSTAR / 系统调用入口
  → Dev / PCI 枚举（有 MCFG 走 ECAM，否则 DoPCIWithoutMCFG）
  → AHCI 驱动
  → PS/2 鼠标 / 键盘
  → ext4 挂在 "sata0" → /mp/
  → FrameBufferDevice::Init()
  → Schedule::Install() ← 把 idle 线程装好，调度器正式接管
  → sys_sysinfo_init()
  → NewProcess + NewThread("/mp/desktop.elf")   ← 桌面
  → NewProcess + NewThread("/mp/hw.elf")       ← 演示程序
  → sti；向 BSP 和其余 CPU 发 SCHED_VEC IPI，立刻触发一次调度
  → BSP 自身进入 hlt 死循环
```

### 三个容易踩的启动坑（源码里有注释记录）

1. **PIT 不重定向 = 系统冻结**。8259 PIC 被屏蔽后，如果不通过 IOAPIC 把 GSI0 重定向到向量 32，PIT 中断永远不到达 → `TimeSinceBootMS()` 冻结 → EEVDF 的 vruntime、量子反馈、所有超时全部停摆，表现为约 14 秒的启动卡顿和运行时鼠标/合成器冻住。
2. **AP 在 idle_thread 就绪前收到调度 tick 会空转**。`Schedule::Switch` 里对 `idle_thread/current_thread` 为空做了早退：重 arm  oneshot 定时器并直接 EOI，绝不解引用空指针。
3. **init 线程必须先 cli 再入队**。否则入队后立刻被中断调度挑出一个还没初始化完的线程运行，RIP 落到半填的上下文上直接崩。

## 4. 地址空间与安全

- 内核镜像由 Limine 给 HHDM（Higher Half Direct Map）偏移，启动早期打印 `HHDM OFFSET`。
- 支持 **KASLR**，构建后必须跑 `cd kernel && make kaslr-check` 确认镜像布局没有破坏该特性。
- 用户态/内核态地址空间隔离，`enable_smep_smap()` 打开 SMEP（不可执行用户页）与 SMAP（内核访问用户页需显式开关）。
