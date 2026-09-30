# tests/ — 内核组件宿主测试

把内核真实源码搬到 WSL/Linux 宿主做混沌 + 压力 + 回归测试。
shim 只替换硬件相关依赖（见下），组件逻辑代码一行不改
（测试专用钩子全部 `#ifdef __KERNEL_TEST_HOST__`，内核构建零影响）。

## 一键复现

```bash
cd tests && bash ci.sh          # Ubuntu 24.04, g++ ≥ 13
```

目标：

| 目标 | 内容 | 性质 |
|---|---|---|
| `slub` | heap.cpp 真实源码：边界/krealloc/kcalloc回绕/OOM注入/混沌/延迟/分档吞吐 | ✅ 真实源码 |
| `slub-poison` | heap.cpp 以 `-DSLAB_DEBUG_POISON` 重编：红区越界负向用例 | ✅ 真实源码 |
| `fc` | fc.cpp+art.c 真实源码：200 万 op 混沌 + 分路径吞吐 + 命中率 | ✅ 真实源码 |
| `fc-reg` | 三个已修缺陷的最小确定性回归 | ✅ 真实源码 |
| `sched` | sched.cpp 账本公式逐行复刻模型 + Linux EEVDF 参考对照 | ⚠️ 公式复刻模型 |

## 随机种子（全部硬编码，失败可重放）

| 套件 | 种子 |
|---|---|
| slub | `0x9e3779b97f4a7c15` |
| fc | `0x2545f4914f6cdd1d` |
| sched | `0xdeadbeefcafebabe` |

## ops 定义（性能数字的解释口径）

- **SLUB 分档吞吐**：1 op = kmalloc+kfree 对；批量计时（预热 50k，2M ops 夹两次
  clock）；含一次 sink 累加防死码消除；不含 memset。
- **SLUB 延迟**：rdtsc 逐 op，TSC 频率用 steady_clock 标定；P50/P95/P99。
- **fc 分路径**：get-hit 1 op = get+put+std::string 键构造；get-miss = 4 缓存全扫
  一次；promote = kcopy(kmalloc+memcpy)+promote(无广播)；invalidate = 跨 4 缓存广播；
  idle_handler = idle_handler+tick 各一次。
- **fc 混沌**：op 权重 = 40% promote(1..4096B, is_dirty=true 触发广播) /
  35% get+put(内容校验) / 10% invalidate / 15% idle_handler+tick。
- **sched**：1 tick = 一次"选线程+按量子执行+vruntime/avg 记账"。

## 环境与边界（结论必须限定在此范围内）

- 单线程；shim 自旋锁无竞争；`this_cpu()` 恒为 id=0 —— **不含 SMP 并发语义**。
- 页面由宿主 libc (posix_memalign) 提供；`cli` 指令被测试宏替换为空操作。
- 调度器是**公式复刻模型**（运行队列 std::multimap 替身），不是编译 sched.cpp。

## shim 改动清单（不改内核逻辑的文件）

| 文件 | 作用 |
|---|---|
| `common/arch/x86_64/vmm/vmm.h` | 页池声明 + 故障注入 + live_pages dump |
| `common/arch/x86_64/smp/smp.h` | cpu_t/cslab/this_cpu 最小定义 |
| `common/klib/{types,str}.h`、`common/errno.h` | 与 libc 冲突裁剪/转发 |
| `common/kernel_shim.{h,cpp}` | 自旋锁/内存函数/Panic/printf_/页池/ASAN 接口 |

内核侧仅三处测试钩子（`#ifdef __KERNEL_TEST_HOST__`）：
`heap.cpp` 的 cli 关断 + 分配日志；`fc.cpp` 的追踪器 + 插入失败注入；`art.c` 的
ARTLOG/verify/dump。**内核构建不含这些代码**（宏未定义）。
