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
| 无压力 | 91 ns | 96 ns | 131 ns | 11.7 ms（首轮：向量增长+首触缺页） |
| 高负载（10 万存活 256B 块） | 90 ns | — | 130 ns | 157 µs |
| **第二轮（热缓存，验 max 复现）** | 91 ns | — | 131 ns | **293 µs（复现）** |

**max 复现核查（已定位）**：第二轮慢路径计数 = **0 次**（128B slab 已热）仍出现
~160 个 >10µs 离群（max ~1.6ms）→ **根因 = 宿主线程抢占**（rdtsc 墙钟包含被
抢占时间，测试方法学限制），**非分配器缺陷**；分配器自身 P50/P99 不受影响。

### 1.4 尺寸分档吞吐（op = kmalloc+kfree 对；批量计时、预热 50k、sink 防消除、3 run）

| 类 | 16B | 32B | 64B | 128B | 256B | 512B | 1024B | krealloc 64→128 |
|---|---|---|---|---|---|---|---|---|
| M ops/s | 12.44 | 12.50 | 12.42 | 12.47 | 12.30 | 12.52 | 12.47 | 5.98 |

3-run 区间 11.45-12.52 M ops/s。**注**：早前报告的 8.8M ops/s 每迭代含一次
steady_clock 调用且 op 定义不同，已作废，以此表为准。
**"与同机 glibc 同数量级/不构成瓶颈"的结论已撤回**：glibc 对比从未实测，且吞吐
≠瓶颈结论（需内核侧 profile，见 `docs/stability-audit.md` §5）。

### 1.4b 多线程并发（slub-mt：4 宿主线程 = 4 虚拟 CPU）

| 阶段 | 结果 |
|---|---|
| A 无竞争（各 50 万 op 独立混沌） | 0 残留 0 失败 |
| B 跨线程 free（环形传递，各 20 万 op） | 0 残留 0 失败（真实跨 CPU free 路径 + 锁竞争） |
| C 同 cache 竞争（4 线程 hammer 128B，5s） | **10.70M ops/s**（单线程基线 12.4M，-14%） |
| TSAN | 功能 failures=0；**有已知理论竞态报告**（跨 CPU free 尾部/reclaimed slab 读窗口，未在混沌中造成功能性失败，见 audit §0''） |

### 1.5 碎片化

内部碎片率（类粒度开销）= 130.7%（主因 >1024B 大页粒度：1025B→4096B）；
**10 轮 × 5 万 op 混沌趋势：页数 5354 → 5354 完全持平，每轮 largest_free ≥ 4MB** ——
稳态无增长、无碎片化退化；SLUB 类内（≤1024）粒度损耗低。此为**分配粒度特征**
而非泄漏（页数守恒 + 大块可用为证）。

### 1.5b 红区负向（slub-poison 独立 `-DSLAB_DEBUG_POISON` 构建）

| 用例 | 结果 |
|---|---|
| oob+64 / +128 / +512 / +1024（写 p+usable 破坏尾部红区 → Free 检测 SIGABRT） | **4/4 ACTIVE** |
| oob-8（前越界） | **N/A** —— 当前布局仅尾部红区（`obj_size = usable + 8B 红区`），头部无红区，如实记录为已知限制 |

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
| 混沌 **2,000,000** op（500 键 Zipf α=1.1，70% 干净/30% 脏写） | **failures=0, tree_broken=0**（修复前 op 565 必现树损坏、op 935 必现越界） |
| 命中率（宽松限 1MB/2MB） | get 类操作 **≈87% 命中**（本地 29% + 跨核迁移 519k 次）；**限压 64KB 软限下 15.3%** —— 已查明为迁移被软限拒止(`smoothed_cache_bytes > soft_limit → continue`)的配置产物，非缺陷 |
| evictions=0 解释 | `file_cache_should_evict` 保护"频率≥均值"条目；Zipf/均匀负载下几乎全体受保护 → 淘汰判定不成立（机制已读码确认） |
| **混合负载整体吞吐 + 延迟（真实权重，3 秒批）** | **0.607M ops/s**；get 命中率 81.7%；get 延迟 P50=583ns P95=2.62µs P99=3.62µs max=2.67ms |
| **多线程并发（fc-mt：4 线程 × 4 实例 × 30 万 op，30% 脏写跨实例广播 + 全程巡检）** | **failures=0**（TSAN 已知理论竞态见 audit §0''） |
| 双重释放追踪 / destroy | 0 真双释 / 无崩溃 |
| **三个最小回归（tests/fc/regression.cpp）** | test_art_true_prefix_key / test_promote_inline_capacity_reuse / test_insert_fail_no_dangling **全部 PASS** |
| 分路径吞吐 | get-hit 0.31M / get-miss 7.37M / promote 0.16M / invalidate 11.96M / idle_handler 1.59M ops/s（op 定义见 tests/README） |

**结论降级**：上述为**单线程宿主**结论。"全量通过"限定为"本表所列套件通过"；
SMP 并发（多线程 per-CPU + 全局 LRU 交互）与 QEMU 真实负载待补（audit §2 F5）。

## 3. 调度器（账本模型仿真 —— 真实源码 EEVDF 骨架的忠实复刻）

**方法（诚实声明）**：`sched.cpp` 与 LAPIC/PIT/线程上下文深度耦合，无法直接宿主编译。
本仿真**逐行复刻 sched.cpp 的账本公式与常量**（每个常量/公式均注释对照源码行号：
`RIPRATE_*` sched.cpp:39-69、`get_dynamic_quantum` :205-225、RIP 采样/钳位 :410-467、
老化 :311-335、`calibrate_and_set_deadline` :588-605、`thread_rb_cmp`/`Pick` :574-582/
:881-919、唤醒抢占 :1220-1238、vruntime/avg 记账 :1010-1031），
运行队列为线性扫描替身（内核为 deadline 排序红黑树 + 子树增广，O(log n)；模型 O(n)，
选择结果等价）。另有 Linux **完整 EEVDF 参考**模型并排对照。

**定性（经逐行读码修正）**：真实源码的**选择逻辑已经是 EEVDF 骨架**，不是"CFS 最左
vruntime"——① 运行队列按 **deadline** 排序（`thread_rb_cmp` :574-582）；② **eligible
集** = vruntime ≤ avg（子树 min_vruntime 增广使"子树含 eligible"为 O(1) 判断，:897-919
下降搜索）；③ 选取 = **eligible 中 deadline 最小**（全树无 eligible 时最左兜底，:889-894）；
④ **唤醒抢占**（:1220-1230）：waker eligible 且 deadline 更小 → 打断，但当前**剩余
slice < ¼ base_quantum 时不打断**（protect_slice 变体，省上下文切换）；⑤ calibrate
的 **lag 钳制**（vruntime ∈ [avg−slice, avg+2·slice]，:591-602）；⑥ 运行量子**按权重
缩放**（`base_quantum×weight/1024`，:212-213）+ **per-thread `custom_quantum` 覆盖**
（sched_setattr-slice 等价接口已存在，:209-211）。

**与 Linux 完整 EEVDF 的差距（已拉齐，2026-09-27）**：此前两处实质差异已在内核关闭——
① **vlag 记账**：睡眠出队保存加权 lag `(avg−vruntime)·weight`，唤醒按
`vruntime = avg − vlag/weight` 放置再对称钳制（Linux `update_entity_lag`/`place_entity`
同构，见 `timer.cpp` Sleep、`calibrate_and_set_deadline`）；② **deadline 保护**：
vruntime 未越过旧 deadline 时不延长（Linux `update_deadline` 语义，slice 未耗尽免被
立即抢占）。注：本内核 slice 已按权重缩放（`get_dynamic_quantum`），一次满 slice 的
vruntime 消耗恒为 base_quantum，故 `deadline = vruntime + slice` 与 Linux 的
`ve + slice/w` 在各自单位下**同构**，此前"偏移未按权重缩放"的注释（:589-591）
已随实现一并更正。残留差异：lag 钳制阈值为常量 ±slice/±2·slice（Linux 用
latency 尺度阈值）；无 task_group 层级（不影响单队列结论）。
**与真实 sched.cpp 的偏差量级：未量化**（模型吞吐、唤醒延迟均为模型行为，不构成对
真实内核的性能结论）。

### 3.1 公平性（权重 1:1:2:4，20 万 tick）

| 线程 | 权重 | 实测份额 | 期望 | vruntime |
|---|---|---|---|---|
| 0 | 1 | 0.125 | 0.125 | 93,513,728 |
| 1 | 1 | 0.125 | 0.125 | 93,512,704 |
| 2 | 2 | 0.250 | 0.250 | 93,513,728 |
| 3 | 4 | 0.500 | 0.500 | 93,513,984 |

vruntime 极差 **0.0056%**（真实源码复刻，含 deadline 保护后收敛略松）/ **0.0000%**（完整 EEVDF 参考）。
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

### 3.4 睡眠唤醒（指数睡眠均值 20 tick，含真实唤醒抢占机制）

| 模式 | 唤醒→运行最大延迟 |
|---|---|
| 真实源码复刻（vlag 放置 + 唤醒抢占 + 剩余 slice ≥ ¼ 保护 + deadline 保护 + **eligible 预算截断**） | **75 tick**（改进前 144 tick） |
| Linux 完整 EEVDF 参考（eligible 含 +slice 余量） | 7 tick |

结论：改进前 144 tick 的根因 = RIP 拉长量子让线程"跑过头"（带负 lag 入睡，唤醒要等
avg 追平）；**eligible 预算截断**（线程最多跑到 vruntime 追平 avg，eligible 结束即
slice 结束）将其降为 75 tick（-48%）。剩余 75 tick 经模型实验确认是**结构性成本**
（deadline 平局按 id tiebreak + 预算截断后的轮转节奏）：lag 钳制 latency 缩放与
抢占抑制 ⅛ 两个候选调整均无收益，未搬入内核。

### 3.5 公平性（本轮改进后）

**eligible 预算截断**同时把公平性极差从 0.0056% 压到 **0.0000%**（所有线程在 avg
处精确停靠），并把 RIP 长量子造成的选取饥饿从 70 次降到 **5 次**。配套：RIP 上限
4× → 2.5×（减少"跑过头"）、唤醒 lag 钳制带放宽到 4×slice（Linux place_entity 的
latency 尺度）、vlag 除法四舍五入（短睡眠保留 lag 信用）。

### 3.6 压力

调度核心循环（16 线程）：≈3.6M tick/s（模型；含逐单位步唤醒检查与预算截断，
数字仅表示模型实现成本，非真实内核吞吐）。

## 4. 方法学边界（诚实）

- **SLUB/SLAB 与文件缓存**：测试的是**真实内核源码**（仅 shim 硬件依赖），但环境因素
  与内核不同：无中断抢占、单线程混沌（自旋锁退化为无竞争）、页面由 libc 提供。
  证明的是组件的**算法正确性、内存安全与单核开销**；**SMP 并发正确性未被本套件验证**。
- **调度器**：账本公式与常量逐行对照 `sched.cpp`（行号见 §3 引言）的**忠实复刻模型**
  （mode 0：真实源码的 EEVDF 骨架——deadline 树选择/唤醒抢占/slice 保护/lag 钳制/
  权重缩放量子；mode 1：Linux 完整 EEVDF 参考），运行队列为线性扫描替身（内核为
  deadline 红黑树 + 子树增广）——不是编译 `sched.cpp` 本身。
- **QEMU 真实内核（升级）**：`qemu-system-x86_64`（Windows 宿主，**TCG 无 KVM**），
  `-m 2G`，全参数（含 disk.img/网络/声卡）+ **`-debugcon` 捕获 E9 端口**（内核
  panic 消息走 E9 而非串口——此前的串口 grep 会漏报）：
  **核数扫描 smp 1/2/4 各 45 秒无异常；smp 4 长稳 30 分钟无异常**。
  **此轮 QEMU 升级实抓一次真回归**：tagged 空闲链改动致 `slub_stack_pop` #GP
  （addr2line 定位），已回退并复验全绿。这是**冒烟/长稳，不是 SMP 正确性证明**。
- 全部测试程序位于 `tests/`，一键复现：`cd tests && bash ci.sh`。
