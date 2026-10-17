# 客户端动态链接：当前卡点与下一步

## 为什么客户端不能用 `-pie`

`lib/include/graphic/winstyle.h`:

```c
#define SKYWIN_PROTO_PAGE_VA  0x400000UL  /* client fixed .prepad protocol page */
```

这个 0x400000 **是编译期常量**，被三方直接使用：

- `lib/stdc/outfb/printf.c`（libsky 自己的 console 协议写入口）
- `programs/notepad/main.cpp:130`
- `programs/PureDOOM/skyline/doom_main.c:427`

而 `programs/desktop/loader.cpp:218` 是这样把页交给客户端的：

```c
uint64_t r2 = sys_pmmapSHARE(self, 0, PAGE_SIZE, SHARE_FLAGS,
                             (uint64_t)pid, SKYWIN_PROTO_PAGE_VA);
```

注意参数顺序 `(dst_pid, dst_addr, length, flags, src_pid, src_addr)`：
**客户端是 src，0x400000 是 src_addr**。也就是说 `sys_pmmapSHARE` 共享的是
客户端**已经存在的**那一页 —— 这正是每个客户端链接脚本里 `.prepad` 页存在的
理由（`PureDOOM/Makefile` 注释：少了它，ELF 自己的只读头部会落在 0x400000，
第一次 `OUT_SEQ` 就写只读页崩在 `addr=0x400048`）。

结论：**客户端必须保留真实页在 0x400000**，因此不能做 PIE（PIE 会被内核
搬到随机地址，0x400000 变成空地址，`pmmapSHARE` 直接失败）。
唯一可行路线是 **ET_EXEC（固定 VA）+ DT_NEEDED**。

## 已经做通的部分

1. **链接脚本**：`console_x86_64.ld` 用自定义 `PHDRS` 时，把
   `.dynamic/.dynsym/.dynstr/.hash/.rela.*/.plt/.got` 全当孤儿段丢掉了，
   链出来是"有 PLT、有 JUMP_SLOT、却没有 `.dynamic`"的畸形 ELF。
   补齐（含 `phdr_dynamic PT_DYNAMIC`）后，链接产物正确：

   ```
   NEEDED   libsky.so
   PLTGOT   0x405000
   JMPREL   0x403260
   5 R_X86_64_JUMP_SLOT
   类型 EXEC   入口 0x401000   71,576 字节（静态版 884,680）
   ```

2. **`_start` 来源**：`libsky.so` 有意不含 `lib/base/arch/x86_64/init.S`
   （`_start` 属于可执行程序），动态链接时必须显式把它编进去，否则 ld 会
   警告"找不到项目符号 _start"并把入口兜底成 0x401000。

## 当前卡点：内核 ELF 加载器写只读页（已修）

把上面这个 ET_EXEC 动态版放进镜像启动，内核在**加载 hw2 时**缺页：

```
ELF LOADER PT_LOAD: vaddr=[0x400000~0x401000], filesz=0x1000, memsz=0x1000, flags=0x6
ELF LOADER PT_LOAD: vaddr=[0x401000~0x404060], filesz=0x3060, memsz=0x3060, flags=0x5
Segfault: unmapped (addr=0000000000402000 rip=FFFFFFFF8003C663)
CR2: 0x402000   ERROR_CODE: 0x3 (present + write)   CS: 0x8 (内核态)
CR0: 0x80010033   ← bit16 = WP 置位, 所以内核写只读页同样缺页
```

符号化调用栈：

```
memcpy_fscpuf              kernel/src/klib/meme.cpp:73
 └ __memcpy                kernel/src/klib/klib.cpp:85
   └ elf_load_impl         kernel/src/arch/x86_64/schedule/syscall/exec.cpp
     └ sys_load            exec.cpp:461
```

### 根因：PT_LOAD 段可以**重叠**，而权限收紧是"每段拷完立刻做"

在拷贝前插桩打印目标页 PTE，得到 hw2 的真实段表：

```
seg va=400000 filesz=1000 flags=6  page.flags=7   (RW)
seg va=401000 filesz=3060 flags=5  page.flags=7   (RW)
seg va=402000 filesz=3040 flags=4  page.flags=5   (RO)   ← 已被上一段收紧
```

第 2 段跨 `0x401000~0x404060`（3 页），第 3 段起点 `0x402000` **落在第 2 段的
页范围内**。原来的实现是"每段：映射 → 拷贝 → 立刻按 p_flags 收紧"，于是第 2 段
收尾把 `0x402000` 设成只读，第 3 段再往同一页拷数据就缺页。

### 修法（已实施）

把权限收紧从"每段一次"改成**第二趟**：所有段都拷贝完成之后，再遍历
PT_LOAD 统一收紧权限（规则不变：可执行段去 W，不可执行段加 NX）。
这样无论段怎么重叠，拷贝阶段全是 RW，收紧只在最后发生。

验证：把 hw2 换回静态后启动 —— `PROC->ID 3`、`[doom] init done 1`、
`KERNEL FAULT 0`、`Segfault 0`、`OOM 0`，说明这个改动对现有静态程序无副作用。

## 重要更正：PIE 客户端会**静默**死掉 (code=77)

上一版这里写着"hw2 已成功动态链接并验证"。**这个结论是错的**，当时我没有看
进程退出日志，只看了 `PROC->ID: 3` 和 `[doom] init done: 1` —— 而那时 **doom
还是静态的**，hw2 其实已经死了。

把 `notepad` / `hw2` / `doom` 三个客户端都转成动态之后，启动日志暴露了真相：

```
[alloc] FATAL qsbr_slot out of range: 1179403647 (0x464c457f) fs=0x6f3f6bf6d3b8
[exit] pid=10 code=77    ← notepad
[exit] pid=11 code=77    ← hw2
[exit] pid=12 code=77    ← doom
```

`0x464c457f` 就是 **`"\x7fELF"`** —— 说明 `%fs + TPOFF` 落到了**模块镜像的
ELF 头**上，而不是 TLS 块。`code=77` 是 libsky 分配器
(`lib/stdc/allocator/allocator.c:272`) 的 TLS 护栏，不是段错误，所以之前的
断言（`Segfault` / `KERNEL FAULT`）抓不到它。

### 已做的修正（必要但不充分）

`elf_dyn_prepare()` 原来把 TCB 的 8 字节提前算进了 `tls_memsz`：

```c
*tls_memsz = so_mem + 8;        // 旧
*tls_memsz = so_mem;            // 新
```

于是 `fs_off` 从 952 变成 960，而 `.so` 是用 `-ftls-model=initial-exec` 链接的，
它在链接期就假定"线程指针 = ALIGN_UP(so_mem)"。消费方
(`exec.cpp` / `task.cpp`) 自己会加那 8 字节：

```
tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
total    = ALIGN_UP(tls_memsz, tls_align) + 8;   ← +8 在这里
```

修正后布局自洽了（`only .so: total=948, fs_off=952`，desktop: `fs_off=1080`），
**但客户端的 77 仍然出现**，说明还有第二处偏差。

### 下一轮的定位手段（一步到位）

在客户端进程里，把下面两个值同时打印出来对比：

1. GOT 里 `tls_data` 的 TPOFF 槽实际内容（`dl_relocate` 写进去的值）；
2. 运行期 `%fs`。

正确关系是 `%fs + TPOFF == tls_mem + addend`。目前 `%fs + TPOFF` 打到的是
模块基址（ELF 头），据此可以反推偏差到底来自 `g_tls_so_base`、`g_tls_fs_off`，
还是 `.so` 的 addend 本身（`.tbss` 与 `.tdata` 的相对位置）。

### 最终结果：全部 5 个程序动态链接，4/4 启动全绿

两个根因都修掉之后（TCB 8 字节不该提前计入 `tls_memsz`、以及
`tls_filesz` 被误设成 `so_file` 导致 ELF 头被拷进 TLS 块），
`notepad` / `hw2` / `doom` 全部转为 PIE 动态链接并通过验证：

| 程序 | 静态 | 动态 | NEEDED |
|---|---|---|---|
| desktop.elf | 1,060,984 | 245,552 | libsky.so |
| doom.elf | 2,605,456 | 1,694,608 | libsky.so |
| hw2.elf | 884,120 | 42,736 | libsky.so |
| hw.elf | 817,256 | 41,856 | libsky.so |
| notepad.elf | 856,376 | 56,520 | libsky.so |

4 次连续启动（每次运行 55 秒）全部一致：

```
NEEDED=5  procs=3  [doom] init done=1
code=77=0  Segfault=0  KERNEL FAULT=0  OOM=0  ACK timeout=0  STALL=0
```

截图确认三个客户端窗口同时存活、文字正常渲染（DOOM 在实际游戏中）。

## 顺带修掉的一个真实死锁（就是用户报的 TLB 刷屏）

`SyncKillProcThreads()` 在**持有 `PROC_LIST_LOCK`** 的情况下遍历
`proc->threads` 环形链表：

```c
do {
    thread_t *next = t->next;
    ...
    t = next;
} while (t != start && proc->threads);     /* ← 链表一旦不成环, 死循环 */
```

链表一旦被破坏（某个 `next` 不指回 `start`），这个循环会**永远持锁**。
现场表现：另一个 CPU 在 `Schedule::PROC_KILL` 里等同一个锁 32 秒以上，
随后所有 TLB shootdown 因为目标核关中断等锁而全部 ACK 超时 ——
日志被 `[KERR] TLB shootdown: ACK timeout` 刷屏（实测一次坏运行里
311 条 ACK timeout + 16 条 STALL WATCHDOG）。

符号化证据（`addr2line` + `nm`）：

```
holder  : FFFFFFFF8004DADF -> SyncKillProcThreads  task.cpp:248
waiter  : FFFFFFFF8004E39E -> Schedule::PROC_KILL   task.cpp:585
lock    : FFFFFFFF801B9F44 -> PROC_LIST_LOCK   (nm: pid2proc_tree+8)
```

修法：给遍历加**有界保护**（4096 次上限），超限就放弃本轮回收并报一次错，
绝不在持锁状态下死等。修完 4/4 次启动 `ACK timeout=0`。



上面那条"ET_EXEC + 自定义脚本"的路最后被放弃了，原因很实在：
把 `.dynsym/.dynstr/.dynamic/.rela.*` 塞进自定义 `PHDRS` 脚本后，ld 会生成
**相互重叠**的 PT_LOAD（`.rodata` 同时出现在 RX 段和 R 段里），而内核的
`dl_vaddr_to_off()` 按"第一个包含该 VA 的 PT_LOAD"反查文件偏移，于是
`DT_STRTAB` 算到了错误段 → 读到垃圾 → `dyn_read_needed()` 找不到
`DT_NEEDED` → `elf_dyn_apply()` 提前返回 → PLT 全是野地址。这个组合还会
连带把静态构建的段表也搅乱，性价比太低。

改成更干净的方案：**由内核保证 0x400000 一定有一页**。

```c
/* exec.cpp, elf_load_impl 的两趟权限收紧之后 */
const uint64_t kProtoVa = 0x400000;
if (!VMM::GetPhysics(pagemap, kProtoVa)) {
    uint64_t pg = (uint64_t)PMM::Request();
    if (pg) {
        VMM::Map(pagemap, kProtoVa, pg, MM_READ | MM_WRITE | MM_USER);
        VMM::VMA::AddRegion(pagemap, kProtoVa, 1, MM_READ | MM_WRITE | MM_USER);
    }
}
```

因为 `sys_pmmapSHARE` 共享的是"客户端**已经存在**的那一页"，只要加载期
保证该 VA 有映射，客户端就可以和其它程序一样用 `-pie + libsky.so`，
完全不需要自定义链接脚本，也不需要 `.prepad`。

### 验证：hw2 变成第三个动态链接程序

`programs/helloworld2/Makefile` 改为 `include ../dynprog.mk` +
`all: $(DYN_ELF)` 之后：

```
hw2.elf  42,736 字节（静态版 884,120）  Type: DYN  NEEDED: libsky.so
启动日志: [dynlink] DT_NEEDED  3  条（desktop / hw / hw2）
         [dynlink] TLS layout  3
         PROC->ID 3   [doom] init done 1   compositor up 1
         KERNEL FAULT 0   Segfault 0   PMM OOM 0
```

至此三个程序是动态链接：`desktop.elf`、`hw.elf`、`hw2.elf`。
`notepad` 与 `PureDOOM` 仍是静态，转换方式完全相同（各加两行、去掉
`-T` 脚本依赖），留给下一轮。

## 已废弃：ET_EXEC + 自定义脚本（保留记录以免重犯）

要点：自定义 `PHDRS` 脚本里必须显式收 `.dynamic/.dynsym/.dynstr/.rela.*/.plt/.got`
并声明 `phdr_dynamic PT_DYNAMIC`，否则得到"有 PLT、有 JUMP_SLOT、没有 `.dynamic`"
的畸形 ELF。即使补齐，多个输出段仍会重叠，触发下面两个连锁问题：
1. 拷贝写只读页（已用"两趟权限收紧"修好，这个修复保留在 `elf_load_impl` 里）；
2. `dl_vaddr_to_off()` 反查歧义 → dynlink 静默跳过。


加载器修好后，换成动态 hw2 启动，`PROC->ID` 从 0 恢复到 **3**（说明加载成功），
但：

- **没有出现 hw2 的 `[dynlink]` 日志**（`DT_NEEDED` 只统计到 2 条：desktop 和 hw.elf）
- 随后内核栈溢出递归（`CR2=0xFFFF9000002FA000`，即内核栈顶之外），并伴随 6 次
  用户态 segfault

也就是说 `elf_dyn_apply()` 对 hw2 提前返回了（`dyn_read_needed` 没找到
`DT_NEEDED`），PLT 里的 5 个 `JUMP_SLOT` 从未被解析，客户端一跳就飞。

下一轮要从 `elf_dyn_apply` 的早退路径入手：给它加日志，确认是
`so_name` 为空（`dyn_read_needed` 失败）还是 `elf_dyn_prepare` 返回 0。
怀疑点：hw2 的 `DT_STRTAB`/`DT_NEEDED` 指向被脚本合并进大段的地址，
`dl_vaddr_to_off()` 用 PT_LOAD 反查时落在**重叠段**的边界上，取到错误偏移。

## 已验证的绿色基线（实验回退后）

```
dynlink DT_NEEDED 2   compositor up 1   [doom] init done 1
PROC->ID 3            Segfault 0        KERNEL FAULT 0
```

镜像已恢复为静态 `hw2`（884,680 字节，无 NEEDED）。
两趟权限收紧的改动**保留**在 `elf_load_impl` 里（对静态程序已验证无副作用）。
