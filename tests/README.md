# tests/ — 内核组件的宿主测试

把内核真实源码拉到 WSL/Linux 宿主上跑混沌、压力和回归。shim 只替换硬件相关依赖（清单见文末），
组件逻辑代码一行不动；测试专用钩子全部包在 `#ifdef __KERNEL_TEST_HOST__` 里，内核构建看不到。

## 怎么跑

```bash
cd tests && bash ci.sh          # Ubuntu 24.04, g++ ≥ 13
```

| 目标 | 测什么 | 性质 |
|---|---|---|
| `slub` | heap.cpp 真实源码：边界、krealloc、kcalloc 回绕、OOM 注入、混沌、延迟、分档吞吐 | 真实源码 |
| `slub-poison` | heap.cpp 用 `-DSLAB_DEBUG_POISON` 重编：红区越界负向用例 | 真实源码 |
| `fc` | fc.cpp + art.c 真实源码：200 万 op 混沌、分路径吞吐、命中率 | 真实源码 |
| `fc-reg` | 三个已修缺陷的最小确定性回归 | 真实源码 |
| `sched` | sched.cpp 账本公式逐行复刻模型，外加 Linux EEVDF 参考对照 | 公式复刻模型 |
| `memops` | ablib x86mem 四个 tier：小尺寸穷举、重叠、越界哨兵、跨 NT 阈值大块 | 真实源码 |
| `memops-bench` | 与 libc 及各 tier 对照的 GB/s 吞吐（见 `memops/README.md`） | 真实源码 |

`tests/stdcpp/` 不在这个体系里。它是 STL 的独立验收工程，入口是 `cd tests/stdcpp && make`，
不参与本目录的 `all`、`ci.sh` 和 GitHub Actions 门禁，原因见 `stdcpp/README.md`。

## 随机种子

全部写死，失败可以按同一串种子重放。

| 套件 | 种子 |
|---|---|
| slub | `0x9e3779b97f4a7c15` |
| fc | `0x2545f4914f6cdd1d` |
| sched | `0xdeadbeefcafebabe` |

## ops 怎么算（性能数字的解释口径）

- SLUB 分档吞吐：1 op = 一次 kmalloc+kfree；批量计时，预热 50k，2M ops 夹在两次 clock 之间；
  含一次 sink 累加防死码消除，不含 memset。
- SLUB 延迟：rdtsc 逐 op，TSC 频率用 steady_clock 标定，报 P50/P95/P99。
- fc 分路径：get-hit 1 op = get+put+std::string 键构造；get-miss = 4 个缓存全扫一次；
  promote = kcopy(kmalloc+memcpy)+promote(无广播)；invalidate = 跨 4 缓存广播；
  idle_handler = idle_handler+tick 各一次。
- fc 混沌：权重 40% promote（1..4096B，is_dirty=true 触发广播）/ 35% get+put（校验内容）/
  10% invalidate / 15% idle_handler+tick。
- sched：1 tick = 选线程、按量子执行、记 vruntime/avg 一次。

## 这些结论只能用在哪

- 单线程，shim 自旋锁无竞争，`this_cpu()` 恒为 id=0，不含 SMP 并发语义。
  SMP 由多线程套件 `slub-mt`/`fc-mt` 单独覆盖：每个宿主线程一个虚拟 CPU。
- 页面由宿主 libc（posix_memalign）给，`cli` 指令被测试宏换成空操作。
- 调度器测的是公式复刻模型（运行队列用 std::multimap 顶替），没有编译 sched.cpp。

## 性能数字的调试开销

- 本仓库所有性能数字都来自 `-O2 -g`，没开 ASAN/TSAN/canary/GDB。混沌期间的 canary 校验只影响
  正确性统计，不进计时区间。
- TSAN/ASAN 变体（`slub-mt-tsan`/`fc-mt-tsan`/`fc-asan`）只用来找竞态和越界，不产出性能数字。
  实测 TSAN 下 kmalloc 竞争吞吐掉到非 TSAN 的 1/20 左右（0.5M vs 10.7M ops/s）。

## shim 都换了什么

| 文件 | 作用 |
|---|---|
| `common/arch/x86_64/vmm/vmm.h` | 页池声明、故障注入、live_pages dump |
| `common/arch/x86_64/smp/smp.h` | cpu_t/cslab/this_cpu 的最小定义 |
| `common/klib/{types,str}.h`、`common/errno.h` | 与 libc 冲突处裁剪或转发 |
| `common/kernel_shim.{h,cpp}` | 自旋锁、内存函数、Panic、printf_、页池、ASAN 接口 |

内核侧只留三处钩子（`#ifdef __KERNEL_TEST_HOST__`）：`heap.cpp` 的 cli 关断与分配日志、
`fc.cpp` 的追踪器与插入失败注入、`art.c` 的 ARTLOG/verify/dump。宏未定义时这些代码不进内核构建。
