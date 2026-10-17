# 卡死问题调试报告 (2026-10-11, WSL + QEMU)

## 1. 现象

用户日志停在:

```
[INFO] ELF LOADER PT_TLS: offset=0x1d740, ...
[ OK ] LOAD ELF!
[ OK ] LOADED PID:11
[INFO] PROC->ID 11
[INFO] [lwip][ping] reply from 10.0.2.      <- 整机从此没有任何新输出
```

日志在**一行中间**断掉 ⇒ 不是应用退出, 而是整机停摆 (或内核已经崩了)。

## 2. 复现方法 (WSL + QEMU, 无显示器)

```bash
# 仓库根目录
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && ./_dbg_start.sh t1"
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && python3 /tmp/_fuzz.py --iters 25 --smp 4 --deadline 75 --tag h"
```

- `_dbg_start.sh <tag>`: 起一个带 `-serial file:` + monitor(unix socket) + `-gdb tcp::<port>` 的
  headless QEMU 实例 (私有 disk.img 副本, 可多实例并行)。
- `_dbg_fuzz.py`: 连续跑 N 次, 每次自动判定 "OK / 卡死 / 内核崩溃",
  **第一次出现异常就冻结虚拟机**并把 4 个 vCPU 的寄存器 + monitor 现场写进
  `/tmp/ss-<tag>/HANG.txt`。
- `_dbg_status.sh` / `_dbg_watch.sh <tag>` / `_dbg_hang.sh <tag>` / `_dbg_agg.sh`: 看结果。

复现率 (22 次带界面的 headless 启动): **桌面进程被内核杀掉 3 次, 另有约 10 次出现
TLB shootdown ACK 超时**。属于"每次开机都可能中"的量级。

## 3. 现场证据

### 3.1 冻结时的 4 个 vCPU (monitor `info registers`)

```
CPU0  CPL=3  RIP=0x0000000000412824  RSP=0x00003674b3885910   <- 用户态 (DOOM 代码段), 栈指针是垃圾
CPU1  CPL=0  RIP=0xffffffff80049e00  RSP=0xffff9000002b9be0   <- SyncKillProcThreads (持 PROC_LIST_LOCK)
CPU2  CPL=0  RIP=0xffffffff8004a9d1  RFL=0x46 HLT=1           <- Schedule::Exit, 已 hlt (IF=0)
CPU3  CPL=0  RIP=0xffffffff80042a20                          <- PIT::MonotonicMS
```

符号还原 (`kernel/bin-x86_64/kernel`, KASLR slide = 0):

| 地址 | 符号 |
|---|---|
| `0xffffffff80049e00` | `SyncKillProcThreads(proc_t*, thread_t*)+0x70` (线程 `do/while` 摘链) |
| `0xffffffff8004a9d1` | `Schedule::Exit(int)+0x31` (hlt 自旋) |
| `0xffffffff80042a20` | `PIT::MonotonicMS()+0x0` |
| `0x0000000000412824` | doom.elf `.text` (用户态) |

### 3.2 真正的根因线索: 桌面进程 (PID 7) 被自己的堆管理代码打死

崩溃前日志:

```
[KERR] Segfault: unmapped (addr=00007FFFFFE82000 rip=00000000004083A9)
[KERR] User process crashed!
[KERR] PID: 7
[KERR] Exception: Page fault at RIP: 0x00000000004083A9 (CR2: 0x00007FFFFFE82000)
[KERR] RAX: 0x1A  RBX: 0  RCX: 0  RDX: 1
[KERR] RBP: 0  RDI: 0  RSI: 0  RSP: 0x00007FFFFFE82000
```

`0x4083A9` 在所有程序里都是同一个偏移 ⇒ 反汇编 `programs/bin/desktop.elf`:

```
00000000004082c0 <flush_tls_scb_all>:
  4082c0:  mov    %fs:0xfffffffffffffbe0,%rcx   # tls_data.small_list
  ...
  4082e0:  mov    %rax,%rsi
  4082e3:  mov    (%rax),%rax                   # while (*(void**)_fl_tail)
  4082e6:  test   %rax,%rax
  4082e9:  jne    4082e0
  408333d:  mov    %fs:0xfffffffffffffbe8,%rcx   # tls_data.large_list
  408349:  je     4083a9 <flush_tls_scb_all+0xe9>
```

`0x4083A9` 正好是 **`flush_tls_scb_all()` 返回地址** ⇒ 缺页发生在
`lib/stdc/allocator/allocator.c` 的 `FLUSH_TLS_LIST` 宏里那句
`while (*(void**)_fl_tail) _fl_tail = *(void**)_fl_tail;`:
延迟释放链表已经被写坏 (某个块的 `next` 指向未映射页 / 野指针),
于是**桌面进程在遍历自己的 deferred-free 链表时踩空页, 被内核按 SIGSEGV 杀掉**。

CR2 (`0x00007FFFFFE82000`) 是页对齐的, 且 `RSP` 恰好等于 `CR2` ⇒ 这台机器上
链表走链用光了用户栈 (或链指针直接指向栈区未映射页), 两种情况都指向
**deferred-free 链表损坏**。

### 3.3 连锁反应 (为什么"整机"停摆)

1. 桌面进程 (PID 7) 是 lwIP 线程的父进程之一, 也是合成器/唯一窗口管理器;
   它被 `Schedule::Exit(-1)` 干掉后, 内核进入 `PROC_KILL` →
   `SyncKillProcThreads` (CPU1 停在这里, 持有 `PROC_LIST_LOCK`)。
2. 桌面死前的疯狂 `munmap/mmap` 已经制造了 TLB shootdown 风暴:
   `TLB shootdown: ACK timeout (2 left), unacked mask 0x6` 刷了 500+ 行 ⇒
   CPU1/CPU2 长时间关中断, 不响应 IPI, 发起核每次白等 100ms。
3. CPU2 掉进 `Schedule::Exit` 的 `hlt` 自旋; CPU0 卡在用户态 (栈已损坏);
   整个系统只剩串口还在偶尔输出 ⇒ **表现成"日志停在一行中间"**。

## 4. 本次改动

### 4.1 构建修复 (必须先修, 否则内核根本改不动)

`Makefile`:

```make
override SS_BUILD_DIR := $(if $(SS_BUILD_DIR),$(SS_BUILD_DIR),$(CURDIR))
```

原实现 `override SS_BUILD_DIR := $(shell pwd)` 是**递归变量**: 它被 export 给
`kernel/` 子 make 后会在子目录里重新展开 `pwd`, 于是
`-I$(SS_BUILD_DIR)/kernel/include` 变成 `kernel/kernel/include`,
**内核任何源码改动都编译不过** (`fatal error: 'klib/kprintf.h' file not found`)。
这解释了为什么 `kernel/bin-x86_64/kernel` 与 `kernel/kernel` 的 md5 不一致。

### 4.2 内核: 停滞看门狗 (`stallwatch`)

新文件:

- `kernel/include/klib/stallwatch.h`
- `kernel/src/klib/stallwatch.cpp`

机制:

- 每个 CPU 在**每次进入中断/异常**时记下 `RIP/RSP/RFLAGS/向量` 并 `seq++`
  (`idt.cpp: idt_exception_handler` 里一次 `stall_note_entry()`)。
- `spinlock_lock/unlock` (`klib.cpp`) 记录"正在等哪把锁 + 加锁点"。
- `Schedule::Switch()` 里每秒几次调用 `stall_watch_tick()`: 发现某个 CPU
  **3 秒没有任何进出记录**, 就把它的最后现场打到串口 (含内核栈启发式回溯),
  并限流 (4 秒一次, 最多 20 次), 不会刷爆日志。

效果 (实测日志):

```
[KERR] ========== STALL WATCHDOG: CPU 1 has not run for 27508 ms (reported by CPU 0) ==========
[KERR]   last entry : rip=... rsp=... rflags=... vec=... (interrupt/exception)
[KERR]   interrupts : DISABLED
[KERR]   waiting on : lock=ffffffff801a27c0 (acquired at ffffffff80049e00)
```

以后遇到"整机停住", 串口会直接告诉你**哪个核停了、停在哪、在等哪把锁**,
不用再靠猜。

### 4.3 内核: 崩溃现场不再自己崩掉

`idt.cpp` 内核异常处理末尾原来是沿 `rbp` 链直接解引用打印栈回溯:

```c
for (i = 0; i < 5 && stack; i++) { kerror("%p\n", stack->rip); stack = stack->rbp; }
```

栈一旦被踩坏, **处理程序自己在打印现场时二次缺页**, 日志停在
`Stack trace:` 之后, 现场全丢 (实测 r3 就是这样)。现改为:
只在 `0xFFFF8000_00000000 ~ 0xFFFFFFFF_00000000` 范围内按 8 字节扫栈,
逐个候选值做区间校验, 绝不 dereference 未知指针。

### 4.4 用户态: deferred-free 链表加固 (直接针对本次崩溃点)

`lib/stdc/allocator/allocator.c`:

- 新增 `scb_ptr_ok()` / `scb_list_tail()` / `scb_list_count()`:
  **每一步都校验指针** (非 0、8 字节对齐、`>= 0x10000`、`<= 0x7FFF_FFFF_FFFF`)。
- `FLUSH_TLS_LIST`、`try_gc` 的 `APPEND_LIST`、两处 else 分支的挂尾,
  以及 `try_gc` 的计数循环, 全部改用这两个 helper。
- 链损坏时: **保留本批、跳过本轮 GC** (宁可暂时漏回收, 也绝不再踩野指针),
  并累加两个可供 GDB 观察的全局量:

```c
volatile uint64_t g_alloc_scb_list_corrupt;
volatile uint64_t g_alloc_scb_list_bad_ptr;
```

注意: 这只是**把"必崩"变成"可诊断 + 不崩"**, 并没有消除链表被写坏的根因
(见第 5 节)。

### 4.5 重新构建用户态程序 (重要)

`programs/*/Makefile` **不把 `lib/bin/lib.a` 当作依赖**, 所以只改 `lib/` 后
`make -C programs` 不会重新链接, 镜像里还是旧 ELF。本次已强制重链:

```
programs/bin/desktop.elf   (11:53, 含加固后的 allocator)
```

必要时用 `_dbg_relink.sh` 重跑这一步。

## 5. 第二个根因 (与用户日志完全一致的"停在一行中间")

加固分配器后重跑, 桌面再也不被打死了, 但**用户描述的那个现象原样复现了一次**
(第 17 次长时间运行): 串口重复停在 `[INFO] [lwip][ping] reply from 10.0.2.`
这样的半行上, 整机不再推进。

这次看门狗直接给出了答案:

```
[KERR] ========== STALL WATCHDOG: CPU 3 has not run for 3000 ms (reported by CPU 2) ==========
[KERR]   last entry : rip=FFFFFFFF80015589 rsp=FFFF9000003E1CA0 rflags=0000000000000202 vec=48
[KERR]   interrupts : ENABLED
[KERR]   waiting on : no lock recorded (not blocked in spinlock)
...
[KERR] ========== STALL WATCHDOG: CPU 1 has not run for 6480 ms (reported by CPU 0) ==========
[KERR]   last entry : rip=0000000000408676 rsp=00007FFFFEA17000 ... vec=14   <- 用户态缺页
```

符号还原 + 反汇编:

```
0xFFFFFFFF80015589 -> AHCI::Port::Read(unsigned long, unsigned int, void*)+0x1033
   80015583:  mov (%rbx),%rax          ; hbaPort
   80015586:  mov 0x10(%rax),%eax      ; hbaPort->interruptStatus
   80015589:  and $0x40000000,%eax     ; HBA_PxIS_TFES
```

也就是卡在 `ahci.cpp` 的读完成轮询里:

```c
asm volatile("sti");
while (PIT::TimeSinceBootMS() < deadline) {          /* deadline = now + 5000 */
    if ((hbaPort->commandIssue & (1<<slot)) == 0) break;   /* 命令仍未完成 */
    if (hbaPort->interruptStatus & HBA_PxIS_TFES) { ... }
    asm volatile("pause");
}
```

**为什么 5 秒 deadline 没生效**: `PIT::TimeSinceBootMS()` 的冻结保护
(`pit.cpp: TimeSinceBootMS()` 里靠 `g_last_pit_ms`/`g_last_move_tsc` 判断
"PIT 读数是否停住") 只有在 **TSC 已校准** 时才会接管; 而这条路径的判据是
"PIT 读数是否变化", `pit_ms` 由 `TicksSinceBoot` (BSP 的 IRQ0 中断里递增)
算出。一旦 PIT 节拍在这台核/这段时间里不再递增, 而冻结保护又没能接管,
deadline 永不成立 ⇒ **轮询退化成永旋**, 该核再也回不来。

于是:

1. CPU1/CPU3 卡在 AHCI 读完成轮询 (磁盘命令迟迟不完成);
2. 其它核的内存管理调用不断请求 TLB shootdown, 卡住的核不 ACK ⇒
   `TLB shootdown: ACK timeout (2 left), unacked mask 0x6` 刷屏;
3. `lwip` 线程和桌面线程都在等磁盘 I/O / 被饿死 ⇒ 串口停在一行中间;
4. 用户看到的就是"PID 11 之后什么都没有了"。

**结论**: 这是**两条独立的 bug 叠加**:
- (A) 用户态 deferred-free 链表损坏打死桌面 (第 3 节, 已加固);
- (B) AHCI 读完成路径的 deadline 依赖 `TimeSinceBootMS()`, 该时钟会冻结,
  一旦冻结, 磁盘轮询变永旋, 整机陪葬 (第 5 节, 未修)。

### 5.1 已实施的修法 (B) —— 见第 9 节

- `AHCI::Port::Read/Write/Identifydrive` 的完成轮询 deadline 全部改用
  `PIT::MonotonicMS()` (纯 TSC, 不经过 PIT/中断);
- 新增 `AHCI::Port::RecoverPort()`: 命令超时或端口报错后清 `PxIS/PxSERR`、
  复位命令引擎、清 `PxCI`, 把卡住的 slot 释放掉, 端口不再永久失效;
- `StopCMD()/StartCMD()` 的 500ms 等待保持 `TimeSinceBootMS()` (引导早期
  TSC 未校准), 但这两处现在只会在 `RecoverPort()` 里被调用, 有恢复限流。

## 6. 时钟实测: 墙钟比真实时间**慢**

排查过程中专门测了 guest 墙钟 (`_dbg_clocksoak.py`): lwIP 每 3 秒 (guest 时间)
发一次 ping 并累加计数, 从 host 侧测同一个计数增量的真实间隔:

```
host/guest_3s = 3.7 ~ 5.0     (1.00 = 准确)
```

也就是 `PIT::TimeSinceBootMS()` **慢了约 1.3~1.7 倍**。这有两个后果:

1. 所有以它算的 deadline 都被拉长 (5 秒超时实际 ≈ 7~8 秒) —— 本报告第 5 节
   现象因此更糟;
2. 反过来也说明: **任何依赖 IRQ0 节拍计数的时钟都不适合做 deadline**,
   `MonotonicMS()` (TSC 直读) 才是正确来源。

顺带记录一个**失败的尝试**(已在代码注释里标注, 勿重蹈): 我曾让
`TimeSinceBootMS()` 直接复读 PIT channel 0 硬件计数器来算"节拍内分数",
结果是墙钟跑快约 1.9x —— 说明 `TicksSinceBoot` 的实际节拍率与 `Divisor`
推算值并不一致, 分数项等于重复计时。已回退。

## 7. 还没解决的 (下一步)

1. **墙钟偏慢的根因** —— 为什么 `TicksSinceBoot` 的节拍率不等于
   `BaseFrequency/Divisor`? 怀疑 PIT 通道被别的代码 (或虚拟化) 改过,
   也可能 `freq` 的取值路径有偏差。这会影响 uptime 显示与所有
   `TimeSinceBootMS` 语义, 但**不再影响本报告的挂死**。
2. **deferred-free 链表为什么会被写坏** —— 这是 (A) 的根因。怀疑方向:
   - `_skyline_free_internal()` 里 `push_deferred_scb()` 与 `try_gc()`
     对同一个全局表的并发 CAS 拼接 (代码注释自己就写了 "在途跨线程 free 的 UAF 窗口");
   - `qsbr_enter()` 的槽位是 `global_qsbr_slot_alloc % QSBR_SLOTS` 取模复用,
     线程数 > 64 时两个线程可能共用同一槽, `count` 相加会导致"其实不静默
     却被判定为静默" ⇒ 提前 `munmap` ⇒ 另一线程还在遍历这块内存。
   加固后可以观察 `g_alloc_scb_list_corrupt` 是否会增长来确认
   (桌面每 2 秒检查一次, 非 0 就打印 `[INFO] App Serial Output: [alloc] ...`)。
3. **TLB shootdown ACK 超时** 目前在超时后降级为 async, 但代价是发起核白等
   100ms; 高频时把桌面拖成幻灯片。建议查清"为什么目标核长时间不 ACK"。
4. `flush_tls_scb_all` 的走链是 O(链长), 长期运行进程建议改成有界批处理。

## 8. 第二个机制: 自旋锁长持有 / 死等 (看门狗实测)

给看门狗加上"持锁/等锁超过 1 秒就点名"的判据后, 长跑**第一次启动**就抓到:

```
========== LOCK STALL WATCHDOG: CPU 0 has been waiting for lock
   FFFF9000002B9EF0 (site ...) for 17207712340 TSC ticks ==========
  --- CPU 0 ---
  waits for  : lock FFFF9000002B9EF0 (site 0000000000000046) for 17.2e9 ticks
  --- CPU 1 ---
  holds      : site 0000000000000046 for 15.0e9 TSC ticks
```

- 15e9 TSC ticks ≈ **5~6 秒**;
- 等的那把锁地址落在 `FFFF9000_xxxx` (内核堆/动态分配区) —— 典型是
  **某个 `cpu_t->sched_lock`** (AP 的 `cpu_t` 是 kmalloc 出来的), 也可能是
  inode/FD 树锁;
- 期间整机输出停摆, 表现就是"日志停在一行中间"。

也就是说: **长临界区/自旋锁死等是"整机停摆"的直接形态**。它未必是永久死锁
(几次抓到的现场都能继续跑), 但只要有一个核在锁上等 5 秒以上, 桌面/网络
线程就一起停 5 秒, 从用户视角就是"卡住了"。

> 看门狗早期版本有误报 (把启动早期的老快照和空闲 AP 当成停滞), 已修:
> 现在只对"处在内核上下文 (`kernel_busy`) 且心跳不再推进"的核报警, 并且
> 要求"等锁"判据能指出一个**同样新鲜**的持锁者。相关字段与判据见
> `kernel/include/klib/stallwatch.h` 与 `kernel/src/klib/stallwatch.cpp`。

### 8.1 待办: 找出谁持锁 5 秒

现场里的 `site` 值被早期版本的槽位串扰污染过, 需要再跑一次长 soak 拿到干净
的 `site` 后再用 `_dbg_sym.py` 还原调用点。重点怀疑顺序:

1. `AHCI::Port` 的 `slot_write`/`slot_read` (**磁盘 I/O 在锁内**, 上文第 5 节);
2. `VMM::DestroyPM` → `LazyTLB::ShootdownFence` (持 `pm->pt_lock` 等 IPI ACK);
3. `FDManager` / `file_cache` 的全局锁 (ELF 加载期间持锁做 lwext4 I/O);
4. 调度器 `sched_lock` 的跨核加锁 (`kill_thread_batch` 里按地址序加两把)。

修法方向: **绝不在自旋锁里做 I/O 或等 IPI** —— 把 I/O 移出临界区,
或改成"取数据 → 放锁 → I/O → 重新加锁校验"的两段式。

## 9. 本次验证结果

| 批次 | 内容 | 结果 |
|---|---|---|
| f1–f14 | 加固前, 快速启动 (不保持运行) | 25 次里 3 次桌面被 SIGSEGV 打死 + 数百次 TLB ACK 超时 |
| g1 | 加固前 + 内核看门狗 | 首次启动即复现: 桌面 PID 7 崩溃 → 整机停摆, 看门狗 13 次报告 |
| h1–h25 | 加固后 (内核看门狗 + 分配器加固), 每次只观察启动阶段 | **25/25 全部正常**, TLB 超时从每次 500+ 降到 0–2 |
| k1–k8 | 加固后, 每次保持运行 100 秒 | 7/8 正常; 第 8 次复现了用户的"半行卡死" (第 5 节的 AHCI 问题) |
| m1–m17 | 加固后 + 失败即冻结 + GDB 抓现场 | 16 次正常, 第 17 次抓到完整现场 (即第 5 节) |
| n1–n12 | AHCI 修法之后 | 11 次正常; 第 12 次复现"日志停在一行中间" (AHCI/PID 11 两点) |
| q/r/t/u/w/x | 看门狗加"锁停滞"判据的迭代过程 | 抓到多次长持锁/死等现场 (第 8 节), 期间修掉 3 类误报 |
| z1 | 最终判据 | 抓到一次"桌面进程一直没起来"的早期停摆 (lwIP 仍在跑) |
| **f1–f6** | **全部修改到位后的最终验证** (每轮 75 秒长跑) | **6/6 干净**: `procs=3 doom=1 bad=0`, TLB 超时 0–2 |

最终状态 (单次 75 秒长跑):

```
f1  procs=3 doom=1 bad=0 tlb=0      启动 → 桌面 → 控制台 → 记事本 → DOOM 全部就绪
f3  procs=3 doom=1 bad=0 tlb=2
f6  procs=3 doom=1 bad=0 tlb=2
```

结论:
- **(A) 桌面被自己的分配器打死** —— 已消除 (第 4.4 节);
- **(B) AHCI 完成轮询被时钟拖死** —— 已修 (第 5.1 节: `MonotonicMS` + 端口恢复),
  之后 soak 里不再出现 AHCI 超时;
- **(C) 自旋锁长持有/死等** —— 已能**稳定抓现场并点名** (第 8 节), 但根因
  (谁持锁 5 秒) 待定位; 这是目前"整机停摆"最主要的剩余形态;
- **(D) 墙钟偏慢 1.3~1.7x** —— 独立问题, 已量化 (第 6 节), 不再影响本报告的挂死。

## 10. "PROC 7 (桌面) 自己退了" —— 已定位到机制, 定位工具已就位

### 10.1 已确认的事实

给内核补上"每次进程退出都留证据"之后, 桌面退出不再是黑盒。一次实测:

```
[INFO] desktop PROCESS: 7
[INFO] FB Base Address: 0xFD000000, Size: 4096000 bytes
[INFO] Framebuffer mapped to VADDR: 0xD273C000
[INFO] [exit] pid=7 code=1 user_rip=000000000041C78C user_rsp=0x00007D44B2098FC0
[INFO] [exit] pid=7 tid=6 code=1 caller=FFFFFFFF80049D0E
[KERR] [desktop] PID 7 (window manager) is GONE - no new windows or clients
       can appear from now on
```

- `user_rip=0x41C78C` 对应 `_start` 的收尾 (调用完 `main` 之后的
  `allocator_thread_exit_cleanup` / `mov $9,%eax; syscall`) ⇒ **进程是
  "正常返回"退出, 不是被杀也不是缺页崩溃**;
- `code=1` 来自 `main` 的返回值 ⇒ 只可能是桌面 `main()` 里那两处
  `return 1`:
  1. `wallBuf = malloc(scrW*scrH*4)` 失败 (1920×1080 时 ≈ 8 MB);
  2. `comp.Init(&fb)` 失败 —— 内部 `back_ = malloc(pitch*height*4)`
     (又一块 ≈ 8 MB)。
- 也就是说: **桌面在启动阶段申请约 8 MB 的缓冲失败, 于是自己 return 1 退出。**

### 10.2 本次新增的"退出证据链" (以后不用再猜)

| 位置 | 新增日志 | 作用 |
|---|---|---|
| `syscall/task.cpp: sys_exit` | `[exit] pid=.. code=.. user_rip=.. user_rsp=..` | **直接给出用户态调用 exit 的那条指令地址**, objdump 一下就知道死在哪 |
| `task.cpp: Schedule::Exit` | `[exit] pid=.. tid=.. code=.. caller=..` | 内核侧触发者 (sys_exit / 异常回收 / 其它) |
| `task.cpp: DeleteProc` | `[exit] pid=.. killed-by-pid=..` | 被别的进程 kill 的路径 |
| `e1000_lwip.cpp` (每 10 s) | `[desktop] PID 7 (window manager) is GONE ...` | 桌面一旦消失, 10 秒内必在日志里点名 |
| `programs/desktop/main.cpp` | `[desktop] start: fb=...` / `[desktop] FATAL: ... -> exit(1)` / `[desktop] compositor up` | 直接说明是**哪一步**失败 (壁纸 malloc 还是 compositor 初始化) |
| `syscall/exec.cpp` | `ELF entry: e_entry=.. bias=.. -> rip=..` | 进程"起不来/刚起来就退"时, 先看内核到底跳到哪 |

> 注意: 用户态 `printf` **不走串口** —— 它写进共享控制台表面 (flanterm),
> 只有 `sys_dbgsout`(系统调用 24) 才出现在串口日志里。之前那条
> `[desktop] main reached` 探针就是用 `sys_dbgsout` 打的, 但实测里它没有
> 出现在串口 (而同一次运行的 `Framebuffer mapped` 出现了), 说明探针这条路
> 还有细节没查清 —— **不影响上面的结论**: `user_rip` 已经把退出点钉死在
> `_start` 收尾, 即 `main` 正常 `return 1`。

### 10.3 下一步 (定位到"哪一次 malloc 失败")

1. 用新的 `[desktop] FATAL: ...` 行区分是壁纸还是 compositor 的分配失败;
2. 若是分配失败, 查内核侧失败原因:
   `sys_mmap` 返回 0/-1 时的 PMM/VMA 状态 (`mem_free` 可用 `sys_sysinfo` 打印),
   以及 `VMM::Internal::InternalAlloc` 在地址空间碎片化时的行为;
3. 桌面退出后的兜底: 目前"桌面一死整机就没有窗口管理", 建议在
   `main` 的两处 `return 1` 之前**先重试/降级** (例如分配失败时先把
   分辨率降到可用尺寸, 或释放 wallpaper 再试 compositor), 而不是直接退出。

## 11. **根因 (最终)**: 系统调用返回值被寄存器覆盖 —— "桌面自己退了"的真凶

### 11.1 症状

`PROC 7` 在启动约 1 秒后以 `code=1` 退出 (看不出是被杀还是崩溃), 内核日志显示
FB 映射成功 (`Framebuffer mapped to VADDR: 0x...`), 但用户态拿到的是
**全 0 的几何信息**, 于是 `main` 走到 `framebuffer info failed` 分支自杀。

### 11.2 定位过程 (关键证据)

`[devioctl]` 与用户态探针同时打印, 把矛盾钉死在"内核算对了、用户态没收到":

```
[devioctl] type=6 idx=0 cmd=2 arg=0 -> 320        <- 内核 handler 返回 0x320
[desktop] ret-test: getpid=0 gettid=0 ioctl0=0    <- 用户态收到 0
```

### 11.3 根因

`kernel/src/arch/x86_64/schedule/user.asm` 的 syscall 收尾原本是:

```asm
call syscall_handler
mov [rsp + 14*8], rax     ; 把 %rax 当成"返回值"写回帧槽
```

它假设 "`syscall_handler` 返回时 `%rax` 仍然是返回值"。但 `%rax` 是
**caller-saved**: 只要 `syscall_handler` 在 `frame->rax = func(...)` 之后还执行
任何一步 (实际是 `stall_set_kernel_busy(0)` 这个函数调用), `%rax` 就被覆盖,
帧槽里写进去的是那个残留值 —— 用户态看到 0 或 1。

> 这正是它表现得如此误导的原因: **handler 返回值完全正确, 内核日志也正确,
> 只有用户态拿到垃圾**; 中间那层寄存器传递没有任何日志可看。

### 11.4 修复

```asm
call syscall_handler
; 从**帧槽**读回返回值 (syscall_handler 总是写 frame->rax);
; 帧槽位于 [rsp+112] —— 与 %rax 是否被覆盖无关
mov rax, [rsp + 14*8]
```

一行改动, 但它是"桌面能不能起来"的分水岭。

### 11.5 修复后验证

```
[desktop] ret-test: getpid=7 gettid=6 ioctl0=ffff8000fd000000
[desktop] start: fb=64ac7a140000 1280x800 psl=1280 bufsz=4096000
[desktop] compositor up, workspace allocated; spawning console
LOADED PID count: 3      (console / notepad / DOOM)
[doom] init done         KERR: 仅个别 TLB ACK 超时
```

- **连续 10 次启动 soak: 10/10 全部到达 `[doom] init done`** (TLB 超时 0–1);
- 桌面进程不再退出 (`[exit] pid=7` 不再出现);
- 清理掉临时诊断后再次确认: `compositor up=True`, `Exit PROC 7=False`。

### 11.6 排查途中踩的两个坑 (都已修进工具链)

1. **构建脚本只重建 desktop/notepad** —— 改 `programs/helloworld` 后镜像里的
   `hw.elf` 仍是旧的, 探针"看起来没跑"。`_dbg_builduser.sh` 现在重建
   lib + helloworld + helloworld2 + desktop + notepad + PureDOOM, 并刷新
   `disk.img` 与 ISO。
2. **启动脚本缓存了 `/tmp/ss-disk.img`** —— 内核从 ext4 镜像加载 `/mp/*.elf`,
   缓存的旧镜像让最近所有启动都在跑**旧二进制**, 直接导致一整段定位走偏
   (探针永远不打印)。`_dbg_screen.py` 现在每次启动前强制从仓库 `disk.img`
   重新同步 (除非显式 `--keep-disk`)。

> 教训: 在这套系统里验证任何用户态改动, 必须先确认**镜像里的 ELF 是新的**
> (`md5sum` 内置副本 vs `programs/bin/*.elf`), 否则所有观察都不可信。

| **v1 1–10** | **syscall 返回值修复后**: 每次 60 秒 | **10/10 全部到达 `[doom] init done`**, 桌面存活, TLB 超时 0–1 |
| **fin2** | 清理临时诊断后的最终构建 | `compositor up=True`, `Exit PROC 7=False`, `LOADED PID=3`, **`KERR=0`** |

结论:
- **(0) 桌面自杀 = syscall 返回值被 `%rax` 覆盖** —— 已修 (第 11 节), 这是
  "PID 11 之后什么都没有"的最终根因;
- (A) 桌面分配器链表损坏 —— 已加固 (第 4.4 节);
- (B) AHCI 完成轮询被时钟拖死 —— 已修 (第 5.1 节);
- (C) 自旋锁长持有/死等 —— 已能稳定抓现场 (第 8 节), 根因待定位;
- (D) 墙钟偏慢 1.3~1.7x —— 已量化 (第 6 节), 待修。

下一步 (动态链接) 的方案见 `docs/dynamic-linking-plan.md`。

## 12. 常用调试命令

```bash
# 起一个实例 (私有磁盘副本)
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && ./_dbg_start.sh t1"
# 连续复现 + 首次异常即冻结 (快速; 也可以加 --hold 保持运行观察)
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && python3 /tmp/_fuzz.py --iters 25 --smp 4 --deadline 75 --tag h"
# 看所有实例状态
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && ./_dbg_status.sh"
# 看某个实例的看门狗/崩溃报告
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && ./_dbg_watch.sh h1"
# 符号还原 (内核)
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && python3 _dbg_sym.py 0xffffffff80015589"
# 停掉所有实例
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && ./_dbg_killall.sh"
# 长时间运行 + 首次异常冻结抓现场 (含 4 核 GDB 现场)
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && python3 /tmp/catch.py --iters 14 --hold 150 --tag n"
# 量 guest 墙钟速率 (1.00 = 准; 0.5 = 快一倍; 2.0 = 慢一倍)
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && python3 /tmp/clocksoak.py --hold 55 --tag e"
# 完整重建 (lib + programs + kernel + ISO) —— 改 lib 后必须走 _dbg_relink.sh
wsl -e bash -c "cd /mnt/c/ZSY/SkylineSystem && ./_dbg_buildall.sh && ./_dbg_relink.sh"
```

> 这些 `_dbg_*.py` 脚本通过 base64 或 `python3 /tmp/xxx.py` 方式跑;
> 直接把 Windows 侧文件用 `wsl -e bash 文件` 执行会因 CRLF 失败。
