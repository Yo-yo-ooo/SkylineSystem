# 内核组件构建说明（Build Guide）

> 面向两类读者：**想把内核/组件改动构建出来并验证的人**，以及**想新增一个内建组件或
> `.ko` 的人**。文中所有命令、路径、段名、宏行为均对应本仓库当前的
> `kernel/Makefile`、`kernel/linker-x86_64.ld`、`kernel/include/kmod/*` 与顶层
> `Makefile`。
>
> 相关文档：[kernel-components.md](kernel-components.md)（组件模型/Tier 划分）、
> [kernel-modules.md](kernel-modules.md)（kmod ABI/治理/加载器）。

## 0. 一句话总览

构建分成两条**互相独立但产出被同一 ISO 打包**的流水线：

1. **内建组件流水线（Tier 1）**：`kernel/src` 的 Tier 0 与 `kernel/modules/builtin/**`
   的组件实现，用**同一套内核编译参数**编译；每个组件 TU 通过 `KCOMPONENT_*` 宏
   额外生成 `.kbuiltin.in.<ID>` / `.kbuiltin.out.<ID>` / `.kbuiltin.desc` 段；
   链接器把这些段收进内核镜像，启动时由 `KmodBootComponents()` **在运行时**建依赖图、
   拓扑排序并初始化。
2. **可加载模块流水线（Tier 2）**：`kernel/modules/loadable/*.cpp` 用**专门的
   freestanding/绝对地址 flags** 编译，`ld.lld -r` 生成 ET_REL `.ko`；ISO 打包后由
   Limine 作为模块下发，启动时 `KmodBootIngest/KmodBootLoad` 登记/加载，加载器
   回填 `.in` 导入并应用重定位。

> **关键点：组件的依赖拓扑排序发生在启动期，不是构建期。** 构建期只负责把
> manifest、导入/导出条目和描述符放进正确的段；`KmodBuiltinBootLoad()` 在启动时
> 读取描述符表并做 Kahn 拓扑排序。

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
make SS_BUILD_DIR="$(cd .. && pwd)"        # 内核镜像 + 内建组件（Tier0+Tier1）
make SS_BUILD_DIR="$(cd .. && pwd)" modules        # 15 个 .ko
make SS_BUILD_DIR="$(cd .. && pwd)" modules-check  # .ko 重定位白名单自检
make SS_BUILD_DIR="$(cd .. && pwd)" DEBUG=1        # 非 KASLR 调试构建（GDB 地址=链接地址）
make SS_BUILD_DIR="$(cd .. && pwd)" kaslr-check    # 检查 32S 重定位是否清零
make clean                                          # 删除 bin-*/obj-*
```

> 直接 `cd kernel && make` 时必须显式传 `SS_BUILD_DIR=<仓库根绝对路径>`；
> 顶层 Makefile 会自动处理并 export。

### 1.3 产物位置

| 产物 | 路径 | 说明 |
|---|---|---|
| 内核镜像 | `kernel/bin-$(KARCH)/kernel` | PIE/KASLR（默认）或 DEBUG 构建 |
| 可加载模块 | `kernel/bin-$(KARCH)/modules/*.ko` | 15 个 ET_REL `.ko` |
| ISO | `SkylineSystem-$(KARCH).iso` | 内核 + `.ko` + Limine + `programs.saf` |
| HDD 镜像 | `SkylineSystem-$(KARCH).hdd` | `make all-hdd` 生成 |
| 用户程序盘 | `disk.img`（ext4，2 GB） | desktop/DOOM 等，`make cm` 在缺失时创建 |

ISO 内部布局：`/boot/kernel`、`/boot/modules/*.ko`、`/boot/limine/limine.conf`、
`/boot/programs.saf`、`/EFI/BOOT/BOOT*.EFI`。

## 2. 内建组件（Tier 1）如何构建

### 2.1 源文件发现：`find_sources` 与 `find_module_sources`

`kernel/Makefile` 用两条递归 `find` 规则收集源文件（无需手工列文件）：

```make
# Tier 0：src/**（排除 arch） + src/arch/$(KARCH)/
define find_sources
  ... find src -type f -name "*.<ext>" -not -path "*/arch/*" ...
  ... find src/arch/$(KARCH) -type f -name "*.<ext>" ...
endef

# Tier 1：modules/builtin/common/** + modules/builtin/$(KARCH)/
define find_module_sources
  ... find modules/builtin/common -type f -name "*.<ext>" ...
  ... find modules/builtin/$(KARCH) -type f -name "*.<ext>" ...
endef
```

扩展名覆盖 `.c`、`.cpp`、`.S`、`.asm`。对象路径约定：

- `kernel/src/.../foo.cpp` → `obj-$(KARCH)/.../foo.cpp.o`
- `kernel/modules/builtin/common/fs/fc.cpp` → `obj-$(KARCH)/modules/builtin/common/fs/fc.cpp.o`
- `kernel/modules/builtin/x86_64/components/fs/fs_vfs.cpp` →
  `obj-$(KARCH)/modules/builtin/x86_64/components/fs/fs_vfs.cpp.o`

对应的编译规则分别打印 `C++`/`[MOD-C++]` 等前缀。**新增文件不需要改 Makefile**，
只要放在上述目录内。

### 2.2 编译参数：与 Tier 0 完全一致

内建组件与 Tier 0 共用 `KCFLAGS/KCPPFLAGS`：

- `clang/clang++ -target x86_64-unknown-none`（Clang target，freestanding）
- `-nostdinc -ffreestanding -fno-stack-check -fno-lto`
- `-ffunction-sections -fdata-sections`（配合链接期 `--gc-sections`）
- `-O2 -g3`，`-Wall -Wextra`
- Release：`-fPIE` + 链接 `-pie`（Limine KASLR）；`DEBUG=1`：非 PIE，地址固定
- include 路径：`kernel/include`、`ablib`、`lib/include`、`fakestdlib` 等

### 2.3 组件宏在编译期生成的段

一个组件是**一个 TU**，通常位于
`kernel/modules/builtin/x86_64/components/<类别>/<组件>.cpp`，形如：

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

宏展开后，这个 TU 在目标文件里产生三类东西：

| 生成物 | 所在段 | 作用 |
|---|---|---|
| `static kmod_manifest_t` | 普通数据段（`.data`） | 名称/版本/Tier/flags/能力/生命周期回调/预算 |
| BEGIN/END 哨兵 + 每条 `kmod_import_t` / `kmod_export_t` | `.kbuiltin.in.<ID>` / `.kbuiltin.out.<ID>` | 该组件的导入/导出条目，段内用哨兵界定范围 |
| `static kmod_builtin_raw_t` 描述符 | `.kbuiltin.desc` | 每组件一条：manifest 指针 + in/out 段边界指针 |

注意两个实现细节：

- 所有段内对象都带 `__attribute__((used, aligned(8)))`：**必须 used**，否则
  `-ffunction-sections/-fdata-sections + --gc-sections` 会把没有被代码直接引用的
  条目回收掉。
- 组件宏依赖唯一标识 token（示例中的 `fs_vfs`）生成**段名和变量名**：
  `.kbuiltin.in.fs_vfs` 等。**同一镜像内 token 与组件名必须唯一**，否则段/符号冲突
  或描述符重复。

### 2.4 链接：描述符表与内核导出表

内核链接命令：

```bash
ld.lld <obj...> [x86mem/memops.o] -nostdlib -static -z max-page-size=0x1000 \
    -gc-sections -Bsymbolic -T linker-$(KARCH).ld [-pie] -o bin-x86_64/kernel
# release 构建追加 -pie（配合编译期 -fPIE，Limine KASLR 生效）；
# DEBUG=1 构建不加 -pie，地址固定便于 GDB。
```

`kernel/linker-x86_64.ld` 在 `.data` 中显式保留并收集组件段：

```ld
.data : {
    *(.data .data.*)
    KEEP(*(.kbuiltin.in.*))
    KEEP(*(.kbuiltin.out.*))
    . = ALIGN(8);
    __kbuiltin_desc_start = .;
    KEEP(*(.kbuiltin.desc))
    __kbuiltin_desc_end = .;
    . = ALIGN(8);
    __kout_start = .;
    KEEP(*(.kout))          /* Tier0 内核导出表 */
    __kout_end = .;
} :data
```

- `.kbuiltin.desc` 必须 **KEEP**：它是组件管理器唯一的入口；`__kbuiltin_desc_start/end`
  由 `builtin.cpp` 用来遍历描述符表并计算组件数量。
- `.kout` 是 Tier0 导出表（当前 57 项：mem/log/epoch/libc/time/syscall/klib/sync/algo
  等）。它放在可写 `.data` 是因为 PIE 构建下函数/数据指针初始化会产生 RELATIVE
  重定位，由 Limine 在启动时应用。
- 链接后建议执行 `cd kernel && make SS_BUILD_DIR="$(cd .. && pwd)" kaslr-check`：
  `readelf -r` 中不允许残留 `R_X86_64_32/32S`。

### 2.5 启动期：构建产物如何变成组件图

`kernel/src/arch/x86_64/init.cpp` 中 Tier 0 收尾调用：

```text
KmodInit()                 # 注册表/能力/座位/syscall 契约表初始化
KmodBootComponents()       # == KmodBuiltinBootLoad()
  └─ KmodBuiltinInit()     # 读取 __kbuiltin_desc_start..end，逐个校验描述符
       └─ KmodModuleAllocBuiltin()   # 把 raw 描述符落成 kmod_module_t
            ├─ 遍历 .kbuiltin.in.<ID>  → imports[]（provider 边）
            ├─ 遍历 .kbuiltin.out.<ID> → exports[]（对外契约/函数）
            └─ 校验 BEGIN/END 哨兵、条目整除、manifest 字段
  └─ Kahn 拓扑排序（provider 先序；检测环/缺失依赖）
  └─ bind：provider="kernel" 查 .kout（contract/ABI/size 校验）；
          其它 provider 查已就绪内建组件的 exports
  └─ apply exports → 按拓扑序调用每个组件的 init → READY
```

串口日志（正常）：

```text
[kmod] builtin components: 37 declared
[kmod] builtin ready: kernel.arch.acpi@1.0.0 tier=1 caps=0x... imports=... exports=...
...（37 条）
[kmod] builtin graph: 37 component(s) ready (topo order)
```

失败时（坏 manifest/哨兵、缺 provider、环、init 失败、能力拒绝）会整批回滚，
组件图不会进入 READY。

## 3. 可加载模块（Tier 2 `.ko`）如何构建

### 3.1 源码与目标

- 源码：`kernel/modules/loadable/*.cpp`（每个文件一个模块，**不递归**）。
- 编译：`obj-$(KARCH)/loadable/<name>.cxx.o`。
- 链接：`ld.lld -m elf_x86_64 -r` → `bin-$(KARCH)/modules/<name>.ko`（ET_REL，可重定位）。
- 平台：当前**仅 x86_64**；其它架构的 `modules` 目标打印 skipped。

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

原因：`.ko` 走**"绝对地址 + 加载期回填"**模型，必须 `-mcmodel=large` 且非 PIE；
内核镜像走 `-fPIE -mcmodel=kernel` 的 KASLR 模型。两套 flags 刻意分离，
混用会导致加载器拒绝的重定位类型。

### 3.3 宏生成的段

`kernel/include/kmod/kmod_module.h` 的宏生成：

- `.in`：`kmod_import_t` 数组（provider/symbol/contract/slot/abi 区间/flags）
- `.out`：`kmod_export_t` 数组（FUNC/DATA/CONTRACT/SEAT/SYSCALL/NAMESPACE）
- `.kmod_manifest`：256B `kmod_manifest_t`（含 init/exit/quiesce/drain 与预算）

所有段内对象必须**非 const + `used` + 8 字节对齐**，否则加载器按段大小
`size / sizeof(entry)` 解析时会失败。

### 3.4 构建后自检：`modules-check`

```bash
make -C kernel modules-check
# == kmod reloc check (allowed: R_X86_64_64/PC32/PLT32/32/32S) ==
[OK]   bin-x86_64/modules/demo_hello.ko
...
kmod reloc check OK
```

它用 `readelf -rW` 检查每个 `.ko` 的重定位类型是否都落在加载器白名单内。
加载器在运行时还会再次做结构/边界/manifest/符号解析校验，非法镜像整体拒绝。

### 3.5 ISO 打包与启动加载

顶层 `Makefile` 的 ISO 目标：

```bash
cp kernel/bin-$(KARCH)/kernel          iso_root/boot/
cp kernel/bin-$(KARCH)/modules/*.ko    iso_root/boot/modules/
cp res/limine.conf                     iso_root/boot/limine/
cp limine-binary/...                   iso_root/boot/limine/  iso_root/EFI/BOOT/
cp programs/programs.saf               iso_root/boot/
xorriso -as mkisofs ... -o SkylineSystem-x86_64.iso
./limine-binary/limine bios-install SkylineSystem-x86_64.iso
```

启动路径：

```text
Limine 读取 res/limine.conf 的 module_path 列表
  → main.cpp GetBootModuleCount/Info 取得 .ko 原始镜像
  → KmodBootIngest() 逐镜像 Inspect + 登记到镜像池（不加载/不执行）
  → KmodBootLoad() 对 flags 含 KMOD_MF_BOOT 的模块执行完整加载事务
  → KmodElfLoadImage() 解析 .in/.out/.kmod_manifest、应用重定位、
     未定义符号按 .kout 导出表解析
  → governance：契约/版本/能力/依赖/回填/init（失败逆序回滚）
  → 非 BOOT 模块留在池中，供 KmodLoadVersion / 热替换 / selftest 使用
```

**注意**：新增 `.ko` 后必须同时把 `module_path: boot():/boot/modules/<name>.ko`
加进 `res/limine.conf`，否则它不会进入镜像池（模块源码构建成功 ≠ 能从 ISO 加载）。

## 4. 内建组件 vs 可加载模块：差异速查

| 维度 | Tier 1 内建组件 | Tier 2 `.ko` |
|---|---|---|
| 源码 | `modules/builtin/{common,<arch>}/**` + `components/**` | `modules/loadable/*.cpp` |
| 编译参数 | 与 Tier 0 相同（PIE/KASLR） | `-fno-pie -mcmodel=large` |
| 链接 | 与内核一起 `ld.lld ... -pie -T linker.ld` | `ld.lld -r`（ET_REL） |
| 段 | `.kbuiltin.in/out/desc` | `.in/.out/.kmod_manifest` |
| 装入时机 | 内核镜像内，启动即存在 | ISO 下发；BOOT 模块启动加载，其余按需 |
| 地址 | 链接期确定（PIE 重定位） | 加载期分配 + 重定位回填 |
| 依赖 | 运行时拓扑图（builtin 图） | 同一拓扑图（含 builtin provider） |
| 可替换 | 机制支持节点热替换（builtin→.ko takeover）；真实 `kernel.*` 组件受 `.ko` 名称保留校验保护，当前不可被外部同名替换 | 可热替换（同 name/version 池；需 MF_HOTREPLACE + CAP_HOT_REPLACE） |
| 卸载 | 仅热替换（builtin 不可直接 unload） | 引用计数为 0 时 `KmodUnload` |

## 5. 新增/修改组件的工作流

### 5.1 新增一个内建组件

1. **实现**：把驱动/FS/服务实现放到
   `kernel/modules/builtin/common/<area>/` 或 `kernel/modules/builtin/<arch>/<area>/`。
2. **组件壳**：在 `kernel/modules/builtin/<arch>/components/<category>/`
   新增一个 TU，写 `KCOMPONENT_BEGIN/END`：
   - 唯一 ID token 与组件名（如 `kernel.fs.myfs`）
   - Tier/flags/所需能力/描述
   - `KCOMPONENT_IMPORT*` 声明依赖的私有契约/provider 符号
   - `KCOMPONENT_EXPORT*` 声明对外导出的函数/数据/契约/座位/syscall
   - 生命周期回调（init 返回 `KMOD_OK` 才 READY；exit/quiesce/drain 可空）
3. **构建**：无需改 Makefile，`make SkylineSystem-x86_64.iso -j4` 会自动发现新 `.cpp`。
4. **验证**：看串口是否出现该组件的 `builtin ready` 与拓扑顺序
   （`builtin components: 37→38 declared`），再跑 `kmod_smoke.ps1`；
   若组件依赖新内核导出/契约，先补 `kernel_exports.cpp` / `kmod_services.h` 等。
5. **文档**：在 [kernel-components.md](kernel-components.md) 台账补一行。

### 5.2 新增一个 `.ko`

1. 在 `kernel/modules/loadable/` 新增 `.cpp`（一个文件 = 一个模块）。
2. 用 `KMOD_IMPORT*` / `KMOD_EXPORT*` / `KMOD_MANIFEST` 声明；所需能力在
   `kernel/src/arch/x86_64/kmod/capability.cpp` 的 STRICT 可信清单登记。
3. `make -C kernel modules modules-check`。
4. `res/limine.conf` 加 `module_path`；若 `KMOD_MF_BOOT`，启动自动加载，
   否则由 selftest/`KmodLoadVersion` 按名加载。
5. 更新 `docs/kernel-modules.md` 的演示模块表（如新增 demo）。

### 5.3 修改现有组件/头文件时

- 只改组件 `.cpp`：增量 `make SkylineSystem-x86_64.iso -j4` 即可。
- 改**头文件**：注意 `kernel/Makefile` 只为 `.c/.S` 生成 header deps（`.d`），
  **C++ 头文件依赖未跟踪**。修改共享头后请 `make -C kernel clean`（或删除相关
  `obj-<arch>/` 对象）再构建，否则可能出现"改头文件不生效"的假象。
- 改 `KCFLAGS/KCPPFLAGS`/`DEBUG` 等构建参数：必须 clean 重建。
- 改组件宏/段名：同时检查 `linker-$(KARCH).ld` 的 KEEP 段与加载器段名常量
  （`kmod_abi.h` 的 `KMOD_SEC_*`）。

## 6. 验证门禁（构建后跑什么）

```bash
# 1) 内核/组件构建 + .ko 契约
make SkylineSystem-x86_64.iso -j4
make -C kernel modules-check                 # 15/15 [OK]

# 2) 宿主测试（加载器/拓扑/能力/治理/组件模型/共享服务）
cd tests && make -B kmod && ./bin/kmod_test          # PASS 6/6
make -B kmod-asan && ./bin/kmod_test_asan            # ASAN 0 报告

# 3) QEMU 串口门禁（Windows PowerShell）
pwsh -File res/scripts/test/kmod_smoke.ps1 `
     -Root C:\ZSY\SkylineSystem -WaitSeconds 200 -MinBuiltins 30
# 期望：KMOD SMOKE: PASS
#       KMOD_SELFTEST PASS 12/12
#       builtin ready ×37 / kernel exports: 57
#       boot load: 2 requested, 2 ready, 0 failed
```

## 7. 常见构建问题

| 现象 | 原因 / 处理 |
|---|---|
| `Please run the ./get-deps script first` | 首次构建未拉取依赖：`make kernel-deps` 或直接 `make cm` |
| 直接 `cd kernel && make` 报找不到 `kernel/include/...` | 缺 `SS_BUILD_DIR`：用 `make SS_BUILD_DIR="$(cd .. && pwd)"` |
| 改了头文件但行为没变 | C++ header deps 未跟踪：`make -C kernel clean` 后重建 |
| `.ko` 加载失败 `UNRESOLVED` | 引用了未导出的符号：改用 `.in` 导入，或把符号加入 `.kout` 导出 |
| `.ko` 加载失败 `reloc`/`modules-check FAIL` | 编译 flags 混用（误用内核 PIE flags）或新增重定位类型；使用 `KMOD_CXXFLAGS` |
| 新组件没出现 | 源码不在 `modules/builtin/{common,<arch>}`；或组件宏 ID/段名冲突导致描述符无效 |
| 启动 `builtin: descriptor #N invalid` | BEGIN/END 哨兵缺失/条目数非整除/manifest 字段非法 |
| 组件 init 失败导致启动失败 | 查看 `[kmod] commit FAILED ... rollback`，确认依赖/能力/契约条件 |
| 新 `.ko` 不在池中 | 未在 `res/limine.conf` 添加 `module_path` |
| STRICT 下 `.ko` 被拒 | 未在 `capability.cpp` 可信清单登记能力 |

## 8. 关键文件速查

| 文件 | 作用 |
|---|---|
| `Makefile`（根） | `cm`/`kernel`/ISO 打包/run/clean |
| `kernel/Makefile` | `find_sources`/`find_module_sources`、编译/链接规则、`.ko`、`modules-check` |
| `kernel/linker-x86_64.ld` | `.kbuiltin.in/out/desc`、`.kout` 的 KEEP 与边界符号 |
| `kernel/include/kmod/kmod_abi.h` | ABI 版本、段名、结构体、Tier/flags/能力/契约常量 |
| `kernel/include/kmod/kmod_component.h` | 内建组件 `KCOMPONENT_*` 宏（生成 `.kbuiltin.*`） |
| `kernel/include/kmod/kmod_module.h` | `.ko` 作者宏（生成 `.in/.out/.kmod_manifest`） |
| `kernel/include/kmod/kmod_services.h` | Tier0 共享服务 ops 契约（klib/sync/algo） |
| `kernel/src/arch/x86_64/kmod/builtin.cpp` | 描述符扫描/校验/拓扑加载/回滚 |
| `kernel/src/arch/x86_64/kmod/kmod_core.cpp` | `KmodInit`/`KmodBootComponents` 入口 |
| `kernel/src/arch/x86_64/kmod/elf_loader.cpp` | ET_REL 加载器（`.ko`） |
| `kernel/src/arch/x86_64/kmod/governance.cpp` | 加载事务/依赖图/热替换/卸载 |
| `kernel/modules/builtin/x86_64/components/` | 37 个组件壳（13 类子目录） |
| `kernel/modules/loadable/` | 15 个 `.ko` 演示源码 |
| `res/limine.conf` | ISO 内 `.ko` 的 `module_path` 清单 |
| `res/scripts/test/kmod_smoke.ps1` | QEMU 串口门禁 |
