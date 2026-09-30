# 稳定性报告自审：证据缺口、补测方案、结论降级与路线图

> 本文件对 `docs/stability-report.md` 逐条审计。原则：
> ① 严格区分「真实内核源码编译」/「公式复刻模型」/「纯仿真」；
> ② 单线程 shim 结论不得写成"组件稳定"或"SMP 安全"；
> ③ 无上下文的性能数字一律视为无效。

## 0'. 补测进度（已落地项，2026-09-27）

| 审计项 | 状态 | 证据 |
|---|---|---|
| S1-S5 边界/krealloc/kcalloc 回绕/OOM 注入/对齐 | ✅ | `tests/slub/main.cpp` 全部 PASS（报告 §1.2） |
| S2 延迟分布 P50/P95/P99 | ✅ | 128B 类 200 万 op，P50=86ns/P99=120ns（报告 §1.3） |
| S8 分档吞吐 + 批量计时 + sink | ✅ | 16..1024B 七档 + krealloc（报告 §1.4） |
| S7 OOM 路径 | ✅ | `vmm_set_fail_all` 注入：大块 NULL、krealloc 保旧块、冷缓存 NULL、恢复（§1.2） |
| 红区越界负向用例 | ✅ | `slub-poison` 独立构建（`-DSLAB_DEBUG_POISON`），Free 检测 ACTIVE |
| S9 live_pages 机制 | ✅ | 17 页 = SLUB 类活动 slab + SLAB 页池；`dump_live_pages()` 输出分配站点 |
| F4 三个最小回归 | ✅ | `tests/fc/regression.cpp`：test_art_true_prefix_key / test_promote_inline_capacity_reuse / test_insert_fail_no_dangling（故障注入）全 PASS |
| F1 分路径吞吐 | ✅ | get-hit/get-miss/promote/invalidate/idle_handler（报告 §2） |
| F2 命中率统计 | ✅ | hits/misses/migrations 输出（报告 §2） |
| 长混沌 | ✅ | 200 万 op、500 键、软硬限 64KB/256KB（报告 §2） |
| 调度器 EEVDF 参考对照 | ✅ | 加权 slice+VD 模型，公平性/唤醒延迟并排（报告 §3.1/§3.4） |
| M3 种子打印 | ✅ | 三套件启动即打印种子 |
| M4 CI | ✅ | `tests/ci.sh` 全量（slub/slub-poison/fc/fc-reg/sched） |
| 五条过强表述 | ✅ | 报告已全部降级/更正（见 §5 处理列） |
| **未完成** | ⏳ | S6/F5 多线程并发、QEMU sched trace、修复前后性能对比、金样 diff、evictions 计数器触达条件核查 |

---

## 0. 现有测试的诚实基线（审计出发点）

| 事实 | 值 |
|---|---|
| 测试宿主机 | WSL2 (Ubuntu-24.04)，g++ 14，-O2；**CPU 型号/核数/绑定/调速器未记录** |
| 线程模型 | 全部**单线程**；shim 自旋锁退化为无竞争 |
| 随机种子 | 硬编码：slub `0x9E3779B97F4A7C15ULL`、fc `0x2545F4914F6CDD1DULL`、sched `0xDEADBEEFCAFEBABEULL`（确定性 ✓，但未写入报告） |
| ops 定义 | SLUB 压力 1 op = kmalloc+单字节 memset+kfree（**每迭代含一次 steady_clock 计时调用**）；命名缓存 1 op = alloc+free；fc 1 op = kcopy(kmalloc+memcpy)+promote(is_dirty=true,含广播)；sched 1 tick = 选线程+按量子执行+记账 |
| QEMU 验证 | Windows qemu-system-x86_64，`-machine q35 -cpu max`，**TCG（无 KVM）**，2G/4 核，桌面空闲负载；最新一轮为 **150 秒**冒烟（早期 300 秒轮次在 ART/内联修复**之前**） |
| 性能数字来源 | 3 run 仅 SLUB 有；fc/sched 无方差/无 P50/P99/无分档/无预热说明 |

---

## 1. SLUB/SLAB 缺口与补测方案

### 1.1 缺失证据清单

| # | 缺口 | 现状 |
|---|---|---|
| S1 | 尺寸分档吞吐/延迟 | 只有 1..512B 混合一个数字，无 per-class 数据 |
| S2 | 延迟分布 | 只有吞吐，无 P50/P95/P99 |
| S3 | krealloc 专项 | 混沌里 10% 权重，无 shrink-in-place / 跨类增长 / realloc(0) / realloc 失败 的断言 |
| S4 | 边界用例 | kmalloc(0)/kmalloc(1)/1024/1025/4096/大页边界、kmalloc_aligned、kcalloc 溢出(numitems*size 回绕) 未测 |
| S5 | 碎片化指标 | 只有 live_pages；无内部碎片率(Σ(类大小-请求)/Σ请求)、页利用率、峰值 RSS |
| S6 | 多线程 per-CPU | 完全缺失（cslab magazine 与 SLUB cpu_active 只在 cpu id=0 上跑过） |
| S7 | OOM/故障注入 | VMM::Alloc 返回 NULL 的路径从未被触发（fc 的 eviction fallback 形同虚设） |
| S8 | 防优化 | 计时循环每迭代调用 clock，度量含测量开销；无 sink 校验和防死码消除；无 -O0/-O2/LTO 对照 |
| S9 | 存活页数=9 的机制解释 | 报告只写"稳态页池"，未验证 9 页各自是什么（假设：SLAB 7 个尺寸类各 1 页 + 元数据 + SLUB 命名缓存描述符，**未经证实**） |
| S10 | CI / 金样 | 无 CI 配置、无期望输出快照、无回归门禁 |

### 1.2 补测方案（可复制到 tests/slub/）

| 用例名 | 目的 | 设计/变量 | 指标 | 预期 | 防自欺 |
|---|---|---|---|---|---|
| `test_boundary_sizes` | 边界不崩不重叠 | size ∈ {0,1,15,16,17,31,32,63,64,127,128,1023,1024,1025,2048,4095,4096,4097,65536}，每个 size 分配 100 次，全量唯一性检查 | dup=0, 返回非空 | 全部通过 | 固定种子；校验和写回+sink 打印 |
| `test_krealloc_shrink_grow` | realloc 语义 | 128→64(原地)、64→4096(跨类)、4096→8、ptr+0(释放返回 NULL)、NULL+size(等价 kmalloc)；每步断言内容保留 | 内容 min 区一致性 | 全部一致 | canary 模式 + 唯一性表 |
| `test_kcalloc_overflow` | 乘法回绕 | kcalloc(1<<40, 1<<12) 等，断言返回 NULL 或安全失败不写坏 | 不崩、不分配 | 拒绝 | 负向用例 |
| `test_oom_path` | OOM 行为 | shim 加 `g_fail_next_alloc` 开关：第 N 次 VMM::Alloc 返回 NULL；测 kmalloc 大块/SLUB 慢路径/回退 | 不崩、错误码正确、后续可恢复 | 拒绝并恢复 | 故障注入开关单测 |
| `fragmentation_steady_state` | 碎片化 | 固定种子混沌 2 阶段后：Σ(class_size-req)/Σreq、页利用率(每页 inuse/objects)、峰值/稳态 live_pages | 稳态不增长、内部碎片率报告 | 有界且打印 | 两阶段差值判泄漏 |
| `test_latency_distribution` | 延迟分布 | 单类 128B，5M 次 alloc/free，记录每次 ns，输出 P50/P95/P99/max | 分布表 | 记录即产出 | 用 rdtsc 或 clock_gettime(CLOCK_MONOTONIC_RAW)；预热 10k |
| `test_multithread_percpu` | per-CPU 路径并发 | **shim 升级**：`this_cpu()` 返回线程局部 cpu_t（id=0..N-1），4 线程各绑一个虚拟 CPU 打混沌 + 跨线程 free 对方指针 | 0 重复、0 泄漏 | 通过 | 线程局部 id + 固定种子；ASAN 变体 |
| `test_size_class_throughput` | 分档吞吐 | 每类(16..1024) 单独 2s×3 run 吞吐 + 延迟 | 分档表 | 记录 | 批量计时（1M ops 间一次 clock） |
| `ops_def_and_measurement` | 度量规范 | 计时外移：先跑 10k 预热，再 N ops 两次 clock；op 定义写入报告 | 纯 op/s | 可复现 | 校验和防消除 |

**报告改动**：
- 吞吐数字必须标注：op 定义（含 memset、clock 调用与否）、预热、运行时长、方差、绑核与否、WSL2 虚拟化噪声。
- "与同机 glibc 同数量级，说明不构成瓶颈" → **删除或降级**：glibc 对比**从未实测**；吞吐≠瓶颈结论。若保留，需实测 glibc malloc/free 同负载同进程测三个数字，且瓶颈结论需内核侧 profile（syscall/IO 占比）支撑。
- "该组件可判定为稳定" → **降级**："在单线程、无故障注入、宿主 shim 环境下，S1-S10 覆盖范围内的算法正确性与内存安全表现良好；SMP 安全、OOM 容忍、碎片化特征尚未验证。"
- live_pages=9：补 `dump_live_pages_sites`（shim 里给每次 VMM::Alloc 记录调用点），解释 9 页构成后再写结论。

---

## 2. 文件缓存缺口与补测方案

### 2.1 缺失证据清单

| # | 缺口 | 现状 |
|---|---|---|
| F1 | 分路径吞吐 | 只有 promote；get(命中)/get(未命中+迁移)/put/invalidate/idle_handler 无单独数字 |
| F2 | 混合负载与命中率 | 无 hit/miss 统计输出、无读写比扫描 |
| F3 | 键长/ART 深度 | 键只有 "file0".."file199"（5-7B）；无 64/256/1024B 键、无二进制键、无对抗前缀族扫描 |
| F4 | 三个已修缺陷的**最小确定性回归用例** | 目前只嵌在大混沌里，无独立命名用例 |
| F5 | SMP 并发 | 单线程；`g_fc_cpus` 多缓存实例之间的广播/迁移并发语义未测（多线程跑同一 cache 实例才算并发） |
| F6 | 修复前后性能对比 | 修复 2（每次复用 TryGetSize）与修复 1（每次 key[depth] 分支）的热路径开销未量化 |
| F7 | flush/写回路径内容正确性 | wb_cb 恒返回 0，从不真正校验写回数据；is_dirty 条目 flush 后 is_dirty 清零、CRC 更新未端到端验证 |
| F8 | 失败注入 | fc_kmalloc_with_fallback 的驱逐回退从未触发（kmalloc 永远成功） |
| F9 | pinned 期间的广播 | pin>0 条目被广播置 INVALID 的路径无定向测试 |

### 2.2 三个已修缺陷的最小回归用例（P0，断言可直接抄）

| 用例名 | 缺陷 | 最小复现 | 断言 |
|---|---|---|---|
| `test_art_true_prefix_key` | ART 真前缀键越界读（修复 1） | art_mini：key 缓冲**尾部填垃圾字节**（`malloc(8); memcpy(buf,"file10",6); buf[6]='X'`）再插入既有 "file105"；反向 "file10" 先插后插 "file105" | insert 后 `art_search("file10")` 命中、`art_search("file105")` 命中、`art_verify` 无违规；**修复前此用例在 buf[6]=非 0 时必失败** |
| `test_promote_inline_capacity_reuse` | promote 内联容量复用溢出（修复 2） | 对同一 key 先 promote(data_len=8) 再 promote(data_len=64)；相邻分配一个哨兵条目，校验其 canary | 哨兵 canary 完好；第二次 promote 返回 0；get 返回 64B 内容一致 |
| `test_insert_fail_no_dangling` | 插入失败后悬垂引用/双重释放（修复 3） | 构造 art_insert 后 search 失配（用 `test_art_true_prefix_key` 的旧触发或注入）：promote 返回 -4 后，`art_iter` 全树无该 key 残留；再次 promote 同 key 成功 | -4 后树无残留值；无 FCDBG 双释/悬垂；重复 100 次无泄漏 |

### 2.3 其余补测

| 用例名 | 目的 | 设计 | 指标 | 防自欺 |
|---|---|---|---|---|
| `test_fc_path_throughput` | 分路径吞吐 | 预填 200 键 → 2s×3 分别测 get-hit/get-miss(migrate)/put/invalidate/promote/idle_handler | 每路径 ops/s+方差 | 批量计时、预热、固定种子 |
| `test_fc_hit_rate` | 命中率与混合负载 | 读写比 ∈ {9:1, 1:1, 1:9}，key 偏斜（Zipf），缓存 4 实例 | hit/miss/迁移/驱逐计数 | 与 shadow 模型期望对照 |
| `test_key_length_sweep` | ART 深度 | 键长 {5,16,64,256,1024}，前缀族对抗（file1/file10/file105/同长随机），10 万 op | 0 树损坏、搜索命中正确 | art_verify 每 op |
| `test_fc_concurrent` | SMP 并发语义 | 4 线程共享 1 个 cache 实例（真并发，shim 自旋锁真实生效）+ 4 线程各 1 实例互广播 | 0 崩溃 0 泄漏 | ASAN 变体 + 唯一性 |
| `test_flush_writeback_content` | 写回正确性 | wb_cb 捕获数据做 CRC 比对；promote(is_dirty) → idle → 断言 wb_cb 收到内容一致、is_dirty 清零 | 内容一致 | 回调侧校验和 |
| `test_fc_oom_fallback` | 驱逐回退 | kmalloc 失败注入 → 断言 fc_kmalloc_with_fallback 触发驱逐且返回可用块 | 不崩、可恢复 | 故障注入开关 |
| `test_broadcast_pinned` | pinned 广播 | get(持 pin) → 他核 invalidate → 断言状态 INVALID、put 后无悬垂 | 无 FCDBG 告警 | 负向断言 |

**报告改动**：
- "全量通过" → 改为"本报告所列 400,000 op 混沌、destroy、三 run promote 吞吐通过"（列明范围）。
- promote 吞吐补方差、op 定义（含广播与 kcopy）、预热。
- "修复后 4 核 300 秒无 #GP" → **降级并更正**："QEMU(TCG) 4 核桌面空闲 150 秒冒烟无异常（早期 300 秒轮次早于 ART/内联修复）；不是 SMP 正确性证明，需 F5 并发测试 + 故障注入补强。"

---

## 3. 调度器缺口与补测方案

### 3.1 与 Linux EEVDF 的机制对照（账本模型缺失项）

| 机制 | Linux EEVDF（6.6+） | 本模型/sched.cpp 现状 | 影响 |
|---|---|---|---|
| 虚拟截止时间 VD | `deadline = avg_vruntime + slice/weight`，**eligible 集合按 VD 排序**选择 | 模型：deadline = vruntime + base_quantum（未加权）；选择只按 vruntime≤avg 取最小 | 无法评估"最紧急"语义；公平性结论只覆盖 vruntime 序 |
| slice 参数 | `sysctl_sched_base_slice`（默认 300µs），slice 按 weight 缩放、受 `min_granularity` 钳制 | 模型 base_q=5 固定；sched.cpp 有 `sched_prio_to_weight[16]` 但量子未按权重缩放（文档自认简化） | 权重只影响 vruntime 记账，不影响时间片 |
| protect_slice | 突发交互线程受保护免被立即抢占 | **完全缺失** | 交互尾延迟评估无效 |
| wakeup preemption | 唤醒抢占检查（wakeup_gran） | 无 sleep/wake 模型 | 场景 2 无意义（线程永不睡眠） |
| lag 记账 | entity lag 累积、placement 补偿 | 模型只有 vruntime_rem 余数 | 长量子下份额漂移（模型已观察到的张力） |
| 多队列均衡 | runqueue 间 push/steal | 模型单队列 | SMP 公平性完全未覆盖 |
| 组调度/CFS 层级 | task_group 层次 | 无 | 不影响本结论，需注明 |

**"公平性严格验证" → 降级**："账本模型（单队列、无睡眠唤醒、固定权重、vruntime 序选择）下，20 万 tick 份额与权重一致、vruntime 极差 0.0014%；不含 EEVDF 的 VD 排序/protect_slice/多队列语义。"

### 3.2 补强方案

| 方案 | 性质 | 内容 | 产出 |
|---|---|---|---|
| `sched_ref_eevdf` | 公式复刻 | 按 Linux EEVDF 方程实现参考模型（VD=avg+slice/w、eligible 集、protect_slice、lag），与 3EVDF 复刻**并排同负载**对比 | 公平性/尾延迟双模型对照表；量化"未加权 deadline + 无 protect_slice"的代价 |
| `sched_wake_sleep_model` | 公式复刻 | 加入随机 sleep/wake（指数分布），测 wake→run 延迟 P99，RIP 开/关 | 交互延迟的真实度量（替代现行"最大选取间隔"） |
| QEMU 内核算账 trace（**真实代码**） | 真源码+内核打印 | 扩展 `kernel/src/arch/x86_64/schedule/syscall/sched_bench.cpp`：每 N tick 向串口输出各线程 vruntime/deadline/quantum/avg_vruntime，QEMU `-serial file:` 采集后离线分析 | SMP 下真实账本的收敛/漂移曲线；比模型强一档的证据 |

---

## 4. 方法学与可复现性缺口

| # | 缺口 | 补测/补齐 |
|---|---|---|
| M1 | 环境上下文 | 报告补：`uname -a`、CPU 型号、核数、是否 taskset、WSL2 版本、QEMU 版本与加速器（TCG/KVM） |
| M2 | 防优化 | 所有压力循环：预热 + 批量计时 + 校验和 sink 打印；`-O0/-O2` 对照一次 |
| M3 | 种子与金样 | 报告列出三个种子；每个测试生成 expected-output 快照文件，CI diff 判回归 |
| M4 | CI | `tests/ci.sh`：`make slub fc sched && ./bin/*_test` 全部 exit 0 才算过；附 GitHub Actions linux runner yml |
| M5 | 负载声明 | 每个数字标注：op 定义、预热、时长、run 数、方差、是否含时钟调用 |
| M6 | 负向测试清单 | S7/F8/test_oom_path/test_fc_oom_fallback/test_insert_fail_no_dangling 列入 CI |

---

## 5. 五条表述逐条判定

| 表述 | 判定 | 处理 |
|---|---|---|
| "该组件可判定为稳定" | **过强** | 降级为"单线程 shim、无故障注入环境下，覆盖范围内算法正确性表现良好；SMP/OOM/碎片化未验证" |
| "全量通过" | **模糊** | 列明测试范围后再用 |
| "与同机 glibc malloc/free 同数量级，说明不构成系统瓶颈" | **无据** | glibc 未实测；瓶颈结论需内核 profile；删除或实测后改写 |
| "公平性严格验证" | **过强** | 限定为"账本模型、单队列、无睡眠唤醒"的份额一致性 |
| "修复后 4 核 300 秒无 #GP" | **不准确** | 更正为 150 秒 TCG 冒烟（300 秒轮次在 ART/内联修复前）；定位为冒烟非证明 |

---

## 6. 真实性矩阵模板

| 模块 | 来源文件 | 真实源码编译 | shim/复刻/仿真 | 可验证边界 | 下一步验证方式 |
|---|---|---|---|---|---|
| SLUB/SLAB | `kernel/src/mem/heap.cpp` | ✅ 真实源码（`-D__KERNEL_TEST_HOST__` 仅关 cli、加 ASAN 钩子） | shim：VMM 页池、this_cpu、自旋锁（无竞争） | 算法正确性、单线程内存安全 | 多线程 per-CPU shim、OOM 注入、QEMU 长稳 |
| 文件缓存 | `kernel/src/fs/fc.cpp` + `art.c` + `heap.cpp` | ✅ 真实源码 | shim 同上；`g_fc_cpus` 多实例单线程 | 单核多缓存实例语义 | 真并发多线程、QEMU 磁盘负载 |
| ART | `art.c` | ✅ 真实源码 | 无（art_mini 直接编译） | 全语义 | 已含前缀/链/删除；补深键+并发 |
| 调度器 | `tests/sched/main.cpp` | ❌ | **公式复刻模型**（常量/公式对照 sched.cpp 行号）+ multimap 替身 | 账本数学 | QEMU sched_bench trace（真码）或 sched_core.h 抽取 |
| 内核整体 | ISO | ✅ | 无 | QEMU TCG 冒烟 | KVM/真机、fault injection、压力负载 |

---

## 7. 最小可执行补测路线图

| 优先级 | 项 | 预计工作量 | 依赖 | 产出物 |
|---|---|---|---|---|
| **P0** | `test_art_true_prefix_key` + `test_promote_inline_capacity_reuse` + `test_insert_fail_no_dangling`（三个最小回归） | 0.5 天 | 无 | 三个独立可跑用例 + 金样 |
| **P0** | 报告更正：五条表述降级/上下文补全（op 定义、环境、方差、种子、150s 冒烟） | 0.25 天 | 无 | 修订版 stability-report.md |
| **P0** | `test_boundary_sizes`、`test_krealloc_shrink_grow`、`test_kcalloc_overflow`、`test_oom_path`、`fragmentation_steady_state` | 0.5-1 天 | shim 加故障注入开关 | SLUB 边界+OOM+碎片证据 |
| **P1** | `test_size_class_throughput` + `test_latency_distribution`（分档+延迟，批量计时+预热） | 0.5 天 | 度量规范 | 分档吞吐/延迟表（带上下文） |
| **P1** | `test_fc_path_throughput` + `test_fc_hit_rate` + `test_key_length_sweep` + `test_flush_writeback_content` + `test_broadcast_pinned` | 1 天 | 无 | fc 分路径/命中率/深键证据 |
| **P1** | `sched_ref_eevdf` + `sched_wake_sleep_model` | 1 天 | 无 | 双模型对照 + 尾延迟数据 |
| **P1** | live_pages 构成 dump（`dump_live_pages_sites`） | 0.25 天 | 无 | 9 页机制解释 |
| **P2** | `test_multithread_percpu` + `test_fc_concurrent`（shim 多 CPU + 真并发） | 1-2 天 | shim 升级（线程局部 this_cpu、真实自旋锁压力） | SMP 语义宿主级证据 |
| **P2** | 内核 `sched_bench` 扩展 + QEMU 串口 trace 采集分析 | 1-2 天 | 内核重建流程 | 真实代码 SMP 账本曲线 |
| **P2** | CI：`tests/ci.sh` + GitHub Actions + 金样 diff | 0.5 天 | P0/P1 用例金样 | 回归门禁 |
| **P2** | 修复前后性能对比（art 守卫分支、TryGetSize 复用校验的开销） | 0.5 天 | 需可切换的对照构建 | 开销量化（预期 <1-3%，若 >5% 需优化） |

**执行顺序建议**：P0 全部 → P1 全部 → P2 按需（P2 的多线程与 QEMU trace 价值最高，CI 与性能对比可后置）。
