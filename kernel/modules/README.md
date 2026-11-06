# kernel/modules — 内核组件模块树

> 本目录是**全模块化重构**后的内核组件归属地：`kernel/src` 只保留 Tier0 固定核心，
> 其余组件实现（驱动/文件系统/网络/分区/平台/syscall 家族等）全部在这里。
>
> 机制与治理模型见 [docs/kernel-modules.md](../../docs/kernel-modules.md)、
> 组件台账见 [docs/kernel-components.md](../../docs/kernel-components.md)。

## 目录结构

```
kernel/modules/
├── builtin/                     # Tier1 内建组件：静态链接进内核镜像，但与 .ko 同一套
│   │                            #   manifest/.in/.out ABI + 拓扑图 + 能力/生命周期治理，
│   │                            #   由 KmodBootComponents() 在启动期按依赖拓扑初始化
│   ├── common/                  # 所有架构共享的组件实现（原先 kernel/src 非 arch 目录）
│   │   ├── acpi/                #   ACPI/MADT 表解析（kernel.arch.acpi）
│   │   ├── drivers/             #   块/网络/USB/输入/显示设备驱动实现
│   │   ├── fs/                  #   VFS/FAT/ext4(lwext4)/SAF 文件系统实现
│   │   ├── net/                 #   lwIP 协议栈
│   │   ├── partition/           #   MBR/GPT 分区管理
│   │   └── smbios/              #   SMBIOS 表解析
│   └── x86_64/                  # x86_64 专属组件实现（平台/驱动/syscall 家族）
│       ├── components/          #   37 个 KCOMPONENT_BEGIN/END 组件壳（按类别分子目录）
│       │   ├── arch/            #     acpi/apic/irq/smp 平台组件
│       │   ├── time/            #     pit/hpet/rtc 时间组件
│       │   ├── sched/           #     sched core/install + 调度策略座位组件
│       │   ├── alloc/           #     分配策略座位组件
│       │   ├── dev/ pci/ block/ #     设备核心 / PCI 核心+枚举 / 块设备核心
│       │   ├── drivers/         #     ahci/nvme/xhci/e1000/ps2 键鼠 驱动组件
│       │   ├── fs/              #     vfs/ext4/fatfs/rootfs/文件缓存组件
│       │   ├── net/             #     netdev 注册表 + lwIP 协议栈组件
│       │   ├── display/ user/   #     帧缓冲 / 用户态进程创建
│       │   └── syscall/         #     7 个 syscall 实现家族组件
│       ├── dev/                 #   PCI 枚举/中断路由等设备核心实现
│       ├── drivers/             #   HPET 等 x86 平台驱动实现
│       ├── ioapic/ lapic/ pit/ rtc/ smp/   # 平台组件实现（时间/SMP/中断控制器）
│       └── syscall/             #   全部 syscall 实现家族（固定槽经治理表注册）
└── loadable/                    # Tier2 可加载 .ko 演示模块源码
                                 #   由 `make -C kernel modules` 编译为
                                 #   kernel/bin-x86_64/modules/*.ko，随 ISO 下发，
                                 #   KmodBootLoad() 按 manifest 的 KMOD_MF_BOOT 加载
```

## 共享服务（klib）给每个模块

Tier0 的 klib 公共工具对**所有模块**开放，两种用法：

1. **直接符号**：C 链接函数（`strlen/strcmp/snprintf_/hashmap_*/art_*/spinlock_*`、
   `kcalloc/krealloc/kmalloc_aligned` 等）已在内核 `.kout` 导出表；`.ko` 直接
   `extern "C"` 声明并调用即可，加载器按 ELF 未定义符号名解析，无需 `.in`。
2. **契约 ops**：`#include <kmod/kmod_module.h>` 后用
   `KMOD_IMPORT_KLIB(klib)` / `KMOD_IMPORT_SYNC(sync)` / `KMOD_IMPORT_ALGO(algo)`
   导入版本化 ops 结构（`kernel.klib.v1` / `kernel.sync.v1` / `kernel.algo.v1`），
   治理层校验 contract/abi/结构尺寸；适用于 bitmap/qsort/RND/中断状态/容器等
   C++ 链接或不透明句柄工具。

内建组件与可加载 `.ko` 共享同一套导出面；内建组件因静态链接也可直接调用 klib。
完整契约表与 ABI 规则见 [docs/kernel-modules.md §4.1](../../docs/kernel-modules.md)，
示例见 `modules/loadable/demo_klib_v1.cpp`。

## 构建

```bash
make -C kernel            # 内建组件（modules/builtin/**）编入内核镜像
make -C kernel modules    # 可加载演示模块（modules/loadable/*.cpp -> *.ko）
make -C kernel modules-check   # .ko 段/重定位白名单自检
```

内建组件源码不单独编译成 `.ko`：它们与 Tier0 核心同属内核可信构建产物，
但通过 `KCOMPONENT_*` 宏生成与 `.ko` 完全相同的 manifest/.in/.out 线格式，
统一进入拓扑图、能力策略、生命周期与热替换治理；`.ko` 可导入内建组件导出的
私有契约，也可在治理事务中热替换内建组件实现（失败回滚恢复内建实现）。

## 迁移约束

- `kernel/src` 只放 Tier0（boot/GDT/IDT/中断/页表/调度核心/syscall 入口/
  kmod 加载与治理/klib/mem/print/elf/flanterm/start/main）。
- 新增组件实现应放 `modules/builtin/<arch>/` 或 `modules/builtin/common/`，
  并在 `modules/builtin/<arch>/components/` 增加对应的 KCOMPONENT 壳。
- 头文件仍集中在 `kernel/include/`（Tier0 机制契约 + 组件接口）；组件私有头
  后续可按 `modules/<area>/include/` 继续下沉（见 docs/kernel-components.md 路线图）。
