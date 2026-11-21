# 内核组件构建说明（Build Guide）

> 面向两类读者：想把内核/组件改动构建出来并验证的人，以及想新增一个内建组件或
> `.ko` 的人。文中所有命令、路径、段名、宏行为均对应本仓库当前的
> `kernel/Makefile`、`kernel/linker-x86_64.ld`、`kernel/include/kmod/*` 与顶层
> `Makefile`。
>
> 相关文档：[kernel-components.md](kernel-components.md)（组件清单/迁移状态）、
> [kernel-modules.md](kernel-modules.md)（kmod ABI/治理/加载器）、
> [kernel-dynlink.md](kernel-dynlink.md)（DynModule / modules.saf 统一模型与注入）。

## 0. 三条流水线

构建分成三条互相独立、最终被同一 ISO/SAF 打包的流水线：

1. **kernel 共享实现库**：`kernel/src` 的 Tier 0 + 尚未迁出的
   `kernel/modules/**` 实现代码，用内核编译参数编进镜像。内核导出
   260 个符号，其中 **197 个** `kernel.component.v1` 由
   `gen_component_exports.py` 从组件 `.ko` 的未定义符号生成；已被其他
   `.ko` 定义的符号会跳过，交给跨 .ko 解析。
2. **组件 .ko**：`kernel/modules/x86_64/components/**` 用
   `-DKMOD_COMPONENT_AS_KO=1` + `-fno-pic -mcmodel=large` 编成 40 个独立 ET_REL
   `.ko`；`KCOMPONENT_*` 映射到 `.kmod_manifest`/`.in`/`.out`。实现文件
   由 `modules/mcr/<component>.sources` 追加链接进同一个 `.ko`（AHCI、NVMe、
   xHCI/USB、PS/2 键鼠、e1000+lwIP 胶水、ext4(lwext4)、
   syscall.{file,proc,time,mem,random,dev,info}、display.fb、dev.core、block.core、
   fatfs 完整实现、lwIP 协议栈、file-cache core、ACPI 解析/MADT、RTC、
   fd_manager core、HPET、LAPIC/IOAPIC、sched_bench 已迁）。
3. **demo .ko + SAF**：`kernel/modules/loadable/*.cpp` 编成 18 个 demo/扩展
   `.ko`；`make modules-saf` 用 `res/scripts/make_modules_saf.py` 把 56 个
   `.ko` 打进一个 `bin-x86_64/modules.saf`。Limine 只加载这个 SAF；启动时
   `KmodSafIngest` 解析入池，`KmodBootLoad` 按依赖拓扑加载 43 个 BOOT 模块。

> 关键点：依赖拓扑排序发生在启动期，不是构建期。组件 .ko 的 manifest/.in/.out
> 和 trusted 标记在加载时校验；kernel 实现符号通过 .kout 表在重定位时回填，
> 已迁入其他 .ko 的符号由 `.out` 表 + `.kmod_dep_names` provider 先序解析。
>
> 构建顺序固定为：组件 .ko → `gen_component_exports.py` → 内核 → demo/SAF。
> 修改 kmod 结构体头后必须 clean build，否则部分目标文件仍是旧布局。

## 1. 构建入口与命令

### 1.1 顶层命令（推荐）

```bash
# 完整构建：clean + SAF 用户态打包 + 可选 x86mem + disk.img(缺失时) +
#          lib + programs + 内核 + .ko + ISO
make cm

# 增量构建：内核 + .ko + ISO（复用已存在的 disk.img / programs.saf）
make SkylineSystem-x86_64.iso -j4

# 只构建内核 + .ko，不出 ISO
make kernel
```

> 首次构建会自动拉取依赖：`kernel-deps` 目标执行 `kernel/get-deps`，
> `limine-binary/limine` 目标下载并编译 Limine 引导器。

### 1.2 内核目录命令

```bash
cd kernel
make SS_BUILD_DIR="$(cd .. && pwd)" components     # 40 组件 .ko（含实现文件）
make SS_BUILD_DIR="$(cd .. && pwd)"        # 内核镜像（Tier0 + 未迁实现库）
make SS_BUILD_DIR="$(cd .. && pwd)" modules        # 18 demo + 40 组件 .ko
make SS_BUILD_DIR="$(cd .. && pwd)" modules-saf    # 58 .ko -> bin-x86_64/modules.saf
make SS_BUILD_DIR="$(cd .. && pwd)" modules-check  # 58 .ko 重定位白名单自检
make SS_BUILD_DIR="$(cd .. && pwd)" DEBUG=1        # 非 KASLR 调试构建（GDB 地址=链接地址）
make SS_BUILD_DIR="$(cd .. && pwd)" kaslr-check    # 检查 32S 重定位是否清零
make clean                                          # 删除 bin-*/obj-*
```

> 直接 `cd kernel && make` 时必须显式传 `SS_BUILD_DIR=<仓库根绝对路径>`；
> 顶层 Makefile 会自动处理并 export。

### 1.3 产物位置

| 产物 | 路径 | 说明 |
|---|---|---|
| 内核镜像 | `kernel/bin-$(KARCH)/kernel` | PIE/KASLR（默认）或 DEBUG 构建；无 `.kbuiltin` 组件表 |
| demo `.ko` | `kernel/bin-$(KARCH)/modules/*.ko` | 18 个 ET_REL demo/扩展 |
| 组件 `.ko` | `kernel/bin-$(KARCH)/components/**/*.ko` | 40 个 ET_REL 组件 |
| SAF 归档 | `kernel/bin-$(KARCH)/modules.saf` | 58 个 .ko 的单一 Limine 模块 |
| ISO | `SkylineSystem-$(KARCH).iso` | 内核 + modules.saf + Limine + programs.saf |
| HDD 镜像 | `SkylineSystem-$(KARCH).hdd` | `make all-hdd` 生成 |
| 用户程序盘 | `disk.img`（ext4，2 GB） | desktop/DOOM 等，`make cm` 在缺失时创建 |

ISO 内部布局：`/boot/kernel`、`/boot/modules.saf`、`/boot/limine/limine.conf`、
`/boot/programs.saf`、`/EFI/BOOT/BOOT*.EFI`。

## 2. 组件 .ko（原 Tier 1）如何构建

### 2.1 源文件发现：`find_sources` / `find_module_sources` / 组件 KO 列表

`kernel/Makefile` 用两条递归 `find` 规则收集源文件（无需手工列文件）：

```make
# Tier 0：src/**（排除 arch） + src/arch/$(KARCH)/
define find_sources
  ... find src -type f -name "*.<ext>" -not -path "*/arch/*" ...
  ... find src/arch/$(KARCH) -type f -name "*.<ext>" ...
endef

# 共享实现库：排除 components/ 与 dosvm/（它们走独立 .ko）
define find_module_sources
  ... find modules/common -type f -name "*.<ext>" -not -path "*/components/*" ...
  ... find modules/$(KARCH) -type f -name "*.<ext>" \
        -not -path "*/components/*" -not -path "*/dosvm/*" ...
endef

# 组件壳 .ko 源（40 个）
KMOD_COMPONENT_SRCS := $(shell find modules/$(KARCH)/components -name "*.cpp") \
                       $(wildcard modules/$(KARCH)/dosvm/*.cpp)
```

扩展名覆盖 `.c`、`.cpp`、`.S`、`.asm`。对象路径约定：

- `kernel/src/.../foo.cpp` → `obj-$(KARCH)/.../foo.cpp.o`
- `kernel/modules/common/fs/fc.cpp` → `obj-$(KARCH)/modules/common/fs/fc.cpp.o`
- `kernel/modules/x86_64/components/fs/fs_vfs.cpp` →
  `obj-$(KARCH)/component-ko/x86_64/components/fs/fs_vfs.cxx.o` →
  `bin-$(KARCH)/components/x86_64/components/fs/fs_vfs.ko`

对应的编译规则分别打印 `C++`/`[MOD-C++]` 等前缀。新增文件不需要改 Makefile，
只要放在上述目录内。

### 2.2 编译参数

共享实现库与 Tier 0 共用 `KCFLAGS/KCPPFLAGS`；组件壳 .ko 用
`KMOD_CXXFLAGS + -DKMOD_COMPONENT_AS_KO=1`（`-fno-pic -fno-pie -mcmodel=large`，
绝对地址重定位）。共享实现库的编译参数：

- `clang/clang++ -target x86_64-unknown-none`（Clang target，freestanding）
- `-nostdinc -ffreestanding -fno-stack-check -fno-lto`
- `-ffunction-sections -fdata-sections`（配合链接期 `--gc-sections`）
- `-O2 -g3`，`-Wall -Wextra`
- Release：`-fPIE` + 链接 `-pie`（Limine KASLR）；`DEBUG=1`：非 PIE，地址固定
- include 路径：`kernel/include`、`ablib`、`lib/include`、`fakestdlib` 等

### 2.3 组件宏在编译期生成的段（AS_KO 模式）

一个组件是一个 TU，通常位于
`kernel/modules/x86_64/components/<类别>/<组件>.cpp`，形如：

```cpp
#include <kmod/kmod_component.h>

KCOMPONENT_BEGIN(fs_vfs, "kernel.fs.vfs", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_FS | KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC),
                 0, "VFS core: fs registry + path resolve + fd file ops",
                 comp_fs_vfs_init, comp_fs_vfs_exit, 0, 0, 1000u, 200u)
KCOMPONENT_IMPORT_FN(fs_vfs, kprintf_, int, (const char *, ...),
                     "kernel", "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(fs_vfs, "kernel.dev.core", dev_core)   /* 纯拓扑依赖 */
KCOMPONENT_EXPORT_CONTRACT(fs_vfs, KMOD_CONTRACT_VFS_V1, g_vfs_ops, 1)
KCOMPONENT_END(fs_vfs)
```

在 `-DKMOD_COMPONENT_AS_KO=1` 下，`KCOMPONENT_*` 被映射成模块宏，产生：

| 生成物 | 所在段 | 作用 |
|---|---|---|
| `static kmod_manifest_t` | `.kmod_manifest` | 名称/版本/类别/flags/能力/生命周期回调/预算 |
| 每条 `kmod_import_t` / `kmod_export_t` | `.in` / `.out` | 该组件的导入/导出条目 |
| 无 `.kbuiltin.*` 段 | — | 内核镜像里不再有静态组件表 |

- 所有条目带 `__attribute__((used, aligned(8)))`，避免 `--gc-sections` 回收；
- 组件 .ko 使用 `-mcmodel=large`，直接引用的 kernel 实现符号由
  `component_exports.cpp` 的 `kernel.component.v1` 表解析；
- 组件 .ko 的 syscall 导出指向 kernel 镜像内的实现函数，使用
  `KMOD_EXF_EXTERNAL` 标记，loader 跳过"导出地址必须在本镜像内"的检查。

### 2.4 链接：描述符表与内核导出表

内核链接命令：

```bash
ld.lld <obj...> [x86mem/memops.o] -nostdlib -static -z max-page-size=0x1000 \
    -gc-sections -Bsymbolic -T linker-$(KARCH).ld [-pie] -o bin-x86_64/kernel
# release 构建追加 -pie（配合编译期 -fPIE，Limine KASLR 生效）；
# DEBUG=1 构建不加 -pie，地址固定便于 GDB。
```

`kernel/linker-x86_64.ld` 仍保留 `.kbuiltin.*` 的 KEEP 与
`__kbuiltin_desc_start/end`（向后兼容/宿主测试），但组件壳不再编进内核，
所以该区间为空（start == end）。`.kout` 是真正的导出表：

```ld
.data : {
    *(.data .data.*)
    KEEP(*(.kbuiltin.in.*)) / KEEP(*(.kbuiltin.out.*)) / KEEP(*(.kbuiltin.desc))
    . = ALIGN(8); __kout_start = .; KEEP(*(.kout)); __kout_end = .;
} :data
```

- `.kout` 当前 260 项：mem/log/epoch/libc/time/klib/sync/algo/dynlink
  + 197 项 kernel.component.v1；
- 197 项由 `res/scripts/gen_component_exports.py` 从 40 个组件 .ko 的未定义
  符号生成到 `kernel/src/arch/x86_64/kmod/component_exports.cpp`；已被其他
  `.ko` 定义的符号会被跳过（例如 HID 键盘回调、`keyboard_feed_byte`），
  交给跨 .ko `.out` 解析；
- 链接后建议执行 `kaslr-check`：`readelf -r` 中不允许残留 `R_X86_64_32/32S`。

### 2.5 启动期：SAF 如何变成组件图

```text
KmodInit()                 # 注册表/能力/座位/syscall 契约表/KdllInit
KmodBootIngest()           # 扫描 Limine module: /boot/modules.saf
  └─ KmodSafIngest()       # 解析 root folder，58 个 .ko 入池
       └─ kernel.* → trusted=true（保留名/能力/固定 syscall 槽）
       └─ vendor.* → trusted=false（STRICT 能力策略）
KmodBootLoad()             # 43 个 BOOT 模块
  └─ KmodGovLoad() 逐模块：依赖闭包 → 拓扑排序 → bind/apply/init
       └─ provider="kernel" 查 .kout（contract/ABI/size 校验）
       └─ provider=组件 .ko 查其 .out
```

串口日志（正常）：

```text
[kmod] saf: 58 .ko accepted, 0 rejected (size=3768952)
[kmod] loaded kernel.arch.acpi@1.0.0 ...（38 条 kernel.*）
[kmod] boot load: 43 requested, 43 ready, 0 failed
```

失败时（坏 manifest/契约、缺 provider、环、init 失败、能力拒绝）会整批回滚。

## 3. relocatable DynModule（原 Tier 2 `.ko`）如何构建

### 3.1 源码与目标

- 源码：`kernel/modules/loadable/*.cpp`（每个文件一个模块，不递归）。
- 编译：`obj-$(KARCH)/loadable/<name>.cxx.o`。
- 链接：`ld.lld -m elf_x86_64 -r` → `bin-$(KARCH)/modules/<name>.ko`（ET_REL，可重定位）。
- 平台：当前仅 x86_64；其它架构的 `modules` 目标打印 skipped。

### 3.2 专用编译 flags（不能与内核镜像混用）

```make
KMOD_CXXFLAGS := -std=gnu++17 -O2 -g -target x86_64-unknown-none \
    -fno-pic -fno-pie -mcmodel=large \
    -mno-red-zone -mno-sse -mno-sse2 -mno-80387 -mno-mmx \
    -fno-stack-protector -fno-stack-check -fno-lto \
    -fno-exceptions -fno-rtti -ffreestanding -nostdinc \
    -I kernel/include -isystem ablib/freestndchdrs/x86_64/include
KMOD_LDFLAGS := -m elf_x86_64 -r
```

原因：`.ko` 走"绝对地址 + 加载期回填"模型，必须 `-mcmodel=large` 且非 PIE；
内核镜像走 `-fPIE -mcmodel=kernel` 的 KASLR 模型。两套 flags 刻意分离，
混用会导致加载器拒绝的重定位类型。

### 3.3 宏生成的段

`kernel/include/kmod/kmod_module.h` 的宏生成：

- `.in`：`kmod_import_t` 数组（provider/symbol/contract/slot/abi 区间/flags）
- `.out`：`kmod_export_t` 数组（FUNC/DATA/CONTRACT/SEAT/SYSCALL/NAMESPACE）
- `.kmod_manifest`：256B `kmod_manifest_t`（含 init/exit/quiesce/drain 与预算）

所有段内对象必须非 const + `used` + 8 字节对齐，否则加载器按段大小
`size / sizeof(entry)` 解析时会失败。

### 3.4 构建后自检：`modules-check`

```bash
make -C kernel modules-check
# == kmod reloc check (allowed: R_X86_64_64/PC32/PLT32/32/32S) ==
[OK]   bin-x86_64/components/x86_64/components/arch/arch_acpi.ko
[OK]   bin-x86_64/modules/demo_hello.ko
...（56 个）
kmod reloc check OK
```

它用 `readelf -rW` 检查每个 `.ko` 的重定位类型是否都落在加载器白名单内。
加载器在运行时还会再次做结构/边界/manifest/符号解析校验，非法镜像整体拒绝。

### 3.5 ISO 打包与启动加载

顶层 `Makefile` 的 ISO 目标：

```bash
cp kernel/bin-$(KARCH)/kernel          iso_root/boot/
cp kernel/bin-$(KARCH)/modules.saf     iso_root/boot/modules.saf
cp res/limine.conf                     iso_root/boot/limine/
cp limine-binary/...                   iso_root/boot/limine/  iso_root/EFI/BOOT/
cp programs/programs.saf               iso_root/boot/
xorriso -as mkisofs ... -o SkylineSystem-x86_64.iso
./limine-binary/limine bios-install SkylineSystem-x86_64.iso
```

启动路径：

```text
Limine 读取 res/limine.conf 的单个 module_path:
      boot():/boot/modules.saf
  → main.cpp GetBootModuleCount/Info 取得 SAF 原始镜像
  → KmodBootIngest() 识别 .saf，KmodSafIngest 解析 root folder
       → 每个 .ko Inspect + 登记到镜像池（kernel.* = trusted）
  → KmodBootLoad() 加载 41 个 KMOD_MF_BOOT 模块（依赖拓扑）
  → KmodElfLoadImage() 解析 .in/.out/.kmod_manifest、应用重定位，
      未定义符号按 .kout（含 kernel.component.v1）解析
  → governance：契约/版本/能力/依赖/回填/init（失败逆序回滚）
  → 15 个非 BOOT demo 留在池中，供 KmodLoadVersion / 热替换 / selftest 使用
```

注意：组件 .ko 与 demo .ko 都通过 `modules-saf` 自动进 SAF；不再需要逐项
修改 limine.conf。新增组件壳后只需重新 `make modules modules-saf`。

## 4. 组件 .ko vs demo .ko：差异速查

| 维度 | 组件 .ko（原 Tier 1） | demo/扩展 .ko（原 Tier 2） |
|---|---|---|
| 源码 | `modules/<arch>/components/**` + `dosvm/` | `modules/loadable/*.cpp` |
| 编译 | `KMOD_CXXFLAGS + -DKMOD_COMPONENT_AS_KO=1` | `KMOD_CXXFLAGS` |
| 链接 | `ld.lld -r`（ET_REL） | `ld.lld -r`（ET_REL） |
| 段 | `.kmod_manifest/.in/.out`（KCOMPONENT_* 映射） | 同样三段 |
| 装载 | modules.saf，trusted；43 个 BOOT 模块 | modules.saf；2 个 BOOT，其余按需 |
| 名称 | `kernel.*`（trusted 放行） | `vendor.*`（STRICT 白名单） |
| 能力 | 按清单直接授予 | STRICT 交集；未知模块拒绝 |
| syscall | 可注册固定槽 0..63 + 动态槽 | 动态槽/可 SWAPPABLE 覆写 |
| 卸载 | 拒绝（trusted 永不卸载） | 引用计数为 0 时 `KmodUnload` |
| 注入 | `KdllInject` 与普通 .ko 相同 | `KdllInject` |

## 5. 新增/修改组件的工作流

### 5.1 新增一个内建组件

1. 实现：把驱动/FS/服务实现放到
   `kernel/modules/common/<area>/` 或 `kernel/modules/<arch>/<area>/`。
2. 组件壳：在 `kernel/modules/<arch>/components/<category>/`
   新增一个 TU，写 `KCOMPONENT_BEGIN/END`：
   - 唯一 ID token 与组件名（如 `kernel.fs.myfs`）
   - 类别/flags/所需能力/描述
   - `KCOMPONENT_IMPORT*` 声明依赖的私有契约/provider 符号
   - `KCOMPONENT_EXPORT*` 声明对外导出的函数/数据/契约/座位/syscall
   - 生命周期回调（init 返回 `KMOD_OK` 才 READY；exit/quiesce/drain 可空）
3. 构建：无需改 Makefile；`make -C kernel modules modules-saf` 会自动把新
   组件壳编成 `.ko` 并打进 modules.saf。
4. 新实现符号：如果组件壳调用了新的 kernel 实现函数/数据，运行
   `python3 res/scripts/gen_component_exports.py` 更新
   `kernel/src/arch/x86_64/kmod/component_exports.cpp`，否则加载会报
   unresolved symbol。
5. 验证：串口出现 `[kmod] loaded kernel.xxx` 与拓扑顺序，再跑
   `kmod_smoke.ps1`；文档在 [kernel-components.md](kernel-components.md) 补一行。

### 5.2 新增一个 `.ko`

1. 在 `kernel/modules/loadable/` 新增 `.cpp`（一个文件 = 一个模块）。
2. 用 `KMOD_IMPORT*` / `KMOD_EXPORT*` / `KMOD_MANIFEST` 声明；所需能力在
   `kernel/src/arch/x86_64/kmod/capability.cpp` 的 STRICT 可信清单登记。
3. `make -C kernel modules modules-check`。
4. `make modules-saf` 会自动把新 .ko 收进 modules.saf；不需要改
   limine.conf。若 `KMOD_MF_BOOT` 启动自动加载，否则由 selftest/`KdllInject`
   按名加载。
5. 更新 `docs/kernel-modules.md` 的演示模块表（如新增 demo）。

### 5.3 修改现有组件/头文件时

- 只改组件 `.cpp`：增量 `make SkylineSystem-x86_64.iso -j4` 即可。
- 改头文件：注意 `kernel/Makefile` 只为 `.c/.S` 生成 header deps（`.d`），
  C++ 头文件依赖未跟踪。修改共享头后请 `make -C kernel clean`（或删除相关
  `obj-<arch>/` 对象）再构建，否则可能出现"改头文件不生效"的假象。
- 改 `KCFLAGS/KCPPFLAGS`/`DEBUG` 等构建参数：必须 clean 重建。
- 改组件宏/段名：同时检查 `linker-$(KARCH).ld` 的 KEEP 段与加载器段名常量
  （`kmod_abi.h` 的 `KMOD_SEC_*`）。

## 6. 验证门禁（构建后跑什么）

```bash
# 1) 内核/组件构建 + .ko 契约
make SkylineSystem-x86_64.iso -j4
make -C kernel modules-check                 # 58/58 [OK]

# 2) 宿主测试（加载器/拓扑/能力/治理/组件模型/共享服务）
cd tests && make -B kmod && ./bin/kmod_test          # PASS 7/7
make -B kmod-asan && ./bin/kmod_test_asan            # ASAN 0 报告

# 3) QEMU 串口门禁（Windows PowerShell）
pwsh -File res/scripts/test/kmod_smoke.ps1 `
     -Root C:\ZSY\SkylineSystem -WaitSeconds 200 -MinBuiltins 30
# 期望：KMOD SMOKE: PASS
#       KMOD_SELFTEST PASS 14/14
#       saf: 58 .ko accepted / kernel exports: 260
#       boot load: 43 requested, 43 ready, 0 failed
```

## 7. 常见构建问题

| 现象 | 原因 / 处理 |
|---|---|
| `Please run the ./get-deps script first` | 首次构建未拉取依赖：`make kernel-deps` 或直接 `make cm` |
| 直接 `cd kernel && make` 报找不到 `kernel/include/...` | 缺 `SS_BUILD_DIR`：用 `make SS_BUILD_DIR="$(cd .. && pwd)"` |
| 改了头文件但行为没变 | C++ header deps 未跟踪：`make -C kernel clean` 后重建 |
| `.ko` 加载失败 `UNRESOLVED` | 组件壳引用了新的 kernel 实现符号：运行 `python3 res/scripts/gen_component_exports.py` 后重建；demo 则改用 `.in` 导入 |
| `.ko` 加载失败 `reloc`/`modules-check FAIL` | 编译 flags 混用（误用内核 PIE flags）或新增重定位类型；使用 `KMOD_CXXFLAGS` |
| 新组件没出现 | 源码不在 `modules/<arch>/components/`；或没执行 `make modules-saf` |
| 启动 `prepare ... bad name` | 组件 .ko 未被 SAF 标记 trusted（manifest 名不是 kernel.*），或普通 .ko 冒用保留名 |
| 启动 `EXTERNAL export not allowed` | 普通 .ko 的导出指向 kernel 镜像；只有 trusted 组件可使用 `KMOD_EXF_EXTERNAL` |
| 组件 init 失败导致启动失败 | 查看 `[kmod] load ... transaction failed ... rollback`，确认依赖/能力/契约条件 |
| 新 `.ko` 不在池中 | 未执行 `make modules-saf`（SAF 才是 Limine 加载入口） |
| STRICT 下 `.ko` 被拒 | 未在 `capability.cpp` 可信清单登记能力 |

## 8. 关键文件速查

| 文件 | 作用 |
|---|---|
| `Makefile`（根） | `cm`/`kernel`/ISO 打包/run/clean |
| `kernel/Makefile` | `find_sources`/`find_module_sources`、编译/链接规则、`.ko`、`modules-check` |
| `kernel/linker-x86_64.ld` | `.kbuiltin.in/out/desc`、`.kout` 的 KEEP 与边界符号 |
| `kernel/include/kmod/kmod_abi.h` | ABI 版本、段名、结构体、Tier/flags/能力/契约常量 |
| `kernel/include/kmod/kmod_component.h` | 组件 `KCOMPONENT_*` 宏（默认 static；AS_KO 时映射为 .kmod_manifest/.in/.out） |
| `kernel/include/kmod/kmod_module.h` | `.ko` 作者宏（生成 `.in/.out/.kmod_manifest`） |
| `kernel/include/kmod/kmod_services.h` | Tier0 共享服务 ops 契约（klib/sync/algo） |
| `kernel/src/arch/x86_64/kmod/builtin.cpp` | 描述符扫描/校验/拓扑加载/回滚 |
| `kernel/src/arch/x86_64/kmod/saf.cpp` | SAF 解析 + trusted 入库 |
| `kernel/src/arch/x86_64/kmod/component_exports.cpp` | 生成的 `kernel.component.v1` 导出表（197 项） |
| `res/scripts/gen_component_exports.py` | 从组件 .ko 未定义符号重新生成导出表 |
| `res/scripts/make_modules_saf.py` | 58 个 .ko -> modules.saf |
| `kernel/src/arch/x86_64/kmod/kmod_core.cpp` | `KmodInit`/`KmodBootIngest`/`KmodBootLoad` 入口 |
| `kernel/src/arch/x86_64/kmod/elf_loader.cpp` | ET_REL 加载器（`.ko`） |
| `kernel/src/arch/x86_64/kmod/governance.cpp` | 加载事务/依赖图/热替换/卸载 |
| `kernel/modules/x86_64/components/` | 40 个组件壳（13 类子目录） |
| `kernel/modules/loadable/` | 18 个 `.ko` 演示/扩展源码 |
| `res/limine.conf` | 单一 `module_path: boot():/boot/modules.saf` |
| `res/scripts/test/kmod_smoke.ps1` | QEMU 串口门禁 |
