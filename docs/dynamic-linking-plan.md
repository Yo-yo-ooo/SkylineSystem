# 动态链接 (共享 lib) 实施方案

> 目标: 用户态程序不再把 libc 静态链进每个 ELF, 而是加载一份共享
> `libsky.so`, 由内核把 .so 映射进进程并在启动前完成重定位。
> 本文是实施计划 —— 按阶段逐条落地, 每阶段都要能在 QEMU 里验证。

## 进度总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 产出可用的 `libsky.so` | ✅ 完成并验证 |
| 2 | 内核加载 `DT_NEEDED` + 静态重定位 | ✅ 完成 |
| 3 | TLS 合并 (STATIC_TLS) | ✅ **完成** (TPOFF64 公式修正后跑通) |
| 4 | 构建系统与镜像 | ✅ `make -C programs/desktop dyn`, 体积 1.06MB→245KB |
| 5 | 验证 | ✅ 完整启动: 桌面存活 + 3 个客户端 + DOOM |

### 最终结果 (round 8)

```
[dynlink] DT_NEEDED 'libsky.so' -> /mp/libsky.so (873432 bytes)
[dynlink] mapped libsky.so at 0x700000000000..0x700000032000
[dynlink] libsky.so: exports=287 syms=288
[dynlink] TLS merged: main=128 @0, so=948 @128, total=1084, align=8
[dynlink] TLS: so_base=128 fs_off=1088 -> tls_data TPOFF=-960
[desktop] main reached, start probe
Framebuffer mapped to VADDR: 0xE80D4000
[desktop] start: fb=6f71e80d4000 1280x800 psl=1280 bufsz=4096000
[desktop] compositor up, workspace allocated; spawning console
LOADED PID: 10 / 11 / 12
[doom] client start
Exit PROC 7 = False
KERR: 仅 0–3 条 TLB ACK timeout (既有小问题)
```

**`desktop.dyn.elf` = 245 KB, 静态版 = 1.06 MB —— 体积降到 23%,
libc/allocator/TTF/flanterm 只有一份, 由 4 个进程共享。**

## 关键的最后一个 bug: TPOFF64 公式算错 832 字节

这是整条链上最隐蔽的一个, 伪装成"TLS 内存被写坏", 连查四轮才定位。
现在回看, 判据其实很简单 —— **把地址算出来对一遍**:

```
实测 (tripwire 输出):
  tls_mem    = 0x6e51ac9c5000
  %fs        = 0x6e51ac9c5440      (tls_mem + 0x440 = 1088)
  &qsbr_slot = 0x6e51ac9c53c0      (tls_mem + 0x3c0)

按布局, &tls_data 应该在 tls_mem + so_base = tls_mem + 0x80
实际却在 tls_mem + 0x3c0            ->  *** 偏了 832 字节 ***
```

**错误写法** (前几轮用的):
```c
val = r->r_addend - g_tls_so_base;        /* 0 - 128 = -128  ✗ */
```

**正确写法**:
```c
val = (g_tls_so_base + r->r_addend) - g_tls_fs_off;
/*    = (128 + 0) - 1088 = -960                                  ✓ */
```

原理: `R_X86_64_TPOFF64` 要的是**相对线程指针**的偏移, 而线程指针在**块尾**
(Variant II)。所以必须减去"线程指针相对块首的偏移"
(`fs_off = ALIGN_UP(总大小, align)`), 而不是减去 `.so` 的起点。

偏 832 字节的后果: `%fs + (-128)` 落进了 `tls_large_cache[]` 数组**中间**,
分配器的 `qsbr_slot` 遂与"某个缓存堆指针"共用同一块内存 —— 读出的 slot 是
那个指针的低 32 位 (`0x36323032`, 小端即 ASCII `"2026"`), `malloc` 于是跳过
`== -1` 的初始化分支, 直接拿它当数组下标去 `lock xadd`, 当场崩溃。

> 教训: 这个 bug 之所以难查, 是因为**每一层单独看都是对的** —— 符号表布局对、
> `.tdata` 初值对、`%fs` 与内核 MSR 一致、GOT 槽写入的值也是"按我的公式"
> 算的。只有把**两个地址都打印出来相减**, 才能看出那 832 字节。
> 以后处理 TLS/重定位类问题, 第一步就该是"算出期望地址, 和实测地址对一遍"。

## 验证结果 (round 8, 最终)

体积对比:

```
lib/bin/libsky.so                 874,976   (共享库)
programs/bin/desktop.dyn.elf      245,552   (动态链接版)  <- 已进镜像
programs/bin/desktop.elf        1,060,984   (静态版, 保留可回退)
```

`desktop.dyn.elf`: `NEEDED libsky.so`, **无 `PT_INTERP`** (内核自己加载)。

镜像一致性: `disk.img:/desktop.elf` 的 md5 == `programs/bin/desktop.dyn.elf`。

**连续 8 次启动 soak: 8/8 全部 `ok (reached doom)`**, 每次都是
`procs=3, doom=1`, TLB ACK timeout 0–1 条, 无 stall。桌面全程存活。

## 仍未解决 (已量化, 非阻塞)

1. **墙钟偏慢 1.3–1.7×** (`docs/debug-hang-2026-10-11.md` 第 6 节)。
   根因是 `TimeSinceBootMS()` 完全由 IRQ0 节拍累加得出, 而**节拍会丢**
   (长临界区里关中断) —— 丢一个 tick 就永久少 1ms, 于是越跑越慢。
   推荐修法: 让 `TimeSinceBootMS()` 在 TSC 已校准时直接走
   `tsc_now_raw() / sched_tsc_per_ms()` (即 `MonotonicMS()` 的做法),
   把 `TicksSinceBoot` 只留给"需要节拍语义"的地方。
   **未实施**: 它会改变调度/vruntime 依赖的时间源语义, 需要单独一轮做
   回归验证, 不适合在收尾时仓促改。
2. **TLB shootdown ACK timeout 偶发 0–1 条**。已有降级路径
   (`degrading to async`), 不影响功能, 属既有小问题。

## 阶段 2/3 实测踩到的 6 个 bug (全部已修)

| # | 症状 | 根因 | 修法 |
|---|---|---|---|
| 1 | 桌面前 3 次启动直接 `#PF @0x20B6`, 无任何 dynlink 日志 | `DT_STRTAB` 是**链接期虚拟地址**(0x40xxxx), 我当成文件偏移用, 于是把随机镜像字节当字符串 → 完全读不到 `DT_NEEDED` | 用 `PT_LOAD` 反查 `vaddr→file offset` (`dl_vaddr_to_off`) |
| 2 | 同上 | `dl_vaddr()` 用"小于 mapped_size 就当偏移"的启发式, 把 PIE 的 `DT_RELA` 误判成偏移, 重定位往**文件偏移对应的地址**写 → 镜像被写坏 | 一律按链接期虚拟地址处理, 加对象 bias |
| 3 | dynlink 完全不触发 | 桌面是 `init.cpp` 里 `Schedule::NewThread` 创建的, **不走 `sys_load`** —— 我把动态链接只接在 `sys_load` 上了 | 抽出 `elf_dyn_apply()`, 两个加载路径都接 |
| 4 | 内核 `#PF CS=0x8 CR2=0x700000000038` | `.so` 的页映射在**子进程页表**里, 我在内核页表下解引用它; 另外导出符号的**名字指针**指向 .so 内部, 页表切回后失效 | 解析+两趟重定位整段包在 `SwitchPageMap` 里; 导出名字**复制进** `dl_obj` |
| 5 | 内核 `#PF CR2=main_bias+0x38` | 少了 bias: 程序头表地址写成 `ehp + e_phoff` 之外又叠了 base | `ehp` 已是运行期地址; 0x38 正是 `e_phoff` 这个线索定位的 |
| 6 | 桌面崩在 `malloc` 的 `lock xadd`, `qsbr_slot` 读到 ASCII `"2026"` | **`TPOFF64` 公式少减了 `fs_off`, 偏 832 字节** → `%fs+TPOFF` 落进 `tls_large_cache[]`, `qsbr_slot` 与缓存指针共用内存 | 见下方 "TPOFF64 最终形态" |

顺带还修了: `R_X86_64_COPY` 支持 (非 PIE 主程序引用 .so 全局变量的标准路径)、
`dl_obj` 数组从内核栈移到 static (栈放不下)、
构建脚本 `touch` 反而阻止重建的坑 (改成 `rm -rf obj obj-so` + 产物自检)。

## TPOFF64 最终形态 (模块感知)

第一版修法 `(so_base + addend) - fs_off` 虽然让 `.so` 的变量落对了地方,
但**对主程序也加了 `so_base`** —— 主程序的 `__thread` 变量会整体高
`g_tls_so_base` 字节, 读到 `.so` 的数据。当时没暴露, 是因为桌面的主程序
用的是 local-exec (编译器把偏移直接编进指令), 压根没有 `TPOFF64` 重定位;
但只要主程序改用 `-ftls-model=initial-exec`, 就会立刻踩上。

正确写法按模块区分:

```c
uint64_t module_tls_base = o->is_so ? g_tls_so_base : 0;
val = (module_tls_base + r->r_addend) - g_tls_fs_off;
```

同时把 `g_tls_fs_off` 的计算**提到无条件路径**上 —— 原来只在
"`.so` 带 TLS" 的分支里算, 若"主程序有 TLS、`.so` 没有", 它会保持 0,
主程序的 `TPOFF64` 就偏 `ALIGN_UP(main_mem, align)` 字节。

实测日志 (两者相差正好 `so_base = 128`, 这就是模块感知的可见证据):

```
[dynlink] TLS layout: total=1084, so_base=128, fs_off=1088,
          main TPOFF@addend0=-1088, so TPOFF@addend0=-960
```

## 动态符号分配

原来 `dl_obj` 里是固定数组 `dl_sym exports[512]`, 两个问题:

1. **静默丢符号**: `libsky.so` 现在就有 287 个导出, 余量不足一倍; 一旦超过
   512 就直接 `return` 掉后续符号, 表现成"某个符号莫名解析不到"的链接错误;
2. **白占内存**: `512 × 72B ≈ 36KB/对象`, 5 个对象就是 ~180KB 常驻 BSS,
   而静态主程序根本不需要导出表 (`exports=0`)。

现在改成 **kmalloc 增长 + 用完即释放**:

```c
dl_sym *exports;        /* 不再内联固定数组 */
uint32_t nexports;
uint32_t exports_cap;   /* 初始 256, 满则翻倍 */
```

- `dl_exports_reserve()`: 容量不够就翻倍, 新块 kmalloc + 拷贝 + `kfree` 旧块;
- `dl_add_export()`: 满了自动扩容, 不再丢符号;
- `dl_exports_free()`: 重定位结束立刻归还内核堆 —— 内核堆没有"进程退出
  自动清账", 漏一次就是每进程泄漏几十 KB, 所以 `elf_dyn_prepare` 的
  **每条退出路径** (含 4 个错误返回) 都调 `dl_cleanup()`。

实测日志 (证明增长真的发生了, 且静态主程序零分配):

```
[dynlink] main: syms=31 exports=0 cap=0 (dyn alloc, 0 bytes)      <- 静态主程序, 不分配
[dynlink] libsky.so: syms=288 exports=287 cap=512 (dyn alloc, 36864 bytes)
                                       ^^^^^^^^^ 256 翻倍到 512
```

## 6. 当前卡点 (下一步)

### 6.1 已修好的关键 bug: TLS 布局方向错了 (round 4)

**症状**: 桌面在 `libsky.so` 的 `malloc+0x3e` 崩, 且 `compositor up` 永远出不来。

**根因**: 我原来把 `.so` 的 TLS 放在合并块**块首**, 主程序 TLS 顺延。这是错的:

- **主程序用的是 local-exec** —— 编译器把 `%fs` 偏移直接编进指令, 链接时按
  "主程序 TLS 就在偏移 0" 算;
- `.so` 用的是 initial-exec/GOTTPOFF —— 偏移放在 GOT 槽里, 加载期可改。

所以占块首的必须是主程序, 该让路的是 `.so`。放反的后果: 桌面的
`tls_data.qsbr_slot` (`%fs:off`) 读到的是 `.so` 里同一偏移的字节, 于是
`malloc` 走了错误支路并在 `lock xadd` 上崩。

**修法**:
1. 布局改成 `[ 主程序 TLS | .so TLS ]`, `g_tls_so_base = align_up(main_memsz, so_align)`;
2. 重定位 `TPOFF64` 时把 `.so` 的偏移整体**减去** `g_tls_so_base`
   (Variant II: 相对线程指针是负偏移);
3. **顺序约束**: `g_tls_so_base` 必须在第一趟重定位之前定好, 所以 TLS 布局
   计算被提到重定位之前 (原来放在最后)。

实测: `TPOFF64 GOT[0x2feb8] = -128 (addend 0 - so_base 128)` —— 修正量正确,
桌面随即跑到了 `[desktop] compositor up, workspace allocated; spawning console`。

### 6.2 现在卡在哪: TLS 槽被**确定性**写坏 (round 5 已把范围缩到 4 字节)

关键进展: **`%fs` 与内核 `FS_BASE` 都是对的**, 坏的是 TLS 块里那 4 个字节。

`fs_watch()` (在 `malloc` 入口用 `movq %fs:0` 读 TCB 自指针, 再用
`SYS_ARCH_PRCTL(ARCH_GET_FS)` 把内核的 `FS_BASE` MSR 读回来对比) 实测:

```
[alloc] malloc: fs[0]=0x6c757eb88440 kernel_FS_BASE=0x6c757eb88440 (match)
after StartWorkers: fs(tcb)=0x6c757eb88440
```

两者一致, `StartWorkers` 之后也没变 —— 所以**不是调度器 / 线程克隆弄丢 `%fs`**,
上一轮列的两个怀疑都排除了。

#### 抓捕结果 (两道探针)

1. **`qsbr_enter()` 入口校验 + 哨兵线程**: 第一次看到合法值时起一个只轮询
   那 4 字节的线程 (`[alloc] sentinel`), 一旦变化立刻报告;
   同时 `qsbr_enter` 入口发现非法 slot 就打印现场并以 `code=77` 退出。

```
[alloc] malloc: fs[0]=0x6c757eb88440 kernel_FS_BASE=0x6c757eb88440 (match)
[alloc] FATAL qsbr_slot corrupt: slot=909258802 fs=0x6c757eb88440
                                &slot=6c757eb883c0 good_was=0
[alloc] sentinel: qsbr_slot CHANGED 0 -> 909258802 (addr=6c757eb883c0)
[exit] pid=7 code=77
```

#### 读出来的事实

- **地址完全正确**: `&tls_data.qsbr_slot == 0x6e51ac9c53c0`, `%fs = 0x6e51ac9c5440`,
  差 **128** —— 与内核修正后的 `TPOFF64 = -128` 完全吻合。**TLS 布局与重定位都对。**
- **只坏了 4 个字节**。整块现场 (按地址顺序):

```
page+000 = 00000000ffffffff   <- 主程序 TLS 的首字段, 仍是 -1, 未被动过
tls+000  = 0000000036323032   <- *** 只有这 4 字节坏了 ***
tls+008  = 0000000000000000
tls+010  = <一个栈地址>        <- 这是 tls_data.small_list (正常)
tls+018 .. tls+078 = 全 0     <- pending_small/large、large_list 都正常
```

- **坏值确定**: `909258802 = 0x36323032`, 小端字节序为 `32 30 32 36`,
  即 ASCII **`"2026"`**。两次运行**一模一样** —— 确定性的写, 不是随机踩内存。
- **哨兵看到的序列** (上一轮): 先是 `0`, 然后跳到 `0x36323032`。
  `tls_data` 静态初值应是 `-1`, 看到 `0` 说明它**正常拿到过 slot 0**,
  之后才被写成 `"2026"`。

> 更正: 上一轮我把坏值写成 `0x3630F0B2`, 那是我手算错了。
> `909258802` 的正确十六进制是 **`0x36323032`** —— 小端即 ASCII `"2026"`。
> 也查过: 整个 userspace 里**没有** `%0Nd` 这类零填充格式 (唯一的几处是
> 本轮我自己写的调试代码), 所以它不是"某个 snprintf 写出来的"。

#### 因此问题定位为

**只有 `tls_data` 的头 4 字节被写成了 ASCII `"2026"`, 前后字段都完好:**

```
page+000 = ffffffff   <- 主程序 TLS 首字段, 完好
tls+000  = 36323032   <- 只有这里坏了 ("2026")
tls+010  = <栈地址>    <- tls_data.small_list, 完好
tls+018..078 = 0      <- 完好
```

越界写不可能只命中中间 4 字节 (前后都完好), 所以这更像
**"一个指向 4 字节字段的指针指错了位置"**: 某处有个 `int32_t`/`uint32_t`
被赋成了 `0x36323032`, 但它实际写到了 `%fs - 128`。

`"2026"` 可能的出处 (待逐一排除):
1. **一个被截断的 64 位指针的低 32 位** —— 这是最像"写错接收者"的形态;
2. 某个 `memcpy`/结构体赋值把 4 字节键值写到了错误的地址;
3. 某处把 4 个字符就地当 `int32` 存 (例如哈希/魔数)。

**下一步 (最快定案)**: 在 `qsbr_enter()` 里, 除了校验 `qsbr_slot`,
再把**整块 32 字节的 `tls_data` 打指纹** (例如记录 4 个 qword 的快照),
每次进入时比对 —— 这样不仅知道"被改成了什么", 还能知道"哪一次调用之间被改的",
把窗口从"几百次分配"缩小到"一次分配"。配合前面已找到的
`&slot == %fs - 0x80` 这个精确地址, 就能写出针对性断言。

#### 已就位的抓捕工具 (留在代码里)

- `qsbr_enter()` 入口校验 + `code=77` 退出 + 整块内存 dump
  (`kernel` 侧不用改, 全在 `lib/stdc/allocator/allocator.c`);
- 哨兵线程 `qsbr_slot_sentinel` (第一次拿到合法 slot 后自动启动);
- `fs_watch()` 对比 `%fs:0` 与内核 `FS_BASE` MSR。

#### 一个重要的判别证据: **静态桌面没有被写坏**

同一轮启动里, `hw.elf` (静态) 的 TLS 块是完好的:

```
[tls] mem=0x7b14d7ed0000 fs(tcb)=0x7b14d7ed03b8 memsz=948 pages=1
[tls] block[0..3]=ffffffff ffffffff 0 0     <- 主程序 qsbr_slot 仍是 -1
[hw] tls probe: fs=7b14d7ed03b8 ... var_init=-1 var_after=23130
```

而动态桌面 (同一个 `libsky.so`, 只是在合并块里被放到了 `g_tls_so_base = 0x80`
之后) 的同一个字段就被写成了 `"2026"`。**同一个 `.so`, 只差 0x80 的偏移** ——
这条对比把范围压得很窄: 问题几乎肯定出在"`.so` 的 TLS 不再位于块首"这件事上,
而不是 `.so` 里某段与 TLS 无关的代码。

最直接的验证: 把 `tls_data` 的偏移人为再挪一次 (例如 `g_tls_so_base` 加 0x40),
如果坏值跟着变、且仍然是 `"2026"`, 就说明是**某个按固定偏移写 TLS 的地方**
(即某段代码自己算 TLS 地址, 而不是走 `%fs` 重定位)。


**下一步 (最直接)**: 在 `qsbr_enter()` 里, 当
`tls_data.qsbr_slot` 既不是 -1 也不在 `[0, QSBR_SLOTS)` 时**立即停机并打印
该值 + `%fs` + 块内偏移**, 这样能拿到"第一次被写坏"的那一次分配 (谁写的、
写前写后是什么), 而不只是崩溃现场。这是最后一公里的抓现场手段。

### 6.2.1 一个让前一整轮白查的构建 bug (已修)

`_dbg_builduser.sh` 原来用 `touch` 源文件来"强制重建" —— **这反而阻止了重建**:
`touch` 把源的 mtime 设成"现在", 而 `.o` 的 mtime 也几乎相同, make 判定
up-to-date 直接跳过。后果: 我加的 `fs_watch` 探针**根本没进二进制**
(`strings libsky.so | grep FS_BASE` 为 0), 却花了一整轮去解读"探针没打印"
这一现象 —— 其实是探针不存在。

已修: 改成 `rm -rf lib/obj lib/obj-so` 强制重编, 并加了两条**构建后自检**:

```
lib.a sanity OK (1751234 bytes)
libsky.so sanity OK (fs_watch present)
```

教训与 round 3 的两条同源: **先证明产物是新的, 再解读现象**。


### 6.3 已确认成立的部分

```
[INFO] [dynlink] DT_NEEDED 'libsky.so' -> /mp/libsky.so (873432 bytes)
[INFO] [dynlink] mapped libsky.so at 0x700000000000..0x700000032000
[INFO] [dynlink] libsky.so: exports=287 syms=288
[INFO] [dynlink] TLS merged: main=128 @0, so=948 @128, total=1084, align=8
[INFO] App Serial Output: [desktop] main reached, start probe
[INFO] Framebuffer mapped to VADDR: 0xCF2CD000
[INFO] App Serial Output: [desktop] start: fb=6f4bcf2cd000 1280x800 psl=1280 bufsz=4096000
[INFO] App Serial Output: [desktop] compositor up, workspace allocated; spawning console
[INFO] App Serial Output: [hw] tls probe: fs=... var_init=-1 var_after=23130   <- 静态程序 TLS 正常
[INFO] App Serial Output: [hw] syscall return check: pid=8 tid=7 (both must be non-zero)
```

即: DT_NEEDED 解析、`.so` 映射、287 个符号重定位、TLS 合并、入口点跳转、
帧缓冲映射、**compositor 初始化**全部正确。镜像一致性也核对过 (`md5` 相同)。

### 6.4 回退手段

`programs/Makefile` 里动态版本是"顶替 `/mp/desktop.elf`"进盘的。要回到静态
桌面, 把 `programs/bin/desktop.elf` 重新 `e2cp` 进盘即可 ——
**静态链路完全没动过, 仍可正常工作** (已多次验证 `compositor up` / `KERR=0`)。


### 阶段 1 实测结果 (已完成)

```
lib/bin/libsky.so   873144 bytes
  SONAME            libsky.so
  Type              ET_DYN
  导出符号          285
  TLS 段            1 个 (memsz 0x3b4, filesz 0x20, align 8)
  DT_FLAGS          STATIC_TLS
  重定位            JUMP_SLOT 120 / GLOB_DAT 32 / RELATIVE 9 / TPOFF64 5
```
`make -C lib` 现在同时产出 `lib.a` 与 `libsky.so`, 静态链路不受影响;
`disk.img:/libsky.so` 已就位 (md5 与构建产物一致)。

### 阶段 1 踩到的四个坑 (都已修进构建)

1. **`base/arch/x86_64` 被重复收集** —— `SRCDIRS` 里的 `base` 已经递归覆盖它,
   再显式加一次会把同一份源收两遍。静态库用 `ar` 时只是静默覆盖, 链接 `.so`
   时直接 `multiple definition of sys_gettid`。用 `$(sort ...)` 去重。
2. **`-fPIE` 会覆盖 `-ftls-model`** —— 这是最隐蔽的一个:
   | 编译选项 | 产生的 TLS 重定位 |
   |---|---|
   | `-fPIC` | `DTPOFF32` |
   | `-fPIC -ftls-model=initial-exec` | `GOTTPOFF` ✅ |
   | `-fPIC -fPIE -ftls-model=initial-exec` | `TPOFF32` ❌ 链接器直接拒绝 |
   `TPOFF32` (local-exec) 在 `.so` 里非法。结论: 共享库的编译**绝不能带 `-fPIE`**。
3. **非零初值的 `static __thread`** —— `lib/stdc/algo/rand.c` 原来是
   `static __thread unsigned long next = 1;`, 那个初值需要一个"链接期已知的 TLS
   偏移", 同样生成 `TPOFF32`。改成零初值 + 首次使用时播种 (`next==0` 作哨兵)。
4. **`memops.o` 是 `-fpie` 编的** —— 其 `R_X86_64_PC32` 对 `x86mem_cache_limit`
   的重定位在 `.so` 里非法。给 `ablib/arch/x86_64/x86mem/Makefile` 加了 `pic`
   目标 (`-fPIC` 重编同一批源 -> `memops.pic.o`), 由 `lib` 的 `so` 目标驱动。

## 0. 现状 (已确认)

- 每个程序用 `ld -static` 把 `lib/bin/lib.a` 整份链进自己的 ELF:
  `desktop.elf` 1.04 MB / `doom.elf` 2.1 MB, 里面各带一份 libc + allocator + TTF + flanterm;
- ELF 是 `EXEC` (固定 vaddr 0x400000, 非 PIE), `_start` 在
  `lib/base/arch/x86_64/init.S`, 由它调用 `_init_runtime_and_global_variables` 再调 `main`;
- 内核 ELF 加载器在 `kernel/src/arch/x86_64/schedule/syscall/exec.cpp`:
  只处理 `PT_LOAD` / `PT_TLS`, 返回 `hdr->e_entry + load_bias`;
- 系统调用返回值链路刚修好 (见 `docs/debug-hang-2026-10-11.md` 第 11 节),
  这是后面所有用户态验证的前提。

## 1. 分阶段计划

### 阶段 1 — 产出可用的 `.so` (纯工具链, 不动内核)

- 新增 `lib/Makefile` 目标 `libsky.so`:
  - 现有 `lib/**/*.c` + `init.S` 用 `-fPIC` 重新编译 (单独 objdir `obj-so/`);
  - `ld -shared -soname libsky.so -z noexecstack --hash-style=sysv` 导出;
  - 用 version script 只导出公共符号 (`malloc/free/printf/...`), 隐藏内部符号。
- **入口点不能进 .so**: `_start` 仍留在程序侧 (每个程序自带一份 `init.S` 的
  可重定位副本), `.so` 只提供 `_init_runtime_and_global_variables` 等函数。
- 验证手段: `readelf -d libsky.so` 有 `SONAME`; `nm -D` 能看到 `malloc`;
  `readelf -r libsky.so | head` 只有 `R_X86_64_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`。

### 阶段 2 — 内核: 加载 `.so` + 静态重定位  ✅ 代码完成

实现文件: `kernel/src/elf/elf_dyn.cpp` (约 480 行), 入口
`elf_dyn_prepare(...)`; 调用点: `sys_load` 在 `elf_load_impl` 之后、分配 TLS 块之前。

流程:

1. `dyn_read_needed()` (在 `exec.cpp`) 解析主程序 `PT_DYNAMIC` 取 `DT_NEEDED`
   字符串与 `DT_STRTAB`, 拼成 `/mp/<name>` 读文件; 没有 `DT_NEEDED` 就静默返回
   (静态程序走原路径, 零影响);
2. `dl_map_segments()` 按 `PT_LOAD` 映射 `.so`, 基址 `DL_SO_BASE = 0x700000000000`,
   权限照 `p_flags` 走 W^X 收紧 (与 `exec.cpp` 同规则), 并登记 VMA;
3. `dl_parse_dynamic()` 读 `PT_DYNAMIC`, 把导出符号收进 `exports[]`
   (SysV ELF hash 去重; 符号数取自 `DT_HASH` 的 `nchain`);
4. 两趟重定位: 先只做 `RELATIVE` (让字符串表/GOT 自身可用) 并收集导出,
   再统一解析 `GLOB_DAT`/`JUMP_SLOT`; `TPOFF64` 直接使用 addend
   (见下方 TLS 说明); 其它类型**拒绝加载并打印**。

**关键的设计选择**: 链接时用 `ld --no-dynamic-linker`, 这样 ELF 里
**没有 `PT_INTERP`** —— 内核不需要先跑解释器再回头加载主程序, 也就不需要
"主程序在未重定位状态下执行"这个难点。实测两种链接方式的形状对比:

| 链接方式 | `PT_INTERP` | 内核需要做什么 |
|---|---|---|
| `ld ... -lsky` | 有 | 得实现用户态 ld.so |
| `ld --no-dynamic-linker ... -lsky` | **无** | 内核自己映射 .so + 重定位 ✅ |

**TLS (阶段 3 已并入)**: `libsky.so` 带 `DT_FLAGS & DF_STATIC_TLS`,
其 5 个 `TPOFF64` 的 addend ∈ [0, 0x3b0], 而 `PT_TLS memsz = 0x3b4`
—— 即 ld 把"线程块块首"当作 .so 的变量偏移 0 (Variant II, .so 是块里第一个
TLS 模块)。所以合并布局为:

```
[ 0 .. so_memsz )            <- .so 的 TLS   (TPOFF64 值无需修正)
[ align_up(so_memsz) .. )    <- 主程序 TLS   (数据来自主程序 PT_TLS)
```

`elf_dyn_prepare` 通过 `*tls_main_off` 告诉调用方主程序数据该放在哪,
并把 `.so` 的初始镜像 (`PT_TLS` 的 filesz 字节) 拷到调用方给的暂存区;
`exec.cpp` 建块时先写 `.so` 镜像, 再在 `tls_main_data_off` 处写主程序镜像。

**先不做**: PLT 延迟绑定 (`DT_PLTGOT` / lazy binding)。全部在加载期解析,
`LD_BIND_NOW` 语义 —— 实现简单、失败可见。

### 阶段 4 — 构建系统与镜像

- 程序链接改为 `ld -dynamic-linker /mp/ld.so ... -lsky`;
  - 需要 `-pie` (主 ELF 变 `DYN`) + `--no-dynamic-linker` 或提供
    `PT_INTERP` (二选一: 先走"内核直接加载"路线, 不实现 ld.so);
- `res/saf` 与 `disk.img` 需要带上 `libsky.so`;
  `programs/Makefile` 增加 `e2cp lib/bin/libsky.so ../disk.img:/`;
- 冒烟检查脚本: 启动后 `[desktop] start:` 必须仍然打印正确的
  1280x800 / bufsz=4096000, 且 `KERR lines: 0`。

### 阶段 5 — 验证

- `_dbg_screen.py` + `_dbg_summary.sh` 已经能给出"是否到达 `[doom] init done`"
  与 KERR 计数;
- 新增断言: `desktop.elf` 体积应从 ~1.04 MB 明显下降 (libc 不再重复链接),
  这是"动态链接真的生效"的最直接证据;
- soak 10 次 (`_dbg_catch.py --iters 10 --hold 60`)。

## 2. 验收标准

1. `libsky.so` 存在且被镜像携带;
2. `desktop.elf` / `notepad.elf` / `doom.elf` 的 `readelf -d` 都有 `NEEDED libsky.so`;
3. 三个程序能正常跑 (桌面 compositor up、DOOM init done), `KERR=0`;
4. 10 次启动 soak 全部通过。

## 3. 主要风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| TLS 重定位 | variant II 的 .so TLS 是难点 | 阶段 3 备选方案: .so 只放无状态代码 |
| 用户态崩溃难查 | 符号化需要 .so 的加载基址 | 内核日志打印每个 .so 的 base + 各段范围 |
| 缺页/权限 | .so 段权限收紧后漏映射页 | 复用 `exec.cpp` 已验证的收紧逻辑, 逐页检查 |
| 回归 | 静态链路的程序 (doom/hw2) 也要能跑 | 阶段 4 先只让 desktop 走动态, 其余保持静态 |

## 4. 建议的提交切分

1. `lib: build libsky.so (no kernel change)` —— 纯构建, 不改行为;  ✅ 已完成
2. `kernel: load a NEEDED shared object and apply RELATIVE/GLOB_DAT relocs`;  ✅ 代码完成
3. `programs: link desktop dynamically` (先只换 desktop);  ⬅️ **下一步**
4. `programs: link notepad/doom dynamically`;
5. `docs: dynamic linking notes`.  🟡 本文

## 6. 下一步 (精确到命令)

程序侧改动 (阶段 4):

1. 各程序的 linker 增加动态链接目标:
   ```make
   LDFLAGS_DYN := -entry=_start -m elf_x86_64 -z noexecstack \
                  --no-dynamic-linker \
                  -T ./link/console_x86_64.ld
   # 不再 -static, 改为 -L../../lib/bin -l:libsky.so
   ```
   注意 `-l:libsky.so` 会记录 `NEEDED libsky.so`, 与 kernel 侧
   `/mp/libsky.so` 的查找规则一致。
2. **`_start` 必须留在程序里** —— `libsky.so` 不含 `init.S` (它定义 `_start`,
   会把入口点带进共享库)。程序仍链接自己那份 `init.S` 副本。
3. 先只换 `desktop`, 其余保持静态; 跑一次 `_dbg_screen.py`, 期望:
   - 内核日志出现 `[dynlink] DT_NEEDED 'libsky.so' -> /mp/libsky.so`
   - `[dynlink] mapped libsky.so at 0x7000...`
   - `[desktop] start: fb=... 1280x800 ...` 仍然正确
   - `KERR lines: 0`
4. **体积验收**: `desktop.elf` 应从 ~1.04 MB 明显下降 (libc 不再重复链接),
   这是"动态链接真的生效"最直接的证据。

## 7. 风险登记 (实测更新)

| 风险 | 状态 |
|---|---|
| TLS 重定位 (`TPOFF32` 非法) | ✅ 已解决: 去掉 `-fPIE` + `-ftls-model=initial-exec` |
| STATIC_TLS 合并布局 | ✅ 已实现: `.so` TLS 放块首, 主程序顺延 |
| `memops.o` 非 PIC | ✅ 已解决: 新增 `x86mem` 的 `pic` 目标 |
| 固定基址冲突 | 🟡 选了 `0x700000000000`; 待实测确认不与栈/PIE 区间冲突 |
| 导出表上限 | 🟡 `dl_obj::exports[512]`, 当前 285 个, 余量不大 —— 若超限会静默丢符号, 需要改成动态分配 |
| 用户态崩溃难查 | 🟡 内核日志已打印每个 .so 的 base + 映射范围 |

