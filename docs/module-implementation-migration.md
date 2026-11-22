<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# 实现文件迁入 .ko：大迁移台账

> 目标：把原本链接进内核镜像的 driver / FS / net / sched / syscall 实现代码，
> 按组件分别链接进自己的 ET_REL .ko；内核只保留 Tier 0 机制 + 尚未迁移的
> 共享实现库，并通过跨 .ko 符号表/契约把边界显式化。

## 1. 构建流水线

组件实现文件用清单声明，路径相对 `kernel/modules/`：

~~~text
kernel/modules/mcr/driver_ahci.sources
    common/drivers/ahci/ahci.cpp
    common/drivers/Disk_Interfaces/sata/sataDiskInterface.cpp
~~~

`kernel/Makefile` 在解析时读取所有 `*.sources`：

1. 把清单里的源文件从 `find_module_sources` 的内核 OBJ 中排除；
2. `.cpp` 用 `KMOD_CXXFLAGS`（`-fno-pie -mcmodel=large`），`.c` 用
   `KMOD_CFLAGS`（C17 + 同样模块 flags + fakestdlib），编译成
   `obj-<arch>/component-ko/extra/...`；
3. 链接组件壳对象 + C/C++ 实现对象，得到该组件的 `.ko`。lwIP 的 84 个
   `.c` 文件就是走这条路径。

根 Makefile 的顺序变为：

~~~text
make -C kernel components        # 先有组件 .ko（含实现）
make -C kernel                   # 链接前 nm 扫描 .ko，生成 .kout 表并链接
make -C kernel modules modules-saf
~~~

为什么顺序不能反：生成器从组件 .ko 的未定义符号生成
`kernel.component.v1` 导出表；内核必须先拿到这张表才能链接。

生成器同时跳过“已由其他 .ko 定义”的符号：这些符号不能再生成
`extern` 内核引用，只能走跨 .ko 解析。当前跳过 HID 键盘回调、
`keyboard_feed_byte` 等。

## 2. 跨 .ko 符号表

### 2.1 显式导出

- 组件私有契约用 `KCOMPONENT_EXPORT_CONTRACT`；
- 需要让其他 .ko 直接引用的函数用 `KCOMPONENT_EXPORT_RAW`（新）：
  按 `nm` 的 mangled name 原样登记到 `.out`，内核不做语义转换；
- 普通 demo 可用 `KMOD_EXPORT_FN` / `KMOD_EXPORT_DATA`。

`.out` 里是一张 `kmod_export_t` 表，跨 .ko 解析只认这张表，不解析任意
ELF symtab —— 这样可见面是显式的。

### 2.2 .kmod_dep_names

`.in` 里的 provider 字符串指针需要重定位，加载器在 prepare 之前读不了。
新增 `.kmod_dep_names` 段：`KCOMPONENT_DEPENDS` / `KMOD_DEPENDS` 同时写入
一条 NUL 分隔的 provider 名字（无指针）。

`governance::gov_stage_add` 在 prepare 当前镜像之前：

1. 读 `.kmod_dep_names`；
2. 对每个 provider：已 READY 就复用；在镜像池里就递归 prepare；
3. 用 staging 路径栈检测环，返回 `KMOD_ERR_CYCLE`；
4. 最后才 prepare 消费者。

这样同一事务里的 provider 先 prepare，消费者重定位时可以从
`g_prep[]`（未 commit 但地址稳定）拿到 provider 的 `.out` 地址。
拓扑提交仍保证 provider init 先于 consumer init。

### 2.3 加载器

`KmodElfLoadImageEx2` 增加 `resolver(ctx, name, &addr)`：

- `kexp`（内核 `.kout`）未命中时调用；
- governance 的回调先扫 READY 模块，再扫 prepare 缓存；
- 仍未命中才返回 `KMOD_ERR_UNRESOLVED`。

## 3. 已迁移组件（本轮）

| 组件 .ko | 迁入的实现文件 | 结果 |
|---|---|---|
| `kernel.driver.ahci` | `drivers/ahci/ahci.cpp`、`Disk_Interfaces/sata/sataDiskInterface.cpp` | 91 KB；从 .ko 探测 AHCI，SATA 根盘挂载成功 |
| `kernel.driver.nvme` | `drivers/nvme/nvme.cpp`、`rw.cpp` | 88 KB；PCI match 仍由组件壳注册 |
| `kernel.driver.xhci` | `usb/xhci.cpp`、`usb_device.cpp`、`msc.cpp`、`uac.cpp`、`uvc.cpp`、`hid.cpp` | 239 KB；XHCI 控制器/USB HID/MSC 全在 .ko 内 |
| `kernel.input.ps2.kbd` | `keyboard/x86/keyboard.cpp`、`input/kbd.cpp` | 55 KB；Kbd 共享页按 4K 对齐加载 |
| `kernel.input.ps2.mouse` | `mouse/x86/mouse.cpp` | 42 KB；滚轮字节通过跨 .ko 解析的 `keyboard_feed_byte` |
| `kernel.driver.e1000` | `net/e1000.cpp`、`net/e1000_lwip.cpp` | 86 KB；DHCP 10.0.2.15 + ping 10.0.2.2 成功 |
| `kernel.fs.ext4` | `lwext4/blockdev/blockdev.cpp`、`lwext4/src/*.cpp`、`ext4_user_alloc.cpp`（23 个） | 979 KB；根盘 `sata0` 挂载 /mp/，Explorer readdir 通过 |
| `kernel.syscall.file` | `syscall/fops.cpp` | 56 KB；槽位 0-5/22/23，readdir/mkdir 走 ext4 .ko |
| `kernel.syscall.time` | `syscall/time.cpp` | 20 KB；17/27/28；`mktime` 收进 Tier0 klib |
| `kernel.syscall.mem` | `syscall/memory.cpp`、`cpu.cpp`、`pmmap.cpp` | 44 KB；4 个槽全部本地 |
| `kernel.syscall.proc` | `syscall/task.cpp`、`exec.cpp` | 86 KB；sys_load/sys_launch + task 函数；elf_load/NOT_RUNQ_P 留 Tier0 |
| `kernel.syscall.random` | `syscall/random.cpp` | 12 KB；走随机座位 |
| `kernel.syscall.dev` | `syscall/dbgout.cpp` | 15 KB；24 dbgsout |
| `kernel.syscall.info` | `syscall/device.cpp`、`sys_info.cpp` | 32 KB；16/21/25/26 全部本地；sysinfo 页 core 留内核 |
| `kernel.display.fb` | `framebuffer/fb.cpp` | 27 KB；帧缓冲设备注册（Fb 指针仍在 Tier0） |
| `kernel.dev.core` | `drivers/dev/dev.cpp`、`devl.cpp`、`fs/mount.cpp` | 70 KB；Dev 注册表 + 挂载点表；fd_manager 仍 Tier0 |
| `kernel.pci.core` | `dev/pci/pcidesc.cpp` | 83 KB；PCI 名称表；Tier0 pci.cpp 经 hook 查名 |
| `kernel.fs.vfs` | VFS 注册表/契约壳 + `fd_ops.cpp` | 63 KB；VFS registry + fd_alloc/get/free |
| `kernel.fs.fdcore` | `fs/fd.cpp`（init/destroy/dup） | 43 KB；fd_manager 核心；sched.core 依赖 fdcore |
| `kernel.arch.acpi` | `acpi.cpp` + `madt.cpp` | 30 KB；ACPI Init/FindTable/MADT；mcfg/madt 全局量/FindTable 跳板留 Tier0 |
| `kernel.time.rtc` | `rtc.cpp` | 26 KB；RTC Init/read/update/ToUnixTime；字段与 PIT 跳板留 Tier0 |
| `kernel.time.hpet` | `hpet.cpp` | 21 KB；HPET Init/Available/GetTimeNS/SleepNS；Tier0 跳板 |
| `kernel.arch.apic` | `lapic.cpp`、`ioapic.cpp` | 50 KB；LAPIC + IOAPIC Init/RemapIRQ |
| `kernel.sched.policy.eevdf` | `sched_bench.cpp` | 63 KB；默认策略座位 + REEVDF 基准 |
| `kernel.fs.fccore` | `fs/fc.cpp`、`fc_idle.cpp` | 112 KB；file cache core + idle/fsync/tick；CPU init/limits 经 hook |
| `kernel.block.core` | `partition/mbrgpt.cpp`、`mgr.cpp` | 47 KB；PartitionManager 本地，块契约不变 |
| `kernel.fs.fatfs` | `ff.cpp`、`ffunicode.cpp`、`diskio.cpp`、`identfat.cpp`、`identfstype.cpp` | 297 KB；FatFs 完整实现；挂载路径仍 identify-only |
| `kernel.net.lwip` | `net/**/*.c`（84 个）+ `e1000_lwip.cpp` | 566 KB；lwIP 协议栈 + 胶水；DHCP/ping 通过 |

对应内核符号已从 `bin-x86_64/kernel` 消失（`nm | grep -c "AHCI\|NVME\|XHCI\|USB::\|Kbd::\|ext4_\|sys_fopen\|sys_time\|sys_mmap\|sys_getrandom\|E1000" == 0`）。
这些实现之间的依赖现在走：

~~~text
kernel.driver.xhci --(HID raw exports)--> kernel.input.ps2.kbd
kernel.input.ps2.kbd --(keyboard_feed_byte)--> kernel.input.ps2.mouse
kernel.fs.ext4 --(ext4_dir_* raw exports)--> kernel.syscall.file
kernel.driver.e1000 --(NetStackInit raw exports)--> kernel.net.lwip
~~~

### 3.1 4K 段对齐

`kbd.cpp` 的 `KbdShared` 要求页对齐。加载器把段对齐上限从 16 提到 4096，
镜像按 `max_align` 手动机内对齐：

- `kmod_alloc(image_size + max_align - 1)`；
- `mem = align_up(raw, max_align)`；
- `kmod_elf_image_t.alloc_base` 保存 raw，`KmodElfFreeImage` 释放 raw。

宿主 `kmod_alloc` 是 mmap，本身页对齐；不需要额外 API。

## 4. 验证（本轮）

~~~text
kmod_smoke.ps1: PASS
  KMOD_SELFTEST PASS 14/14
  kernel modules loaded x38
  boot load: 43 requested, 43 ready, 0 failed
  desktop PROCESS: 6
  no crash/panic/STALL
  saf: 58 .ko accepted (size=3768952)
  kernel exports: 260 symbol(s) (197 component)

e1000 实测（-netdev user + e1000）:
  [lwip] netif e0 up: 10.0.2.15/24 gw 10.0.2.2
  [lwip] DHCP success: 10.0.2.15
  [lwip][ping] reply from 10.0.2.2 (replies=3)

ext4 + syscall.file 实测:
  [fs.rootfs] mounted sata0 at /mp/
  [kmod] syscall bind: kernel.syscall.file -> 8 builtin fixed
  client_churn_gate.py --cycles 5: C1..C6 全 PASS

syscall.proc/mem 实测:
  [kmod] syscall bind: kernel.syscall.proc -> 9 builtin fixed
  [kmod] syscall bind: kernel.syscall.mem  -> 4 builtin fixed
  [OK] LOADED PID:10/11/12（sys_load 来自 .ko）
  DOSVM trip PASS；churn 3/3 PASS

syscall.info/sysinfo 实测:
  [kmod] syscall bind: kernel.syscall.info -> 4 builtin fixed
  [INFO] OK! REMOVE SYSINFO MAPPING! × 6
  desktop ready，14/14 selftest PASS

display.fb 实测:
  [display.fb] ready: 1280x800 pitch=5120 ... kernel.display.v1 published
  desktop ready in 1528ms

dev/block/fatfs 实测:
  [comp.dev.core] init: Dev registry + file/mount manager ready
  [comp.block.core] contract=kernel.block.v1 registered_slots=8
  [fs.rootfs] mounted sata0 at /mp/
  boot load: 43/43；selftest 14/14；desktop ready

net.lwip 实测:
  [lwip] netif e0 up: 10.0.2.15/24 gw 10.0.2.2
  [lwip] DHCP success: 10.0.2.15
  [lwip][ping] reply from 10.0.2.2 (replies=3)
  desktop ready in 1545ms

fatfs 完整实现实测:
  [kmod] pool: + kernel.fs.fatfs (297056 bytes)
  [fs.fatfs] ready: identify-only registration at vfs slot 1
  boot load 43/43；rootfs sata0 mounted

mount table 实测:
  [comp.dev.core] init: Dev registry + file/mount manager ready
  [fs.rootfs] mounted sata0 at /mp/
  desktop ready in 1627ms（kelf_read_file 经 mount hook）

PCI 名称表实测:
  [comp.pci.core] init: contract=kernel.pci.v1 abi=1 slots=16
  [comp.pci.core] enumerate done: MCFG/ECAM
  [comp.pci.enumerate] two-phase enumeration complete

file-cache core 实测:
  [kmod] pool: + kernel.fs.fccore (112 KB)
  [comp.fs.cache] init: bsp file_cache soft=4096 KiB hard=8192 KiB
  [fs.vfs] ready: fs registry 8 slots ... kernel.vfs.v1 published
  boot load: 43/43；EXT4 MOUNTED；desktop ready in 1508ms
  fccore 注册 5 个 hook（fsync/tick/idle/cpu_init/limits）

fd 操作实测:
  [fs.vfs] exports fd_alloc/fd_get/fd_free
  [kmod] syscall bind: kernel.syscall.file -> 8 builtin fixed
  desktop ready in 1502ms；churn 3/3 PASS

ACPI 解析实测:
  [kmod] pool: + kernel.arch.acpi (30 KB)
  Preparing ACPI... ACPI_DIV: 4；Found MADT.
  boot load 43/43；desktop ready 1581ms

RTC 实测:
  [kmod] pool: + kernel.time.rtc (26 KB)
  [comp.time.rtc] init: unix=1767225600
  [comp.time.pit] init after time.rtc；desktop ready 1435ms

fd_manager core 实测:
  [kmod] pool: + kernel.fs.fdcore (43 KB)
  edge kernel.fs.fdcore -> kernel.sched.core
  boot load 43/43；desktop ready 1580ms

HPET 实测:
  [kmod] pool: + kernel.time.hpet (21 KB)
  HPET: Detected frequency of 100000000 Hz
  tsc: calibrated 2610571 cycles/ms (HPET window 100049100ns)
  boot load 43/43；desktop ready 1440ms

LAPIC/IOAPIC 实测:
  [kmod] pool: + kernel.arch.apic (50 KB)
  [comp.arch.apic] init: LAPIC/IOAPIC ready, PIC masked
  IOAPIC COUNT:24；boot load 43/43；desktop 1646ms

sched_bench 实测:
  [kmod] pool: + kernel.sched.policy.eevdf (62 KB)
  [comp.sched.policy] init: kernel default EEVDF policy installed
  boot load 43/43；desktop ready 1781ms
~~~

新增 selftest `cross-module-symbol`：demo_xmod 直接 `extern "C"` 引用
demo_hello 的 `hello_fn`，init 写入 magic，再用 `KdllResolve` 读回校验。
boot log 同时可见 `[demo.xmod] cross-ko hello_fn -> 1001`。

## 5. 剩余迁移

按“先无共享状态、后 bootstrap 强耦合”的顺序：

1. `kernel.fs.vfs` / `fs.rootfs` / `pci.core`：全局注册表/根挂载/枚举状态，
   需要先定义跨模块只读句柄或契约；
2. `kernel.arch.acpi`：PCI/APIC/SMP/kernel 代码直接调用，需要先做
   Tier0 包装/函数指针；
3. `kernel.arch.apic` / `kernel.arch.smp`：bootstrap 顺序、IPI/IDT 共享状态；
4. `kernel.time.*`：PIT/HPET/RTC 被 VMM/stallwatch/scheduler 直接调用；
5. sched policy / alloc policy 座位实现——座位机制已可用，代码仍在共享实现库，
   待逐函数契约化。

## 6. 注意事项

- 修改 `kmod_internal.h` / `kmod_abi.h` 的结构布局后必须 **clean build**；
  增量 make 不会自动重编所有依赖该头的 kmod 对象，会制造“部分 TU 新布局、
  部分 TU 旧布局”的假故障。
- 跨 .ko 解析目前只认 `.out` 显式导出，不是完整 ELF dlsym；新增跨模块
  接口必须显式导出，否则加载期会以 unresolved 拒绝。
- provider 先序由 `.kmod_dep_names` 驱动；只写 `KMOD_IMPORT` 而不写
  `KMOD_DEPENDS` 的旧模块仍按“provider 已 READY”假设工作。迁移新代码时
  同时声明依赖。
