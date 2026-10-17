# PMM OOM 与 TLB shootdown 超时 —— 同一个根因

## 症状

桌面起来之后，约 20~40 秒内核心日志被两类消息刷屏并最终 OOM：

```
[KERR] TLB shootdown: ACK timeout (1 left), unacked mask 0x1, degrading to async
[KERR] PMM: out of contiguous physical memory (1 pages)
[KERR] PMM: OOM in Alloc
```

## 定位过程（三次采样，逐步收紧）

### 1. 先确认"是不是真耗尽"

在 OOM 处打印 `pc`（请求页数）、`mapped`（已映射页数）、`free`（剩余空闲页）：

```
PMM: OOM in Alloc (pc=4 mapped=0 free=0)
```

`free=0` ⇒ 真的耗尽了，不是记账错误；`pc=4` ⇒ 只求 16KB，不是"某个超大请求"。

### 2. 再确认"是不是大块吃掉的"

给 `VMM::Alloc` 加 `pc >= 256` 的采样，整个运行期间大块分配**只有 5 次**：

```
3 × 1025 页  ← sys_mmap_（约 4 MB）
1 × 4097 页  ← sys_mmap_（约 16 MB）
1 ×  637 页  ← SLAB::Alloc
```

合计约 30 MB。⇒ 2 GB 是被**成千上万次小分配**吃掉的，不是某一个大请求。

### 3. 最后按调用者 / 进程采样

`PMM::Request` 每 20000 次成功分配采样一次直接调用者：
**只有一个调用者** `VMM::Alloc`（23 次采样 ≈ 46 万页 ≈ 1.8 GB）。

再在 `sys_mmap_` 里按 pid 采样：

```
[mmapsample] #2000  pid=7 mode=2 pc=64      ← 每次 64 页 = 256KB
[mmapsample] #10000 pid=7 mode=2 pc=4
[munmapsample] #2000 pid=7 len=4            ← 只还 4
```

`pid=7` 是桌面。它 60 秒内 mmap **12000+ 次**。

## 根因：`LessCore` 把**页数**当成**字节数**传给 `sys_munmap`

`lib/stdc/allocator/allocator.c` 里这一层全程用**页数**：

```c
void* MoreCore(uint64_t PageCount) {
    uint64_t p = sys_mmap(0, PageCount, 2, 0, 0);   /* mode=2: 参数就是页数 */
}
void LessCore(void* x, uint64_t y) {
    sys_munmap((uint64_t)x, y);                     /* ← y 是页数，却被当字节 */
}
```

而 `sys_munmap(addr, length)` 的第二个参数是**字节长度**，内部算 `pages = length / PAGE_SIZE`。
于是 `LessCore(ptr, 4)` → `pages = 4/4096 = 0` → `if (pages == 0) return 0;`，
**一个字节都没归还**。

净效果：每轮 mmap 64 页、munmap 0 页，2 GB 在半分钟内被抽干。

## 修复 1：单位换算

```c
void LessCore(void* x, uint64_t y) {
    sys_munmap((uint64_t)x, y * PAGE_SIZE);   /* 页数 -> 字节 */
}
```

验证：`[munmapsample] len=16384`（4 页 × 4096），`OOM: 0`、`out of contiguous: 0`。

## 修复 2：TLB ACK 计数改成饱和递减

OOM 修掉后仍有少量 `ACK timeout`。看 `WaitAcks()`：超时 100ms 后会把
`shootdown_acks` **清 0** 并降级为 async；但那次请求的 IPI 仍可能在目标核排队，
稍后照样回 ACK。而 `DoAck()` 原来是无条件 `__atomic_sub_fetch(...)`：

- 计数被减成 **-1**；
- 下一次 shootdown 先 `add 1` → 又变回 **0** → `WaitAcks()` 看到 0 **立刻返回，
  一页都没刷** ⇒ 陈旧 TLB / 随机缺页；
- 若先减到负很多，则之后每次等待都白等 100ms。

改成**只在 `> 0` 时递减**（CAS 循环），让"迟到 ACK"变成无害空操作。

## 验证（同一镜像、同一 QEMU 参数，运行 60 秒）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `PMM: OOM in Alloc` | 3471 ~ 6352 | **0** |
| `out of contiguous physical memory` | 19988 | **0** |
| `TLB shootdown: ACK timeout` | 0~若干 | **0** |
| `Segfault` | 0~3 | **0** |
| `STALL WATCHDOG` | 有 | **0** |
| `[doom] init done` | 0~1 | **1** |
| `compositor up` | 1 | **1** |

## 仍未解决

- **桌面字体不显示**：`res/font/SourceHanSerifTC_Medium.ttf` 不存在（`*.ttf` 被
  gitignore），沙箱代理会截断下载（`curl: (18)`）。构建已改为不因此失败。
- **全部程序动态链接**：`desktop.elf` 与 `hw.elf` 已是动态版；
  `hw2/notepad` 的自定义链接脚本写死 `. = 0x400000`，需先去掉才能转 `-pie`；
  `doom.elf` 是第三方移植，仍为静态。

## 遗留的临时诊断（问题定案后应清理）

- `kernel/src/mem/pmm.cpp`：`PMM: reserved N bootloader pages`、`[pmmsample]`
- `kernel/src/arch/x86_64/vmm/vmm.cpp`：`[vmmdiag] big alloc`、OOM 行里的 `free=/pc=`
- `kernel/src/arch/x86_64/schedule/syscall/memory.cpp`：`[mmapsample]`、`[munmapsample]`
- `kernel/src/arch/x86_64/schedule/sched.cpp`：`[pmmdiag]`（OOM 场景下 idle 拿不到
  CPU，实测一条都没打出来，留着意义不大）
