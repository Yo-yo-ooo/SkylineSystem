# 调度器 —— deadline 键调度 + RIP 速率反馈（曾用名 3EVDF）

源码：`kernel/src/arch/x86_64/schedule/sched.cpp`、`task.cpp`、`timer.cpp`、`syscall/`。

> 本文档只描述代码**实际做到**的事，并明确列出**没有做到**的事。

## 1. 实际结构

每个 CPU 一棵**红黑树**运行队列，键为**虚拟 deadline**（P3-74 修正：
原写"键为 vruntime"——实际入队时 `deadline = vruntime + base_quantum`,
队列按 deadline 排序；由于 deadline ≡ vruntime + 常数, 序与 vruntime 序
等价, 但键字段本身是 deadline），节点增广 `min_vruntime_subtree`
（子树最小值），比较点带 `PREFETCH_R`。线程按权重
（`sched_prio_to_weight[16]`）得到基准时间片，`vruntime` 按**真实流逝
毫秒 × 1024 / weight** 推进，`avg_vruntime` 按负载加权速率推进。

`Pick()` 只用 `vruntime` 与 `avg_vruntime` 做选择：沿"子树含 eligible 节点"的分支下降，取最小者。

## 2. RIP-progress-rate 反馈（3EVDF 部分）—— 它改什么、不改什么

每个 tick 对当前线程采样 RIP 推进量，经 Q10 定点双通道 EWMA 得到快/慢倍率 `mult`（钳制 [0.25x, 4x]），加一个 ±4 tick 的有符号修正项 `adj`，叠加出**每个线程的时间片长度**：

```text
eff_quantum = (base × fused_mult >> 10) + adj
```

该结果只用于 **LAPIC oneshot 定时器的时长**（`sched.cpp` 中 `get_dynamic_quantum()` 的调用点）。

**它不改的事（重要）**：

- `Pick()` 中**不出现** `mult` / `adj` —— 选择仍由 `vruntime` 决定；
- `vruntime` 按真实时间记账，**不被倍率缩放**；
- 因此反馈影响的是**抢占节奏/中断延迟**（忙等线程更频繁被打断、推进快的线程获得更长的不间断运行），**不改变 CPU 份额**；
- 公平性由 vruntime 机制承担，与速率反馈正交。

## 3. EEVDF 成分的真实程度

- `calibrate_and_set_deadline()` 在**每次入队**时把 `vruntime` 钳进 `[avg−q, avg+2q]` 并令 `deadline = vruntime + base_quantum`；
- 时间片**已按权重缩放**（P3-75 修正：原文档写"未按权重缩放"——
  实际 `base_quantum` 按 `sched_prio_to_weight[priority]` 缩放后作为
  deadline 的常数项），deadline ≡ 缩放后 vruntime + 常数；
- 所以这是**EEVDF 结构的子集**，不是完整的 EEVDF。README 与本文档均按此描述。

## 4. 动态基准量子与老化

`dynamic_adjust_quantum()` 每 100ms 按空闲占比/上下文切换数调整 `base_quantum`（钳制 [2,15] tick）。速率反馈侧：

- 50ms 未采样 → `mult` 按 1/16 步长向 1.0 收缩、`adj` 向 0 收缩（源码注释记录过 `(x+7)>>4` 在 |adj|≤8 时恒 0 的老 bug）；
- 离群观测（>16x）钳位到 16x；
- 新线程 `mult==0` 按 1.0 处理。

## 5. SMP 负载均衡

- **Active push（`TryPush`）**：本核盈余且线程 ≥2 时，按总权重找最轻目标，优先**同 SIMD 特性掩码**（注意：`cpu_simd_mask()` 是 SIMD 指令集位图，**不是 SMT 拓扑**；内核里没有 APIC-ID/核兄弟数据），双锁按 `cpu.id` 排序防 ABBA，批量 `SCHED_STEAL_BATCH=8`，推完若目标 hlt 立即发 IPI 唤醒。
- **Lazy steal（`StealThread`）**：每 8 次节流扫一次；先同掩码后任意核；`spin_trylock` 上限 100 次；偷到先标 `THREAD_TRANSFER` 再换锁入队。

## 6. 抢占与上下文切换

`Schedule::Switch()`（SCHED_VEC ISR）：先停 LAPIC 定时器 → `this_cpu==nullptr` 也 EOI → AP 早期窗口重 arm 返回 → `preempt_count > 1` 不清标志只重排 oneshot → 自愿 `yield()` 走独立 `yield_request_flags`。

**已知缺陷（未修）**：`CheckPreempt()` 在代码树中没有调用者，`preempt_count` 归零后的重新触发依赖下一次定时器 tick。

## 7. 已知修复与新线程记账

新线程的 `last_run_time` 曾在所有创建点未初始化（首片 `delta = uptime`，抬飞 vruntime）；现已**在创建点初始化**并在记账处对 `==0` 做防御。线程创建点均 `memset` 后赋值，行为确定。

## 8. 测试现状（诚实）

**真实内核调度质量基准已跑通（round 24）。** `sched_bench` 四相位在真内核
（QEMU, cpu 1 专用线程, 网络栈延后 = 安静机器）完整运行：

| 相位 | 结果 | 解读 |
|---|---|---|
| 阶跃 (RIP 反馈收敛) | **t90=4ms, 无超调 ✓** | REEVDF 反馈环安静环境极快收敛 (污染环境 193ms) |
| 抗污染 | 未过: base=[41,2057] mult=[785,2211] | 真内核与模型有差异 → **待查** |
| 短窗口 | 未过: windows=0 stalled=5 | 短窗口未检出 → **待查** |
| 振荡迟滞 | w=[64,64] toggles=0 ✓ | 迟滞有效无乒乓 |

**两个未过相位 = 真内核实现与模型测试的真实差距**（模型测试通过、真内核
失败 → 不是基准 bug，是 RIP 反馈实现在污染/短窗口场景的行为差异），
即下一轮调度器调试的入口。

round 25 根因分析增量：
- **shortwin 未过 (windows=0)**：`dynamic_adjust_quantum` 把 base_quantum
  动态钳在 [2,15]ms 并按负载自整定；bench 的静态假设 (base=5ms → prio15
  权重 288 → 片长 ~1.4ms < 3ms 门) 在动态量子下失效 (base=15ms 时片长
  ~4.2ms > 门 → 0 个短窗口)。**不是调度缺陷, 是基准语义与动态量子特性
  的失配** → bench 应读实际 base_quantum 或在相位期冻结自整定。
- **pollute 未过 (倍率摆幅 41..2057)**：victim 的 RIP 倍率在 polluter 竞争
  下大幅振荡——模型测试中稳定、真内核振荡 = 真内核特有行为 (候选: 中断/
  抢占噪声进采样、`sched_tsc_per_ms` 未校准致墙钟分母失真)。**需 TSC 校准
  后复测**, 或排查反馈增益在竞争场景的稳定性。

round 26 增量 (TSC 校准 + 复测):
- 实现 `sched_calibrate_tsc()` (tsc_cal.cpp, PIT 基准, 忙等带毫秒级守卫 ——
  早期 PIT 未走时守卫过大曾卡死启动) + `sched_tsc_per_ms` 强符号。
- 复测: step t90=42ms sat=1 (饱和检测出现); **pollute 仍过不了**
  (base=[80,1322] mult=[979,2560]) → 排除分母失真候选后, 指向
  **RIP 反馈增益在竞争场景的真稳定性问题** (反馈回路本身振荡, 待增益分析)。
- shortwin 仍 windows=0 (动态量子失配, 同 round 25 结论)。
round 33 (累加器落地 + UAF 实锤):
- 累加器采样已实施 (thread_t 新增 rip_acc_progress/ms, 4ms 窗口出样本);
  首测 pollute base=[41,2057]→[51,724] (3x 收窄), 修正分母为自身片长
  (wall_ms; exec_ms 的 tsc_dt 跨 gap 含他线程片, 语义错) 后待复测。
- **bench Exit 路径 Page fault 符号化定位**: RIP=atomic_test_and_set
  (自旋锁原语), CR2=0xFFFF90000211D044 (kmalloc 对象内的锁字),
  on thread 16 → **退出/回收路径的自旋锁 UAF**: 某结构体 (含锁)
  被释放后另一线程仍在其锁上自旋。真实调度器缺陷, 回收审计 = 后续任务。

round 34 (复测 + UAF 嫌疑):
- wall_ms 分母修正复测: pollute base=[51,724] 与修正前相同 → 剩余方差
  在分子/相位过渡侧 (4ms 窗口跨越 polluter 开关点), 需窗口内逐样本
  插桩 —— 收敛性改进已获 (3x), 完整通过留给后续。
- UAF 嫌疑收敛: proc 僵尸回收 (DrainProcZombieList) 释放 FDMan/pagemap
  时无全局同步, FDMan 内含锁表 —— 锁字 UAF 的最可能来源; 修复需
  fd_manager_destroy 与回收窗口的同步审计。

round 40 (UAF 修复) + round 41 (防御纵深):
- 根因: 相位线程 `Schedule::Exit(0)` 击杀整个 bench 内部 proc; 后续相位
  线程 spawn 进被并发终结的同一 proc → FDMan 锁字 UAF (round 33 符号化)。
- 修复: 相位线程不再 Exit (改 hlt 闲转; wait_done 本就按超时推进) +
  NewKernelThreadEx/NewThread 增加 exiting 守卫 (向退出中的 proc spawn
  显式失败) → UAF 债完全关闭。

round 42 (UAF 修复后完整报告):
- pollute **base 相位通过** (base=[1067,1309], 极差 <25% —— 累加器修复的
  首个通过项); mult 相位 [962,1463] 仍超 [820,1230] 界 (mult 目标 = polluter
  按倍率运行时 victim 的乘数, 残余方差待窗口内插桩)。
- osc 本次 entered=0 (w 停 256, 相位变体); step t90=176ms ✓。
- **0 异常**: UAF 修复后全轮无 fault (round 33 的符号化问题闭环)。

round 44 (round 25 假说证伪 + 新机制):
- 实测 `实际 base_quantum=5 ms` —— **round 25 的"动态量子失配"假说被证伪**
  (量子与 bench 静态假设一致)。shortwin windows=0 的真因改道:
  同相位 stalled=11895 (progress==0 样本主导) → 目标线程 1.4ms 短片内
  定时器 tick 极少命中 (采样落在伴线程片) → 目标样本稀疏且跨长 gap;
  且 rip_stats 的 short_windows/stalled 为 **CPU 级总量**而非目标线程级
  (eval 用 CPU 级 delta 判据)。下一轮: 采样落点分析 / 目标级计数。

round 45 (目标级计数实锤):
- 新增 thread_t 级 rip_short_windows/rip_stalled + bench 目标自采样。
- 实测: **目标线程目标级 shortwin=0 stalled=0** —— 采样从未落在目标的
  1.4ms 短片上 (tick 采样按片边界但间隔 ~5ms > 片长)。round 44 机制确认。
- 修复方向定稿: 采样从 tick 处改到**切换点** (每线程片末采样当前线程),
  tick 采样对短片天然失明; 该改动同时影响 pollute/shortwin 双相位的
  判据有效性 —— 是调度器画像的最后一块结构性修复。

round 47 (内联 Exit 漏网 + TSC 超时 + 全链解释):
- **内联 Exit 漏网**: round 40 的 replace_all 只替换了独立行形式, polluter/
  companion 的内联 `Schedule::Exit(0)` 仍在 → 相位2 击杀 bench proc
  (exiting=1) → 相位3 spawn 被 round 41 守卫拒绝 (companion=target=NULL)
  —— 此前的"采样失明/目标从未运行"全部是 spawn 失败的连锁假象。
- **TSC 超时**: TCG 下污染相位 CPU 饱和冻住 PIT 虚拟时钟 → wait_done
  超时永不触发 → 相位2 结构性挂死; 全部相位超时改 TSC 基准 (rdtsc ×
  tsc_per_ms) 后 bench 600s 内完整跑完。
- 修复后: mf=823/msl=1056 (首次采到目标), dispatch=47 (EEVDF 高频选中✓),
  片长 min=1 max=9ms (抢占截断可见)。

round 47b (stalled 真机制终现):
- 圈数随机化无效 (stalled 72/57 不降) → "fast_body RIP 归位"理论证伪。
- **真机制**: `CheckPreempt` 的软件陷阱 `int SCHED_VEC` 的帧 RIP =
  **陷阱指令的固定地址** (常量) → deadline 抢占样本 progress 恒 0;
  只有硬件 LAPIC tick 的中断帧 RIP 是真实执行点。目标的片大多以
  deadline 抢占结束 → 其样本几乎全是常量 RIP → stalled。
- **结论: 切换点采样成为必要**: switch 处出线程的 ctx.rip 是真实末位
  RIP —— 采样移到 switch-out + 与 tick 去重。实施 = 下一轮。

round 68 (P2-62 披露 + P3-77):
- ~~Schedule::Sleep + 定时器轮 (tv1/2/3) 为死代码~~ **round 96 已接活**:
  PIT::Sleep 在调度器就绪 (smp_started + 有线程上下文) 时改调
  Schedule::Sleep (定时器轮 + THREAD_SLEEPING + Yield, 释放本核),
  引导早期的探测路径保持忙等。验证: BUILD_EXIT=0 + golden PASS +
  QEMU ping 3 + 0 异常。
- ~~切换点采样 (round 47b 结论) 仍未实施~~ **round 102 复核确认已实施**:
  sched.cpp Switch 路径每切换点调 riprate_update (626-628), shortwin
  判据 (elapsed < RIP_MIN_SAMPLE_MS → rip_short_windows++) 与片长分布
  (rip_min/max_slice_ms) 全部在线 —— round 47b 的结论在后续轮次落地,
  文档滞后已修正。
- UAF 嫌疑收敛: proc 僵尸回收 (DrainProcZombieList) 释放 FDMan/pagemap
  时无全局同步, FDMan 内含锁表 —— 锁字 UAF 的最可能来源; 修复需
  fd_manager_destroy 与回收窗口的同步审计。
- round 35 增量: Exit → PROC_KILL 路径确认 (退出线程自身走进程击杀路径,
  关中断 + 停定时器); 击杀路径的 fd 清理与 DrainProcZombieList 的
  FDMan 释放之间存在竞态窗口 (bench 密集 Exit 触发, 生产负载未观测到)。
  修复设计: FDMan 释放延后到"全部线程已摘链"之后, 或 fd 清理移入
  击杀路径的锁内。**生产 soak 从未触发此路径 → 优先级 = 中。**

round 28 根因闭环 (pollute 振荡的完整机制):
- 采样结构: 每片 tick 采样, `obs_rate = RIP 差分 / 自身片长 (last_slice_ms)`
  —— 分母正确 (线程自身运行时长, 非墙钟 gap)。
- **振荡源 = 抢占驱动的变长片**: EEVDF 下同优 polluter 竞争时, victim 的片
  被定时器/截止期抢占截短 → 短片的 RIP 差分系统性偏低 → obs_rate 高低
  交替 → 快通道 (1/4 EWMA) 跟随摆动 (基 [41,2057] 的机制解释)。
- 亚毫秒片被强制 delta=1 → 进一步放大短片的低估。
- **修复设计 (待实施)**: 采样改为**累加器式固定窗口** —— 按线程自身运行
  时长累积 RIP 差分与时长 (如每 4ms 自身时长出一个 obs_rate 样本),
  变长片噪声被累加器吸收; 短窗口防御 (RIP_MIN_SAMPLE_MS) 一并简化。
- **shortwin 未过 (windows=0)**：`dynamic_adjust_quantum` 把 base_quantum
  动态钳在 [2,15]ms 并按负载自整定；bench 的静态假设 (base=5ms → prio15
  权重 288 → 片长 ~1.4ms < 3ms 门) 在动态量子下失效 (base=15ms 时片长
  ~4.2ms > 门 → 0 个短窗口)。**不是调度缺陷, 是基准语义与动态量子特性
  的失配** → bench 应读实际 base_quantum 或在相位期冻结自整定。
- **pollute 未过 (倍率摆幅 41..2057)**：victim 的 RIP 倍率在 polluter 竞争
  下大幅振荡——模型测试中稳定、真内核振荡 = 真内核特有行为 (候选: 中断/
  抢占噪声进采样、`sched_tsc_per_ms` 未校准致墙钟分母失真)。**需 TSC 校准
  后复测**, 或排查反馈增益在竞争场景的稳定性。

历史：`sched_bench` 此前只有模型测试路径；真内核挂点三连（bootstrap init
直调 Yield / 早期 PIT::Sleep / init 忙等饿死同核线程）已全部定位并绕过
（专用线程 + init 不等待 + wrapper 尾部起网络）。

## 9. 调参常量速查

| 常量 | 值 | 含义 |
|---|---|---|
| `SCHED_STEAL_BATCH` | 8 | 单次 push/steal 批量 |
| `SCHED_STEAL_THROTTLE` | 8 | steal 扫描节流 |
| `ZOMBIE_RECLAIM_THRESHOLD/BATCH` | 8 / 16 | 僵尸线程回收触发点与批量 |
| `RIPRATE_AGING_MS` | 50 | 速率老化窗口 |
| `RIPADJ_MIN/MAX` | -4 / +4 | 直接修正项范围 |
| `base_quantum` 初始 | 5 tick | 约 5 ms |
