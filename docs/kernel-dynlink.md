# 统一 DynModule 动态链接层 (kdll) + modules.saf

> 本文是实现记录，不是规划。对应代码：
> **kernel/include/kmod/kdll.h**、**kernel/include/kmod/kmod_abi.h**、
> **kernel/src/arch/x86_64/kmod/kdll.cpp**、**saf.cpp**、**kernel/Makefile**
> （构建期生成 `.kout` 导出表）、**res/scripts/make_modules_saf.py**。
>
> 结论：**Tier 1（内建组件）与 Tier 2（外部 .ko）已合并为一个 DynModule
> 类别；内核镜像里不再有 .kbuiltin 组件**。40 个原组件壳各自编译成独立
> .ko，与 18 个 demo/扩展 .ko 一起打进一个 **modules.saf**，Limine 只声明
> 这一个模块。4 个 Tier0 核心服务（pmm/vmm/heap/syscall 入口）以 resident
> 节点形式留在模块表里，供模块显式注入。

## 1. 为什么合并 / 为什么要 SAF

旧模型里 Tier 1 是"静态链接进内核、但走 .ko 线格式"的内建组件，Tier 2 是
"运行时 ET_REL 加载"的外部 .ko。两者共享 manifest/.in/.out，却仍然有：

1. 两个加载入口：KmodBootComponents() 走 builtin.cpp，KmodBootLoad() 走
   governance.cpp；
2. 两套 tier 常量（POLICY=1 / IMPL=2），tier 出现在清单和自省里；
3. 不能把内建组件当成可注入对象按名 LoadLibrary；
4. 一个模块无法在运行期显式注入另一个模块；
5. 每个 .ko 都要在 limine.conf 里逐个 module_path，模块一多配置就爆炸。

现在：

- 组件壳用 **-DKMOD_COMPONENT_AS_KO=1** 把 KCOMPONENT_* 映射到
  .kmod_manifest/.in/.out，各自链接成独立 .ko；
- 内核保留共享实现库（driver/FS/net/sched 实现代码），并在链接前生成
  **kernel.component.v1** 导出表（`obj-<arch>/generated/component_exports.S`）
  供组件 .ko 解析；
- 所有 .ko（40 组件 + 18 demo）打进一个 **modules.saf**；
- limine.conf 只有一行 module_path: boot():/boot/modules.saf；
- KmodSafIngest 解析 SAF 入池，KmodBootLoad 按 manifest 依赖加载。

## 2. 模型

~~~text
Tier0 内核镜像
  ├─ 固定机制：boot/IDT/PMM/VMM/SLAB/SLAB-klib/kmod loader...
  ├─ 未迁共享实现库：ACPI/APIC/SMP/time/PCI/dev/FS/net/sched/syscall...
  ├─ 已迁实现：AHCI/NVMe/xHCI+USB/PS2 键鼠/e1000+lwIP 胶水/ext4(lwext4)/
  │             syscall.{file,proc,time,mem,random,dev,info}、display.fb、
  │             dev.core、block.core、fatfs、lwIP 协议栈、file-cache core、ACPI、RTC、
  │             fd_manager core、HPET、LAPIC/IOAPIC、sched_bench
  └─ .kout 导出：260 symbols
        - kernel.mem/log/sync/klib/algo/epoch/time/dynlink...
        - 197 个 kernel.component.v1：尚未迁出的实现函数/数据
  └─ resident DynModule 节点（无代码镜像）：
        kernel.mem.pmm / vmm / heap / arch.syscall

Limine 单一模块: modules.saf
  ├─ 40 个 kernel.* 组件 .ko（trusted）
  │    manifest/.in/.out；实现文件按 modules/mcr 链接进来
  │    已迁组件之间走 .out 跨 .ko 符号表 + .kmod_dep_names provider 先序
  └─ 18 个 vendor.demo.* .ko（普通 .ko，STRICT 能力策略）
~~~

对所有调用方，组件 .ko 与普通 .ko 完全同构：都走 governance 的
布局/重定位/bind/apply/init，都参与拓扑、能力、引用计数、热替换、注入。

### 2.1 ABI / 加载器变化

| 项 | 变化 |
|---|---|
| tier | **KMOD_TIER_DYN = 1**；KMOD_TIER_POLICY 仍等于 1；旧线值 **KMOD_TIER_IMPL = 2** 作为兼容别名接受 |
| flags | **KMOD_MF_RESIDENT**（static 镜像属性，.ko 不得声明）、**KMOD_MF_DEMAND**（按需注入） |
| 导出 flag | **KMOD_EXF_EXTERNAL**：导出地址不在本 .ko 镜像内（trusted 组件把 syscall 实现指向 kernel 镜像） |
| 契约 | **kernel.dynlink.v1**（注入服务）、**kernel.component.v1**（组件实现符号） |
| 镜像池 | 新增 **trusted** 标记：来自内核构建的 SAF，允许 kernel./core. 保留名、能力按清单授予、固定 syscall 槽 |
| 模块节点 | 新增 pin_count、**KMOD_MODF_RESIDENT/CORE/INJECTED/TRUSTED** |

kmod_manifest_t / kmod_export_t / kmod_import_t 的布局没有变，
KMOD_ABI_VERSION 仍为 1；KMOD_MAX_EXPORTS 从 128 提升到 512。

### 2.2 注入服务

kernel.dynlink.v1 的 ops：

~~~c
typedef struct kmod_dynlink_ops {
    uint32_t abi_version, ops_size;
    int32_t (*inject)(const char *name, uint32_t flags);       /* LoadLibrary */
    int32_t (*eject)(const char *name);                        /* FreeLibrary */
    void   *(*resolve)(const char *provider, const char *sym);  /* GetProcAddress */
    int32_t (*inject_image)(const void *data, uint64_t size, uint32_t flags);
    int32_t (*inject_file)(const char *path, uint32_t flags);
} kmod_dynlink_ops_t;
~~~

模块侧：

~~~c
KMOD_IMPORT_DYNLINK(dynlink);
...
dynlink->inject("kernel.mem.pmm", KMOD_INJ_PIN | KMOD_INJ_SILENT);
void *fn = dynlink->resolve("vendor.demo.hello", "hello_fn");
~~~

治理规则：

- 导入 kernel.dynlink.v1 / KdllInject / KdllInjectImage / KdllInjectFile
  要求 **KMOD_CAP_MODULE_LOAD**；
- 直接导入 KdllEject 要求 **KMOD_CAP_MODULE_UNLOAD**；
- **KMOD_INJ_PIN** 让 refcount +1，KdllEject 只释放自己持有的 PIN；
  PIN 归零 + 无消费者 + 声明 UNLOADABLE 才真正卸载；
- trusted 组件 .ko 与 resident 节点永远不卸载代码；
- 注入失败不会留下半加载：relocatable 路径仍走 governance 的全回滚事务。

## 3. 启动路径

~~~text
Tier0: ... PMM/VMM/SLAB -> syscall_init -> KmodInit
                                    |
                                    +-- KmodGovInit / KmodSeatInit / KmodSyscallInit
                                    +-- KdllInit  (登记 4 个 resident 核心服务)
        |
        +-- KmodBootIngest()
        |     Limine module: boot():/boot/modules.saf
        |     KmodSafIngest: 58 .ko -> 镜像池（kernel.* = trusted）
        |
        +-- KmodBootLoad()
        |     43 个 BOOT 模块（40 组件 + hello/greeter/xmod 等 demo）按依赖拓扑加载
        |     15 个非 BOOT demo 留在池中，供 selftest / KdllInject
        |
        +-- KmodSelfTest()  14 个用例（含 dynamic-inject）
        +-- sti
~~~

### 3.1 resident 核心服务

KdllInit() 把 Tier0 里没有组件壳的核心机制登记成 resident DynModule：
kernel.mem.pmm、kernel.mem.vmm、kernel.mem.heap、kernel.arch.syscall。
它们没有代码镜像/导出符号，但出现在统一模块表里：

- 模块可以 inject/resolve（resolve 返回 NULL 是可预期的）；
- KmodDump() / syscall 29 快照里可见；
- 注入引用计数与普通模块一致。

dosvm 的 init 会显式注入 pmm/acpi，串口证据：

~~~text
[dosvm] dynlink: inject kernel.mem.pmm rc=0, kernel.arch.acpi rc=0 (pins=1/1)
~~~

### 3.2 dosvm 现在也是独立 .ko

原 Tier1 实现 + 原 Tier2 门面已合并到
kernel/modules/x86_64/dosvm/dosvm.cpp，并作为
**kernel.arch.dosvm.ko** 从 SAF 加载：

- 模式切换/影屏/低内存实现都在这个 .ko 内；
- syscall 210..214 用 KCOMPONENT_EXPORT_SYSCALL 直接注册（外部地址，
  KMOD_EXF_EXTERNAL）；
- init 里注入 kernel.mem.pmm 与 kernel.arch.acpi，exit 里 eject；
- 用户态 ABI 未变：DOS 客户端仍调用 210/211/212/213/214。

## 4. 注入示范模块

| 模块 | flags | 作用 |
|---|---|---|
| **vendor.demo.late** | DEMAND + UNLOADABLE | 平时不加载；导出 late_magic |
| **vendor.demo.inject** | DEMAND + UNLOADABLE | 依赖 late；init 再 inject hello 并 resolve hello_fn |

KdllInject("vendor.demo.inject", PIN) 的实际效果：

1. governance 按 .in 依赖先注入 vendor.demo.late；
2. demo_inject init 调 dynlink->inject("vendor.demo.hello", PIN)；
3. dynlink->resolve("vendor.demo.hello", "hello_fn") 返回导出地址；
4. selftest eject 两级模块，PIN 归零，模块表恢复干净。

## 5. modules.saf

### 5.1 格式

沿用 res/saf/SPECS.md：

~~~text
header = <QQ256sQ>  (magic, len, name, flags)
folder = header + u64 num_children + u64 children[]
file   = header + u64 size + u64 addr
~~~

res/scripts/make_modules_saf.py 把输入目录下所有 .ko 递归收集、按 basename
平铺到 root folder，确定性排序，写出一个不含目录嵌套的 SAF。内核解析器
saf.cpp 只信任 root folder + file 节点，所有 offset 都做边界检查。

### 5.2 构建与打包

~~~bash
cd kernel
make SS_BUILD_DIR=$(cd .. && pwd) modules       # demos + 40 component .ko
make SS_BUILD_DIR=$(cd .. && pwd) modules-saf   # -> bin-x86_64/modules.saf
~~~

根 Makefile 的 kernel 目标会依次执行 modules 与 modules-saf；ISO 只拷贝
**kernel/bin-x86_64/modules.saf** 到 **/boot/modules.saf**，不再逐个拷贝
.ko。res/limine.conf 只有：

~~~text
module_path: boot():/boot/modules.saf
~~~

Setup 的 os.pkg 也包含 boot/modules.saf（不再枚举单个 .ko）。

## 6. 兼容性 / 信任边界

- 旧 .ko 的 .in/.out/manifest 布局不变，tier=2 仍被接受；
- 新模块应写 KMOD_TIER_DYN；
- .ko 不能声明 KMOD_MF_RESIDENT；普通 .ko 也不能使用
  KMOD_EXF_EXTERNAL（只有 trusted SAF 组件可以）；
- trusted 判定只看 manifest 名 kernel./core.，由 KmodSafIngest 在把
  SAF 文件入池时设置；普通 vendor.* 仍走 STRICT 白名单；
- 注入仍是内核态协作式：没有签名/沙箱，与 Linux 模块同一信任级。

## 7. 验证

~~~bash
# 宿主（真实 kdll.cpp + governance/builtin/elf_loader）
cd tests && make -B kmod && ./bin/kmod_test          # 7/7 PASS
cd tests && make -B kmod-asan && ./bin/kmod_test_asan # 7/7 PASS

# 58 个 .ko 的重定位白名单
make -C kernel modules-check                           # 58/58 OK

# 内核 + ISO
make -j$(nproc)

# QEMU 无头门禁
pwsh -File res/scripts/test/kmod_smoke.ps1 -WaitSeconds 95
#   KMOD SMOKE: PASS
#   [kmod] saf: 58 .ko accepted, 0 rejected
#   [kmod] boot load: 43 requested, 43 ready, 0 failed
#   [kmod] kernel exports: 260 symbol(s) published
#   KMOD_SELFTEST PASS 14/14

# DOSVM 真实模式往返（dosvm.ko 从 SAF 加载）
python res/scripts/test/dosvm_trip_gate.py --wait 75
#   [dos] init: ... syscall_read=210 syscall_enter=211
#   [dosvm] trip OK: cpu=1 gen=1 trips=1
#   RESULT PASS
~~~

## 8. 仍未做 / 边界

1. **driver/FS/net/sched 的实现代码仍在 kernel 镜像里**。取消的是
   "模块内建"：组件 manifest/lifecycle/import/export 全部在独立 .ko，
   不再有 .kbuiltin 静态组件表。把这些实现也拆成独立 .ko 需要跨模块
   符号表、共享状态迁移和 bootstrap 顺序重做，属于下一阶段；当前
   构建期生成的 `.kout` 表把实现函数作为 kernel.component.v1 导出。
2. resident 核心服务节点没有导出表（它们是机制，不是可替换实现）。
3. KdllInjectFile 已实现，但没有用户态 syscall 包装（避免任意进程加载
   内核模块的权限模型问题）。
4. 在模块 init 回调里注入"尚未加载"的模块属于嵌套加载事务；当前 demo/
   dosvm 只注入已 READY 的 resident/builtin，依赖自动拉起由 governance
   的拓扑事务完成。
5. 组件 .ko 中仍可能引用 kernel 内部数据/函数；新增实现符号后重新构建
   即可（`kernel/Makefile` 在链接前用 `nm` 扫描 .ko 并生成
   `obj-<arch>/generated/component_exports.S`），不需要手工维护导出清单。
