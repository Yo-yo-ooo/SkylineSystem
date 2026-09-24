# 3EVDF —— Rate-aware EEVDF 调度器

源码：`kernel/src/arch/x86_64/schedule/sched.cpp`（约 44 KB）、`task.cpp`、`timer.cpp`、`syscall/`。

## 1. 它在标准 EEVDF 上加了什么

经典 EEVDF（Earliest Eligible Virtual Deadline First）按**虚拟时间 vruntime** 和 lag 选下一个线程，能保证公平，但它分不清两件事：

- 一个线程在**真实推进**（渲染、解码、响应用户输入）；
- 一个线程只是在 `for(;;)` 里**空烧 CPU**，VRuntime 走得很快却对系统毫无贡献。

3EVDF（Rate-aware EEVDF / REEVDF）引入一个 **RIP-progress-rate** 信号：在内核里采样线程指令指针的推进速度，经 EWMA 得到快/慢倍率，**运行时回灌 EEVDF 的量子计算**。于是：

- 推进快、I/O 密集、交互型线程 → 给更长的量子、更容易被选中；
- 纯忙等自旋线程 → 被自动压短量子；
- **应用侧不需要写任何 `yield()`**。

实测：128 MB 内存下，一个进程起 4 个 `for(;;)` 线程，鼠标采样与屏幕合成依旧平滑；旧调度器同样负载必须显式 `sys_yield()` 才不卡。

## 2. 定点数与反馈通道

所有速率计算用 **Q10 定点**（`1.0x = 1<<10 = 1024`），避免内核浮点。

```c
#define RIPRATE_FRAC_BITS  10
#define RIPRATE_ONE        (1 << 10)   /* 1.0x */
#define RIPRATE_SHIFT      3           /* EWMA α = 1/8 */
#define RIPRATE_MAX_MULT   (4  << 10)  /* 4x 上限 */
#define RIPRATE_MIN_MULT   (1  << 8)   /* 0.25x 下限 */
#define RIPRATE_OUTLIER_MULT (16 << 10)/* 观测>16x 钳位而非丢弃 */
```

最终量子是**两条通道叠加**：

```text
eff = (weight_quantum × mult >> FRAC) + rip_quantum_adj
```

| 通道 | 作用 | 特征 |
|---|---|---|
| **乘法通道** `mult` | 稳态塑形：线程长期是快是慢 | EWMA 慢收敛，老化时向 1.0 收缩 |
| **修正通道** `adj` | 即时偏差的快速双向响应 | 有符号，范围 ±4 个 tick，可正可负 |

设计理由：乘法通道负责长期特征，修正通道负责瞬时纠偏；两个独立通道比单个倍率更不容易震荡。

### 采样与老化

- `rip_last_sample_ms` 超过 `RIPRATE_AGING_MS = 50ms` 没采样：`mult` 按 1/16 步长向 1.0 收缩，`adj` 向 0 收缩。注释里专门记录了一个 bug：旧公式 `(x+7)>>4` 在 `|adj|≤8` 时恒为 0，导致 adj 永不衰减，改成 `(x+15)>>4` 才保证至少走一步。
- 同一毫秒内两个 tick：窗口无效，只做老化不做速率采样。
- 离群观测（>16x）钳位到 16x 而不是直接丢弃——否则慢基线永远追不上突然变快的线程。
- 新线程 `mult==0` 一律按 1.0 处理，防止第一个量子被压成 1 个 tick。

## 3. 运行队列与 Pick

每个 CPU 一棵**红黑树**，键是 vruntime；每个节点额外缓存 `min_vruntime_subtree`（子树最小值）。

- `Pick()` 沿"子树最小值 ≤ avg_vruntime"的分支向下走，用 `min_vruntime_subtree` 做剪枝，命中 eligible 线程平均只走树高；
- 每个比较点都 `PREFETCH_R` 左子/右子节点，减少 cache miss；
- `InsertToQueue` / `RemoveFromQueue` 沿 parent 链向上更新 `min_vruntime_subtree`；
- 权重表 `sched_prio_to_weight[16]` 与 CBSD nice 权重同量级，并用 `static_assert` 锁死 16 项。

## 4. 动态基准量子

`dynamic_adjust_quantum()` 每 100ms 看一眼本 CPU：

- 空闲时间占比 > 50% → 拉长 `base_quantum`（摊薄定时器/切换开销）；
- 空闲 < 10% 且上下文切换 > 500 次 → **也拉长**（注释 v3 FIX：旧代码在这里缩短量子，方向反了——切换已经过多，缩短只会制造更多切换）；
- 否则向 5 收敛。

`base_quantum` 被钳在 [2, 15] tick。

## 5. SMP 负载均衡

双机制：

### Active push（`TryPush`）
- 本 CPU `has_surplus` 且线程数 ≥2 时触发；
- 遍历所有 CPU，按总权重找最轻的目标；**优先同 SMT/亲和掩码**，不同核作为 fallback；
- 双锁按 `cpu.id` 大小顺序获取，杜绝 ABBA 死锁；
- 只推 `rb_last` 起的尾部（权重最大/最老的）一批（`SCHED_STEAL_BATCH=8`）；
- **推完若目标正 hlt，立刻发 LAPIC IPI 唤醒**（v3 FIX：旧版要等一个 idle 量子的 tick 才捡起来）。

### Lazy steal（`StealThread`）
- 每 `SCHED_STEAL_THROTTLE=8` 次才真扫一次，避免空转；
- 两趟扫描：先只找同 SMT 掩码核，找不到再退化到任意核；
- `spin_trylock` 重试上限 100 次，失败就换下一个受害者（注释：旧写法 retries 到 101 才退出，正确性靠巧合）；
- 偷到后先标 `THREAD_TRANSFER` 释放受害者锁，再拿本 CPU 锁入队，缩小临界区。

### 公平性说明
- 队列里 vruntime 在入队时 `calibrate_and_set_deadline` 就被 clamp 到 `avg+2q` 以内，因此旧代码里"vr > avg+5M"的饥饿检查恒假，已作为死代码删除。

## 6. 抢占与上下文切换

`Schedule::Switch()` 是 SCHED_VEC 的 ISR，关键顺序：

1. 先停 LAPIC 定时器；
2. 拿当前 cpu；`this_cpu==nullptr` 时也要 EOI，否则 ISR 位悬挂、后续中断全堵；
3. AP 早期窗口（idle/current 还没建好）直接重 arm 定时器返回；
4. `preempt_count > 1` 时**不清** need_resched/yield 标志，只是重排下一个 oneshot——注释 v3 FIX：旧代码在函数顶部就消费标志，若后面走早退，抢占请求被无声吞掉；
5. 自愿 `yield()` 通过独立的 `yield_request_flags` 锁存，确保即便 runqueue 里只有一个竞争者也必须把 CPU 让出去。

## 7. 调参常量速查

| 常量 | 值 | 含义 |
|---|---|---|
| `SCHED_STEAL_BATCH` | 8 | 单次 push/steal 批量 |
| `SCHED_STEAL_THROTTLE` | 8 | steal 扫描节流 |
| `ZOMBIE_RECLAIM_THRESHOLD/BATCH` | 8 / 16 | 僵尸线程回收触发点与批量 |
| `RIPRATE_AGING_MS` | 50 | 速率老化窗口 |
| `RIPADJ_MIN/MAX` | -4 / +4 | 直接修正项范围 |
| `base_quantum` 初始 | 5 tick | 约 5 ms |
