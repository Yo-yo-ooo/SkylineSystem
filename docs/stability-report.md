# 内核核心组件稳定性/性能测试报告

> 测试方法：把内核**真实源码**（`kernel/src/mem/heap.cpp` 等）搬到 WSL/Linux 宿主，
> 仅替换硬件相关依赖（页面分配、自旋锁、日志、CLI 指令）为等价 shim
> （见 `tests/common/`，清单见 `tests/README.md`），再进行**随机参数混沌测试**、
> **定时压力测试**与**最小确定性回归**。
> 所有数字由 `tests/` 下的程序在 Ubuntu-24.04 / g++14 / -O2 / **单线程**下产出，
> `cd tests && bash ci.sh` 一键复现；种子硬编码（slub `0x9e3779b97f4a7c15`、
> fc `0x2545f4914f6cdd1d`、sched `0xdeadbeefcafebabe`），失败可重放。
> **结论边界**：单线程、无中断抢占、libc 页面 —— 不含 SMP 并发语义（见 §4）。

## 复现方式

```bash
cd tests && bash ci.sh         # slub + slub-poison + fc + fc-reg + sched 全量
```

## 1. SLUB / SLAB 内核堆（真实源码：kernel/src/mem/heap.cpp）

### 1.1 混沌测试（随机参数模拟）

- 模式：200 万次随机操作/阶段 × 2 阶段；45% `kmalloc`（1..4096 B canary 填充）、
  35% 随机释放、10% `krealloc`（1..8192 B，校验旧内容保留）、10% canary 抽查。
- **结果（2026-09-27）**：

| 阶段 | 操作数 | canary 抽查 | 失败 | 存活页数 |
|---|---|---|---|---|
| phase 1 | 2,000,000 | 217,075 | **0** | 17 |
| phase 2 | 2,000,000 | 434,781 | **0** | 17 |

- **零失败、两阶段页数相等**（17 页 = 分配器稳态页池：SLUB 各尺寸类活动 slab +
  SLAB 页池保留，`dump_live_pages()` 可列出每个页的分配站点；两阶段差值=0 即无泄漏）。

### 1.2 边界/语义/负向用例（本轮新增）

| 用例 | 结果 |
|---|---|
| 25 种尺寸（0..65536B）×32：全非空、无重复地址、**全部 16 对齐** | PASS |
| krealloc 缩容/扩容/大幅缩容/同尺寸：内容保留；krealloc(ptr,0)=NULL；krealloc(NULL,n) | PASS |
| kcalloc 乘法回绕（1<<40 × 1<<12 等） | 全部拒绝（NULL） |
| OOM 故障注入（页分配失败）：kmalloc(8192)→NULL、krealloc OOM 保旧块、冷缓存 SLUB::Alloc→NULL、解除后恢复 | PASS |
| 双重释放 → SIGABRT（fork 子进程验证） | ACTIVE |
| **红区越界写**（`-DSLAB_DEBUG_POISON` 构建，SLAB::Alloc(512) 写 p+512 破坏 magic → Free 检测） | **ACTIVE** |

### 1.3 延迟分布（128B 类，rdtsc 逐 op，TSC 频率标定）

| 负载 | P50 | P95 | P99 | max |
|---|---|---|---|---|
| 无压力 | 86 ns | 90 ns | 120 ns | 375 µs（首触页缺页） |
| 高负载（10 万存活 256B 块） | 86 ns | — | 119 ns | 719 µs（偶发页分配） |

### 1.4 尺寸分档吞吐（op = kmalloc+kfree 对；批量计时、预热 50k、sink 防消除、3 run）

| 类 | 16B | 32B | 64B | 128B | 256B | 512B | 1024B | krealloc 64→128 |
|---|---|---|---|---|---|---|---|---|
| M ops/s | 12.44 | 12.50 | 12.42 | 12.47 | 12.30 | 12.52 | 12.47 | 5.98 |

3-run 区间 11.45-12.52 M ops/s。**注**：早前报告的 8.8M ops/s 每迭代含一次
steady_clock 调用且 op 定义不同，已作废，以此表为准。
**"与同机 glibc 同数量级/不构成瓶颈"的结论已撤回**：glibc 对比从未实测，且吞吐
≠瓶颈结论（需内核侧 profile，见 `docs/stability-audit.md` §5）。

### 1.5 碎片化（混沌后重填 5 万块 1..2048B）

内部碎片率（类粒度开销）= 130.7%（主要来自 >1024B 请求的大页粒度：1025B→4096B 页）；
SLUB 类内（≤1024）粒度损耗低。此为**分配粒度特征**而非泄漏（两阶段页数相等为证）。

### 1.6 结论（已降级）

> 在**单线程、无中断抢占、libc 页面 shim**环境下，SLUB/SLAB 在覆盖范围内的算法正确性、
> 内存安全（含负向检测）、边界语义与 OOM 行为表现良好；SMP 并发、真实中断抢占、
> 长期碎片化趋势尚未验证。**不表述为"SMP 安全"或无条件"稳定"。**

## 2. 文件缓存（fc.cpp + art.c）— 3 个内存安全缺陷已定位并修复，全量通过

**方法**：真实编译 `kernel/src/fs/fc.cpp` + `kernel/src/klib/algorithm/art.c` +
`heap.cpp`（仅换 shim 层），4 个 per-CPU 缓存实例、200 个逻辑文件键、
40 万次随机操作（promote/get/put/invalidate/idle_handler 加权），
带 canary 内容校验、逐操作 ART 键值一致性巡检、代际双重释放追踪、ASAN 毒化、
GDB 硬件内存监视点。

### 2.1 发现的缺陷（全部为"静态 review 看不见、只有混沌测试能逼出"）

| # | 位置 | 缺陷 | 后果 | 修复 |
|---|---|---|---|---|
| 1 | `art.c` 7 处 `key[depth]` | vendored libart 在递归→迭代重构时**丢失 0 终结符约定**；新键是既有叶子的真前缀时 `depth==key_len` 越界读，子索引取决于分配器残留字节 | 树结构损坏（遍历可见/搜索不可达）→ 缓存全 miss | 全部 `key[depth]` 读取加终结符守卫（搜索/插入分裂/递归插入/删除/迭代） |
| 2 | `fc.cpp` promote exist 分支 | 旧条目按**当初的 data_len** 分配内联容量；复用更大 data 时 `__memcpy(exist->inline_data, data, data_len)` 直接溢出 | 越界写砸进相邻条目 → 内存损坏（GDB 监视点抓现行，fc.cpp:1049） | 复用前校验 `SLUB::TryGetSize(exist)` 容量，不足则摘除旧条目改走新条目路径 |
| 3 | promote -4 路径 / put / pick_and_unlink | 插入后校验失败时 `kfree` **仍在 ART 中的条目**；pending 分支对已摘除条目二次摘链释放 | ART 悬垂值、双重释放写坏 LRU → **QEMU 实测 #GP 的根因** | 幂等 `art_delete` + 插入重试；`art_delete`==NULL 即跳过；`fc_lru_remove` 幂等化 |

### 2.2 修复后全量结果（2026-09-27, g++14, -O2）

| 项 | 结果 |
|---|---|
| 混沌 **2,000,000** op（500 键，软硬限 64KB/256KB） | **failures=0, tree_broken=0**（修复前 op 565 必现树损坏、op 935 必现越界） |
| 命中率统计 | hits=145,247 misses=1,140,875 migrations_in=25（限压下淘汰激进，evictions 计数器触达条件待查，见 audit） |
| 双重释放追踪 | 0 真双释 |
| destroy 清场 | 无崩溃 |
| **三个最小回归（tests/fc/regression.cpp）** | `test_art_true_prefix_key` / `test_promote_inline_capacity_reuse` / `test_insert_fail_no_dangling`（故障注入）**全部 PASS** |
| 分路径吞吐 | get-hit 0.31M / get-miss 7.37M / promote 0.16M / invalidate 11.96M / idle_handler 1.59M ops/s（op 定义见 tests/README） |
| promote 吞吐（3 run） | 171,873 / 167,772 / 162,795 ops/s |

**结论降级**：上述为**单线程宿主**结论。"全量通过"限定为"本表所列套件通过"；
SMP 并发（多线程 per-CPU + 全局 LRU 交互）与 QEMU 真实负载待补（audit §2 F5）。

## 3. 调度器（账本模型仿真 + Linux EEVDF 参考对照）

**方法（诚实声明）**：`sched.cpp` 与 LAPIC/PIT/线程上下文深度耦合，无法直接宿主编译。
本仿真**逐行复刻 sched.cpp 的账本公式与常量**（每个常量/公式均注释对照源码行号：
`RIPRATE_*` sched.cpp:39-69、`get_dynamic_quantum` :205-225、RIP 采样/钳位 :410-467、
老化 :311-335、`calibrate_and_set_deadline` :588-601、vruntime/avg 记账 :1010-1031），
运行队列用 `std::multimap` 等价替身（内核为内联红黑树）。**新增**：Linux EEVDF 参考
模型（加权 slice、VD=vruntime+slice、eligible 集按 min-VD 选择）并排对照。
验证的是**账本算法的稳定性、公平性与反馈行为**，不是编译 sched.cpp 本身。

### 3.1 公平性（权重 1:1:2:4，20 万 tick）

| 线程 | 权重 | 实测份额 | 期望 | vruntime |
|---|---|---|---|---|
| 0 | 1 | 0.125 | 0.125 | 93,513,728 |
| 1 | 1 | 0.125 | 0.125 | 93,512,704 |
| 2 | 2 | 0.250 | 0.250 | 93,513,728 |
| 3 | 4 | 0.500 | 0.500 | 93,513,984 |

vruntime 极差 **0.0014%**（3EVDF 复刻）/ **0.0000%**（EEVDF 参考）。
**降级表述**：这是"账本模型、单队列、无睡眠唤醒"下的份额一致性，不是对真实调度器的
"严格验证"。

### 3.2 RIP 速率反馈（4 忙等 + 1 交互，开/关对照）

| 模式 | 忙等平均量子 | 交互平均量子 |
|---|---|---|
| 反馈关 | 3.00 tick | 6.00 tick |
| 反馈开 | **1.06 tick**（压至 0.25x 下限附近） | **19.45 tick**（拉长至 ~4x） |

时间片塑形机制清晰可见。**注意**：RIP 只塑形时间片长度、不改变份额（与 scheduler.md
一致）；长量子使交互线程 vruntime 短期冲高、被选取频率下降——真实内核的校准钳制与
时钟推进会兜底，此张力已如实记录（模型行为，非真实内核结论）。

### 3.3 随机 spawn/exit 混沌

200,000 次随机 spawn（权重 1-16、三种行为类型）/exit/tick 混合：
**0 失败**，vruntime 有界不变量成立。

### 3.4 睡眠唤醒（新增，指数睡眠均值 20 tick）

| 模式 | 唤醒→运行最大延迟 |
|---|---|
| 3EVDF 复刻 | 104 tick（早期 avg<5M 阶段无放置补偿所致，模型假象） |
| EEVDF 参考（eligible 含 +slice 余量） | 0 tick |

结论：eligible 余量（+slice_i）对唤醒延迟影响显著；复刻模型缺少该余量是已知简化。

### 3.5 压力

调度核心循环（16 线程）：≈6.9M tick/s（模型；本轮含 EEVDF 分支，较上轮 9.8M 略降）。

## 4. 方法学边界（诚实）

- **SLUB/SLAB 与文件缓存**：测试的是**真实内核源码**（仅 shim 硬件依赖），但环境因素
  与内核不同：无中断抢占、单线程混沌（自旋锁退化为无竞争）、页面由 libc 提供。
  证明的是组件的**算法正确性、内存安全与单核开销**；**SMP 并发正确性未被本套件验证**。
- **调度器**：账本公式与常量逐行对照 `sched.cpp`（行号见 §3 引言）的**复刻模型** +
  EEVDF 参考模型，运行队列为 `std::multimap` 等价替身——不是编译 `sched.cpp` 本身。
- **QEMU 冒烟（真实内核）**：`qemu-system-x86_64`（Windows 宿主，**TCG 无 KVM**），
  `-m 2G -smp 4`，桌面空闲负载，最近一轮 **150 秒**无异常输出（早期 300 秒轮次早于
  ART/内联修复）。这是**冒烟**，不是 SMP 正确性证明；QEMU 长稳/故障注入见 audit 路线图。
- 全部测试程序位于 `tests/`，一键复现：`cd tests && bash ci.sh`。
