# kernel/modules — 内核组件模块树

> 本目录是全模块化重构后的内核组件归属地：`kernel/src` 只保留 Tier0 固定核心，
> 其余组件实现（驱动/文件系统/网络/分区/平台/syscall 家族等）全部在这里。
>
> Tier1（内建）与 Tier2（`.ko`）已合并为统一 **DynModule**：`builtin/` 下的
> 组件壳现在各自编译成独立 `.ko`，`loadable/` 是 demo/扩展；55 个 `.ko` 全部
> 打进一个 **modules.saf**，Limine 只加载它。 `KdllInject`（`kernel.dynlink.v1`）
> 负责运行期注入。
>
> 统一模型见 [docs/kernel-dynlink.md](../../docs/kernel-dynlink.md)；
> 机制与治理模型见 [docs/kernel-modules.md](../../docs/kernel-modules.md)、
> 组件台账见 [docs/kernel-components.md](../../docs/kernel-components.md)。

## 目录结构

```
kernel/modules/
├── builtin/                     # 组件/共享实现：组件壳独立成 .ko，实现代码进 kernel
│   │                            #   manifest/.in/.out ABI + 拓扑图 + 能力/生命周期治理；
│   │                            #   组件 .ko 由 KmodSafIngest/KmodBootLoad 从 SAF 加载
│   ├── common/                  # 所有架构共享的组件实现（原先 kernel/src 非 arch 目录）
│   │   ├── acpi/                #   ACPI/MADT 表解析（kernel.arch.acpi）
│   │   ├── drivers/             #   块/网络/USB/输入/显示设备驱动实现
│   │   ├── fs/                  #   VFS/FAT/ext4(lwext4)/SAF 文件系统实现
│   │   ├── net/                 #   lwIP 协议栈
│   │   ├── partition/           #   MBR/GPT 分区管理
│   │   └── smbios/              #   SMBIOS 表解析
│   └── x86_64/                  # x86_64 专属组件实现（平台/驱动/syscall 家族）
│       ├── components/          #   37 个 KCOMPONENT_BEGIN/END 组件壳 -> 独立 .ko
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
└── loadable/                    # 17 个 demo/扩展 .ko 源码
                                 #   make modules 产出 kernel/bin-x86_64/modules/*.ko；
                                 #   make modules-saf 把 55 个 .ko 打成 modules.saf；
                                 #   KMOD_MF_BOOT 由 KmodBootLoad() 启动加载，
                                 #   其余可由 KdllInject 运行期注入（demo_inject/late）
```

## 共享服务（klib）给每个模块

Tier0 的 klib 公共工具对所有模块开放，两种用法：

1. 直接符号：C 链接函数（`strlen/strcmp/snprintf_/hashmap_*/art_*/spinlock_*`、
   `kcalloc/krealloc/kmalloc_aligned` 等）已在内核 `.kout` 导出表；`.ko` 直接
   `extern "C"` 声明并调用即可，加载器按 ELF 未定义符号名解析，无需 `.in`。
2. 契约 ops：`#include <kmod/kmod_module.h>` 后用
   `KMOD_IMPORT_KLIB(klib)` / `KMOD_IMPORT_SYNC(sync)` / `KMOD_IMPORT_ALGO(algo)`
   导入版本化 ops 结构（`kernel.klib.v1` / `kernel.sync.v1` / `kernel.algo.v1`），
   治理层校验 contract/abi/结构尺寸；适用于 bitmap/qsort/RND/中断状态/容器等
   C++ 链接或不透明句柄工具。

所有组件 .ko/demo .ko 共享同一套导出面；组件壳直接调用的 driver/FS/net/sched
实现函数通过 `kernel.component.v1`（component_exports.cpp）导出。
需要 LoadLibrary/GetProcAddress 时用 `KMOD_IMPORT_DYNLINK` 导入注入服务。
完整契约表与 ABI 规则见 [docs/kernel-modules.md §4.1](../../docs/kernel-modules.md)，
示例见 `modules/loadable/demo_klib_v1.cpp`。

## 构建

```bash
make -C kernel                 # kernel 镜像（Tier0 + 共享实现库）
make -C kernel modules         # 17 demo + 38 组件 .ko
make -C kernel modules-saf     # 55 .ko -> bin-x86_64/modules.saf
make -C kernel modules-check   # 55 .ko 段/重定位白名单自检
```

组件壳用 `-DKMOD_COMPONENT_AS_KO=1` 编译成独立 .ko（`KCOMPONENT_*` 映射到
`.kmod_manifest`/`.in`/`.out`），不再生成 `.kbuiltin.*` 静态段。新增组件壳
调用了新的 kernel 实现符号时，重新运行
`python3 res/scripts/gen_component_exports.py`。

## 迁移约束

- `kernel/src` 只放 Tier0（boot/GDT/IDT/中断/页表/调度核心/syscall 入口/
  kmod 加载与治理/klib/mem/print/elf/flanterm/start/main）。
- 新增组件实现应放 `modules/builtin/<arch>/` 或 `modules/builtin/common/`，
  并在 `modules/builtin/<arch>/components/` 增加对应的 KCOMPONENT 壳。
- 头文件仍集中在 `kernel/include/`（Tier0 机制契约 + 组件接口）；组件私有头
  后续可按 `modules/<area>/include/` 继续下沉（见 docs/kernel-components.md 路线图）。
