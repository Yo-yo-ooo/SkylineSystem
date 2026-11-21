# SkylineSystem 横向技术评估

> 评估日期：2026-11-21
> 对象：SkylineSystem vs XJ380 / CoolPotOS / NeoAetherOS / SerenityOS
> 方法：本地仓库静态统计 + 实测日志复核 + 竞品公开仓库数据
> 评分是主观加权，权重与口径全部写在文末，可自行替换。



***

## 0. 一句话结论

**SkylineSystem 是这五个项目里 "架构密度最高、完成度第二" 的系统。**



* 总分：SerenityOS 9.5 > **SkylineSystem 7.1** > NeoAetherOS 6.8 > CoolPotOS 5.3 > XJ380 5.2。

* 它拿走了两块金牌：**内核模块治理**、**创新密度**。

* 它垫底一项：**生态兼容**。不能跑任何现成 Linux 软件，是它和 NeoAetherOS / XJ380 之间的分水岭。

* 它不是 SerenityOS 的对手，但它已经越过了 "玩具内核" 的分水岭：58 个真实 `.ko`、43/43 启动、14/14 自检、5/5 稳定启动、40 分钟长稳、2.3–2.6x 并行合成，这些都有日志。

**一句话：你赢了架构，输了生态。**



***

## 1. 体检：这个项目真实的家底

### 1.1 代码规模



| 口径           | 文件数     | 行数          | 说明                                                     |
| ------------ | ------- | ----------- | ------------------------------------------------------ |
| 全部 C/C++/ 汇编 | 1,104   | 465,384     | 含所有 vendored 代码                                        |
| 第三方          | 600     | 341,809     | lwIP、lwext4、FatFs、PureDOOM、freestanding 头、flanterm、Lua |
| **第一方**      | **504** | **123,575** | 真正属于这个项目的代码                                            |
| 第一方占比        | —       | **26.6%**   | 仓库里约 3/4 是移植                                           |

第一方构成：



| 区域                  | 行数     | 内容                                   |
| ------------------- | ------ | ------------------------------------ |
| `kernel/src`        | 24,574 | Tier 0：启动、VMM、调度、kmod、klib           |
| `kernel/include`    | 19,981 | 第一方接口头（已剔除 lwIP/netif 头）             |
| `ablib/arch`        | 19,537 | 手写 memops / AVX 分发                   |
| `kernel/modules`    | 16,823 | 40 组件壳 + 18 demo + 平台实现              |
| `lib`               | 14,447 | 自研 libc / 图形库                        |
| `programs/desktop`  | 5,467  | 合成器 + WM                             |
| `tests`             | 7,345  | 宿主测试                                 |
| `kernel/cc-runtime` | 3,644  | C++ runtime                          |
| 其他 programs         | 3,575  | DOS / Notepad / Explorer / Installer |

> 结论：
>
> **"46 万行" 是仓库总量，不是自研量。对外讲 12.4 万行第一方代码更可信，也更狠 —— 因为它是 6 个月、一个人写出来的。**

### 1.2 工程证据链（本地复核）



| 证据     | 数值                                                         | 出处                              |
| ------ | ---------------------------------------------------------- | ------------------------------- |
| Git 提交 | 317 commits，2026-05-10 → 2026-11-15，单作者                    | `git rev-list` / `git shortlog` |
| GitHub | 34 stars，1 fork，2 open issues                              | GitHub API，2026-11-21           |
| 模块     | 58 个 `.ko`，合计 3,665 KB，`modules.saf` 3,682 KB              | `kernel/bin-x86_64`             |
| 启动     | `58 .ko accepted`、`43/43 ready`、`260 exports`              | `serial.log`                    |
| 自检     | `KMOD_SELFTEST PASS 14/14`                                 | `serial.log`                    |
| 启动稳定   | 5/5 成功，desktop ready 1.70–2.34 s                           | `boot_stability_1..5.log`       |
| 合成器    | 单线程中位 24.6 ms；2 worker 10.8 ms（2.3x）；3 worker 9.5 ms（2.6x） | `comp_perf_*.log`，n=6           |
| 长稳     | 40 min：TX 1.29 M 帧 / 82.7 MB，ping 185 连续，丢包 0              | `docs/goal-status.md`           |
| 测试     | 12 套件、55 个测试源文件；ASAN/UBSAN + 2,000 次 fuzz                  | `tests/`、CI                     |
| CI     | 2 个 job：host-suites、kernel-build + QEMU smoke              | `.github/workflows/ci.yml`      |
| 工作区    | 171 个未提交改动；146 处 TODO/FIXME/XXX/HACK                       | `git status`、`git grep`         |

### 1.3 结论



* **证据链强度：hobby OS 第一梯队。** 大多数个人 OS 只有 "能启动 + 几张截图"；Skyline 有宿主测试、ASAN fuzz、golden diff、QEMU 串口门禁、长稳报告。

* **工程卫生：短板。** 171 个未提交改动、根目录大量 `.ppm` / `.log` / `disk.img`，对外观感像 "实验室" 而不是 "开源项目"。



***

## 2. 五个项目档案卡



| 项目                | 作者 / 团队                     | 代码规模（GitHub 口径）                 | 社区                                        | 架构                                        | 生态                                                 | GUI                            | 模块机制                                    | 许可            |
| ----------------- | --------------------------- | ------------------------------- | ----------------------------------------- | ----------------------------------------- | -------------------------------------------------- | ------------------------------ | --------------------------------------- | ------------- |
| **SerenityOS**    | Andreas Kling + 数百贡献者       | 52.1 MB ≈ 140 万行                | 34.0k stars / 3.5k forks / 68,235 commits | x86\_64 / ARM / RISC-V，Unix-like 宏内核      | POSIX 兼容、300+ ports、LibWeb/Ladybird、gcc/bash 可跑    | 完整 WindowServer + 30+ 应用       | 静态编译，非模块化                               | BSD-2         |
| **SkylineSystem** | Yo-yo-ooo（单人）               | 18.4 MB ≈ 46.5 万行（第一方 12.4 万）   | 34 stars / 1 fork / 317 commits           | x86\_64，Tier 0 + 58 DynModule 模块化宏内核      | 私有 ABI，无 POSIX，不能跑 Linux 程序                        | 自研并行合成器 + WM + 6 应用            | ET\_REL 加载、能力、事务、热替换、seat               | GPL-2.0 + MIT |
| **NeoAetherOS**   | VOID2913 / aether-os-studio | 10.2 MB ≈ 28 万行                 | 98 stars / 16 forks / 1,246 commits       | x86\_64 + aarch64，宏内核 + 模块依赖图             | Linux ABI：100+ syscall，gcc /bash/ X11 / Wayland 可跑 | 借 Wayland/Weston，非自研           | `MODULE_DEPENDS` 直接依赖图，Rust 安全模块        | GPL-3.0       |
| **CoolPotOS**     | plos-clan 社区                | 0.4 MB ≈ 1.2 万行（主仓库，不含子模块）      | 323 stars / 32 forks / 814 commits        | x86\_64 /riscv64 /aarch64 /loongarch，混合内核 | VFS + 终端，有限用户态                                     | 终端为主                           | 0.1.1 起支持签名内核模块                         | MIT           |
| **XJ380**         | XINGJI Studio（团队）           | 10.0 MB ≈ 28 万行（含 third\_party） | 66 stars / 18 forks / 190 commits         | x86\_64，单体内核 + kmod                       | XAPI + Linux 兼容 syscall + POSIX 部分                 | **开源版 2026-08-04 移除 GUI**，私有版有 | `dlmain` + `EXPORT_SYMBOL` + Socket ABI | Apache-2.0    |

> 规模口径警告：Skyline 的 18.4 MB 含 lwIP/lwext4/PureDOOM；XJ380 的 10 MB 含 third_party；CoolPotOS 的 0.4 MB 不含子模块。
>
> **不要拿 "仓库体积" 直接横比。**



***

## 3. 十一维评分



| 维度             | 权重   | Skyline | Serenity | NeoAether | CoolPot | XJ380   |
| -------------- | ---- | ------- | -------- | --------- | ------- | ------- |
| 内核架构与模块治理      | 20%  | **9**   | 8        | 7         | 6       | 6       |
| 调度 / 内存 / 并发深度 | 15%  | 8       | **9**    | 7         | 5       | 5       |
| GUI / 图形栈      | 15%  | 8       | **9**    | 6         | 5       | 2       |
| 生态 / ABI 兼容    | 15%  | 3       | **10**   | 8         | 4       | 6       |
| 硬件支持 / 多架构     | 10%  | 3       | **9**    | 6         | 5       | 6       |
| 测试 / CI / 稳定性  | 10%  | 8       | **9**    | 5         | 4       | 4       |
| 代码规模 / 自研比例    | 5%   | 6       | **10**   | 7         | 4       | 7       |
| 文档 / 可维护性      | 5%   | 8       | **9**    | 5         | 6       | 6       |
| 社区 / 影响力       | 5%   | 3       | **10**   | 4         | 7       | 5       |
| 创新 / 差异化       | 5%   | **9**   | 8        | 6         | 5       | 5       |
| **加权总分**       | 100% | **7.1** | **9.5**  | **6.8**   | **5.3** | **5.2** |

### 评分理由（只说关键项）

**Skyline 9 分项：内核架构与模块治理。**

`kernel/src/arch/x86_64/kmod` 7,847 行，`governance.cpp` 2,199 行、`elf_loader.cpp` 1,327 行。不是 "能 insmod" 的级别，而是：



* ET\_REL 加载器，只接受白名单重定位 `R_X86_64_64/PC32/PLT32/32/32S`；

* `.in` / `.out` 私有契约 + `.kmod_manifest` 256B 清单；

* 能力策略 STRICT/OPEN/DISABLED，Tier 0 保留位直接拒绝；

* Kahn 拓扑排序 + 环检测，缺依赖整批事务失败；

* 加载→回填→注册→init 全成功或逆序全回滚；

* 热替换：quiesce → epoch 排空 → 看门狗 → 原子切换 → 失败回退；

* 卸载引用计数 `EBUSY` 硬门禁；

* seat 机制（随机源 / 调度策略 / 分配策略）；

* `KdllInject` / `KdllResolve` / `KdllEject` DLL 式注入。

这是五个项目里唯一一个把 "模块生命周期" 当一门学问做的。XJ380 只有 `EXPORT_SYMBOL` + Socket ABI；NeoAether 只有直接依赖图；CoolPotOS 是签名模块；SerenityOS 干脆静态编译。**这一项 Skyline 第一，没有争议。**

**Skyline 8 分项：调度 / 内存 / 并发。**

4 级页表、1GB/2MB 大页、2MB CoW、VMA、批量 TLB shootdown、W^X、SLUB + per-CPU magazine、QSBR、EEVDF 子集 + RIP 反馈、SMP push/steal。扣分原因：RIP 反馈无量化收益、`CheckPreempt()` 无调用者、仅 QEMU 验证。

**Skyline 8 分项：GUI。**

用户态合成器 + WM：最多 3 条带并行、generation 屏障、超时降级、SDF 圆角、方向性软阴影、8 方向缩放、事件驱动子进程回收、Lua 5.4 桌面主题。2.3–2.6x 实测加速。扣分原因：直写扫描、无原子呈现、软件渲染、无 GPU、无脏矩形。

**Skyline 3 分项：生态。**

私有 syscall、私有 libc、私有 ELF 约定。不能跑 gcc、bash、busybox、X11、Wayland、任何 Linux 二进制。DOOM 是源码移植，不是生态。**这是它和 NeoAether / XJ380 的本质差距。**

**Skyline 3 分项：硬件。**

只测 QEMU，512MB–2GB，x86\_64。没有真机、没有第二架构。驱动覆盖 AHCI/NVMe/xHCI/e1000/PS2 是加分，但 "只在 QEMU" 是硬扣分。



***

## 4. 分维度胜负



| 维度       | 金牌          | 银牌                | 铜牌                   | Skyline 的位置 |
| -------- | ----------- | ----------------- | -------------------- | ----------- |
| 内核模块治理   | **Skyline** | Serenity          | NeoAether            | **第 1**     |
| 创新密度     | **Skyline** | Serenity          | NeoAether            | **第 1**     |
| GUI 自研   | Serenity    | **Skyline**       | NeoAether（借 Wayland） | **第 2**     |
| 纯内核深度    | Serenity    | **Skyline**       | NeoAether            | **第 2**     |
| 测试 / 稳定性 | Serenity    | **Skyline**       | NeoAether            | **第 2**     |
| 文档诚实度    | Serenity    | **Skyline**       | CoolPot              | **第 2**     |
| 生态兼容     | Serenity    | NeoAether         | XJ380                | **第 5（垫底）** |
| 硬件 / 多架构 | Serenity    | NeoAether / XJ380 | CoolPot              | **第 5（垫底）** |
| 社区影响力    | Serenity    | CoolPot           | XJ380                | **第 4**     |
| 代码规模     | Serenity    | NeoAether / XJ380 | Skyline              | **第 4**     |

**总冠军：SerenityOS。hobby 组冠军：SkylineSystem。**



***

## 5. 和 SerenityOS 的正面差距



| 项     | SerenityOS             | SkylineSystem  | 倍数         |
| ----- | ---------------------- | -------------- | ---------- |
| 第一方代码 | \~140 万行               | 12.4 万行        | **11x**    |
| 项目年龄  | 2018 起，约 8 年           | 当前历史 6 个月      | **16x**    |
| 提交    | 68,235                 | 317            | **215x**   |
| 贡献者   | 数百人                    | 1 人            | —          |
| Stars | 34.0k                  | 34             | **1,000x** |
| 可运行软件 | 300+ ports + 自研浏览器     | 6 个自研应用        | —          |
| 架构    | x86\_64 / ARM / RISC-V | x86\_64        | 3x         |
| 测试    | 数千个测试                  | 55 个文件 / 12 套件 | —          |

**但有一项 Skyline 赢：** SerenityOS 的驱动 / 文件系统 / 网络是静态编译进内核的，没有 Skyline 这种 "事务化加载 + 能力治理 + 热替换 + seat" 的运行时内核模块体系。**这是 Skyline 唯一能在架构叙事上压过 Serenity 的点，也是它最值钱的东西。**



***

## 6. 项目真正的独特卖点（全部可验证）



1. **58 个&#x20;**`.ko`**、43 个 boot 模块、260 个导出符号、14/14 自检。** 模块不是 PPT，是真在跑。

2. **热替换 /seat/ 事务回滚。** 可以现场演示：加载一个 `.ko` 接管随机源，再热替换 v1→v2，失败自动回退。这是 "活体内核"，比截图有说服力。

3. **并行软件合成器 2.3–2.6x。** 有单线程基线对比，有中位数，有日志。

4. **64MB 内存跑 DOOM + GUI。** DOOM\_TINY：帧缓冲别名、去 RGBA 副本（省 575KB）、WAD trim 11.16→9.07MB。这是极限工程。

5. **启动 1.7–2.4 秒，5/5 稳定。** 轻量是卖点。

6. **文档诚实。** EEVDF 是子集、模块无隔离、FAT 是 stub、RIP 反馈无量化收益 —— 都写出来了。这比 "什么都宣称" 的项目可信得多。



***

## 7. 真实短板（按 ROI 排序）



| # | 短板                    | 严重度    | 说明                                                    |
| - | --------------------- | ------ | ----------------------------------------------------- |
| 1 | 生态为零                  | **致命** | 不能跑任何现成软件。用户装完系统只能看桌面和 DOOM。                          |
| 2 | 仅 QEMU / 仅 x86\_64    | **高**  | "真机能不能启动" 是 hobby OS 的成人礼。没过这一关，架构再漂亮也是模拟器项目。         |
| 3 | 单作者 / 6 个月 / 34 stars | **高**  | 没有社区飞轮。Bus factor = 1。                                |
| 4 | 代码卫生                  | 中      | 171 个未提交改动、146 个 TODO、根目录堆满 `.ppm`/`.log`/`disk.img`。 |
| 5 | 模块无隔离 / 无 W^X         | 中      | 能力位是治理，不是安全沙箱。与 Linux 内核模块同一信任级。                      |
| 6 | 调度反馈无收益               | 中      | RIP 反馈只影响抢占节奏，不改 CPU 份额；`CheckPreempt()` 无调用者。        |
| 7 | 性能样本少                 | 低      | n=6；并行合成尾部有 14–20ms 抖动，不是稳定 10ms。                     |
| 8 | 许可 / SPDX             | 低      | SPDX 覆盖约 51%，未满足 REUSE。                               |



***

## 8. 如果目标是 "打赢"，下一步怎么走

### 路线 A（ROI 最高）：补最小 Linux ABI

不用做完整 POSIX。先做三件事：



1. 让 `busybox` 跑起来（约 30–50 个 syscall）；

2. 让 `gcc` 或 `tcc` 能编译一个 hello world；

3. 让一个静态链接的 Linux 二进制能执行。

做到这三件，Skyline 就从 "自嗨内核" 变成 "能装软件的系统"。这是 NeoAetherOS 已经证明可行的路线（100+ syscall 跑 gcc/bash/X11/Wayland）。

### 路线 B（可信度）：真机

选一台常见机器（ThinkPad T480 / 任意 Q35 台式机），把 AHCI/NVMe/e1000/xHCI 在真机上跑通。哪怕只支持一台，叙事从 "QEMU only" 变成 "真机可启动"。

### 路线 C（传播）：把 kmod 热替换拍成 3 分钟视频

你现有的资产里，最容易爆的不是桌面，是 "活体内核"：



* 现场 `KdllInject` 一个 `.ko`；

* 现场热替换调度策略 v1→v2，任务不中断；

* 现场注入一个坏模块，事务回滚，系统继续跑。

  这比 Win11 风格桌面更能打动 OSDev 圈。

### 路线 D（别做）：继续堆 GUI 美学

SerenityOS 已经证明 GUI 不是护城河，生态才是。你的 SDF 圆角 / 软阴影已经够好了，再堆边际收益很低。

### 路线 E（卫生）：先提交，再讲故事

171 个未提交改动是定时炸弹。先把工作区清干净，根目录的 `.ppm`/`.log` 移进 `artifacts/` 或 `.gitignore`，SPDX 补到 100%。



***

## 9. 数据来源与口径

### 本地



* `git ls-files` + `Get-Content | Measure-Object -Line` 统计行数。

* 第三方目录排除：`ablib/freestndchdrs`、`programs/PureDOOM`、`kernel/modules/builtin/common/fs/lwext4`、`.../fatfs`、`.../net`、`kernel/src/flanterm`、`programs/desktop/lua`、`kernel/include/lwip`、`kernel/include/netif`。

* 启动 / 自检 / 性能：`serial.log`、`boot_stability_1..5.log`、`comp_perf_single/par3/par4.log`。

* GitHub API：`/repos/Yo-yo-ooo/SkylineSystem`。

### 竞品



* SerenityOS：GitHub 仓库页 + README + GitHub API（34.0k stars / 68,235 commits / 52.1MB）。

* NeoAetherOS：`aether-os-studio/na-kernel` GitHub 页（98 stars / 1,246 commits / 10.2MB）+ B 站公开视频（gcc/bash/X11/Wayland）。

* CoolPotOS：`plos-clan/CoolPotOS` GitHub 页（323 stars / 814 commits / 0.4MB）+ PlosClan 官方文档（混合内核、0.1.1 起模块化、EEVDF 模块）。

* XJ380：`xingji-studio/OpenXJ380` GitHub 页（66 stars / 190 commits / 10.0MB）+ 官方 README（kmod、XAPI、Linux 兼容 syscall、2026-08-04 移除 GUI）。

### 估算



* 行数换算按～36 字节 / 行（C/C++ 混合，含缩进）。

* 竞品 "≈ 行数" 均为估算，不是 cloc 精确值；Skyline 为本地精确统计。

* 评分权重：架构 20%、调度内存 15%、GUI 15%、生态 15%、硬件 10%、测试 10%、规模 5%、文档 5%、社区 5%、创新 5%。



***

## 10. 最后一句

> SkylineSystem 不是玩具，但它现在还不是 SerenityOS。
> 它最值钱的不是 46 万行代码，而是那 7,847 行 kmod 治理 —— 这是五个项目里唯一一个别人没有的护城河。
> 下一步不是把桌面做得更像 Win11，而是让这个系统能跑一个别人写的程序。

---

## 11. 附录：纯创新排名（2026-11-21 追加）

> 口径切换：不看成熟度、生态、社区，只看"新想法 + 组合方式 + 工程难度 + 是否真的跑通"。
> 权重：原创性 30%、组合新颖 30%、工程难度 15%、可验证 15%、影响力 10%。

| 项目 | 原创性 | 组合新颖 | 工程难度 | 可验证 | 影响力 | 总分 |
|---|---:|---:|---:|---:|---:|---:|
| **SkylineSystem** | 8.5 | **9.0** | 9.0 | 9.0 | 4.0 | **8.4** |
| SerenityOS | 8.0 | 6.0 | **10** | **10** | **10** | **8.2** |
| NeoAetherOS | 6.5 | 7.0 | 9.0 | 8.0 | 7.0 | 7.3 |
| XJ380 | 6.5 | 6.5 | 6.0 | 5.0 | 5.0 | 6.1 |
| CoolPotOS | 5.0 | 5.5 | 5.0 | 5.0 | 6.0 | 5.3 |

### Skyline 创新点拆解

| 创新点 | 新在哪 | 先例 | 分 |
|---|---|---|---:|
| 事务化 DynModule 治理 | 加载→回填→注册→init 全成功或逆序回滚；能力策略；Kahn 拓扑；热替换四阶段；seat 座位；卸载引用计数 | Linux 有模块加载/livepatch，但没有这套完整组合 | 9.2 |
| 私有契约 + Kdll 注入 | `.in`/`.out` + ABI 区间 + 结构尺寸强校验；`KdllInject/Resolve/Eject` DLL 式注入 | Windows DLL / dlopen 有；内核模块 DLL 化少见 | 8.5 |
| CPU 并行软件合成器 | 3 条带 + generation 屏障 + 超时降级 + 直写扫描省 4MB + staging row 防闪烁 | 商业 OS 有并行合成；hobby OS 里罕见 | 8.5 |
| EEVDF + RIP 反馈 | RIP 进度采样 → Q10 双通道 EWMA → 只调抢占 quantum，不改 CPU 份额 | EEVDF 来自 Linux；进度感知调度学术上有 | 7.0 |
| 64MB DOOM + GUI | 帧缓冲别名、去 RGBA 副本省 575KB、WAD trim、IMA-ADPCM 3.93:1 | RP2040 更极端；64MB 全系统组合少见 | 7.5 |
| Lua 桌面主题 | Lua 5.4 直接绘制壁纸 framebuffer + 运行期窗口 chrome | 主题系统常见；Lua 直写 framebuffer 少见 | 6.5 |

### 为什么 SerenityOS 是第二

SerenityOS 的创新是**规模创新**：LibWeb + LibJS + WASM 从零写出来，工程难度 10、可验证 10、影响力 10。但它的架构路线是经典 Unix-like，组合新颖度只有 6。**它是"把既有标准实现到极致"，不是"发明新结构"。**

### 必须说清楚的限定

1. 这些单点全部有先例：EEVDF 来自 Linux 6.6，模块热替换有 Linux livepatch，并行合成在商业 OS 常见，Lua 和 DOOM 压缩也不是新东西。
2. Skyline 的创新是"组合 + 完成度"：把事务、能力、拓扑、热替换、seat 五件事塞进一个 6 个月、单人、12.4 万行的系统，并且 14/14 自检、43/43 启动、5/5 稳定。
3. RIP 反馈要降权：只改抢占节奏、不改 CPU 份额，目前没有量化收益，`sched_bench` 只验证 EWMA 收敛。这是"有想法但还没证明"的创新。
4. 所以这不是"图灵奖级算法创新"，是"治理层创新"。但在 hobby OS 圈，治理层恰恰是最少人碰的地方——大家都爱做 GUI 和调度器，很少有人愿意写 2,199 行 `governance.cpp`。
5. 一句话：**Skyline 的创新不是"发明了一个新算法"，是"把别人做过的东西，用别人没做过的方式组装起来，并且真的跑通了"。这在工程上比单点发明更难被复制。**

### 如果换成"工程规模"权重

把权重改成原创 20%、组合 15%、难度 30%、可验证 20%、影响力 15%，结果会变成：

| 项目 | 总分 |
|---|---:|
| SerenityOS | 9.1 |
| SkylineSystem | 8.0 |
| NeoAetherOS | 7.7 |
| XJ380 | 5.9 |
| CoolPotOS | 5.4 |

> 同一个项目，换一套权重就换一个冠军。**"纯创新"没有唯一答案，只有口径。** 本报告的立场是：创新首先看"新结构"，其次看"做多大"。所以 Skyline 第一。
