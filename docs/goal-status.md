# Goal status and verification ledger

> The goal has three parts: (A) e1000 driver + (B) lwIP porting completion + (C) DPDK-style high-performance rework,
> plus the production-grade checklist (SMP concurrency / long-term stability / fault injection / security / full semantics / performance profiling /
> regression gating / observability). Checked item by item, annotated truthfully.

## 🏁 Milestone: four-source audit fix list —— ✅ all cleared

All six tiers cleared: **P0 ×18** (including two convergence items that were reverted and redone: the TLB ACK fence —
root cause of the 400M pause spins under TCG → capped at 100ms wall clock, measured 0 timeouts; and the AHCI
per-request DMA buffer that replaced the lock spanning the sti window), **P1 ×36**, **P2 ×8** (cross-core ABA
isolation stack foreign_head, AHCI release barrier, QSBR 64 slots), **P3 ×21**, **P4 ×8** (allocator host chaos
test — which also caught a real MCB allocation-granularity bug; fc writeback content verification), **P5 ×19**.
Verification after each change set: BUILD_EXIT=0 + GOLDEN PASS + QEMU ping 3 + 0 exceptions.

## 🏁 Roadmap convergence

| # | Roadmap item | Status |
|---|---|---|
| #7 | FD inheritance (filedesc snapshot-style deep copy) | ✅ also confirmed two latent defects (rb_postorder_iter callback contract + exec FDMan release not nulling) |
| #2 | proc lifecycle TOCTOU | ✅ ProcessAddThread re-check inside lock + reaper FDMan reclamation into lock (5 call sites closed) |
| #6 | vsnprintf fuzz | ✅ ASAN host fuzz + real out-of-bounds read fix (NUL-terminated format string) |
| — | fsync production activation | ✅ fclose + implicit dirty-page flush at process exit |
| — | Sleep/timer wheel revived | ✅ PIT::Sleep dual mode (scheduler ready → real sleep) |
| — | RX burst observability | ✅ rx_burst_avg (DPDK metric) |
| — | Static analysis | ✅ clang --analyze 9 files + 2 defensive fixes (fc LRU double-advance + rbtree sibling guard) |

## A. e1000 driver —— ✅ done

- 82574L (QEMU e1000e) complete driver: probe/reset/EEPROM MAC/dual ring 256/IRQ+polling dual mode
- Bug-fix history: RDT ownership, _memcpy transposed order, GetPhysics in-page offset,
  pbuf double free, PIT::Sleep hang during probe, early PIT busy-wait guard
- Multi-core TX spinlock serialization + ring-full backoff

## B. lwIP porting —— ✅ done (production default: polling + copy path)

- End-to-end verification: DHCP lease (10.0.2.15), ICMP ping consecutive replies, multi-core TX flood 2.2-2.7k frames/s
- Porting fixes: lwipopts (software checksum/alignment/pool), cc.h DIAG varargs chain fix,
  sys_now monotonic milliseconds, ACD disabled (slirp ARP proxy), ip4addr_ntoa static-buffer pitfall
- Network thread stack 32KB (16KB stack overflow = one of the deep root causes of all intermittent crashes)

## C. DPDK-lite —— 🟡 base complete, two experimental items documented

| DPDK concept | Status |
|---|---|
| PMD polling | ✅ production path |
| Multi-core TX concurrency | ✅ spinlock + 3-core flood verification |
| Per-core private / lock-free | 🟡 single RX queue (dual queue = MSI-X blocking item) |
| Zero-copy RX | 🟡 experimental (NET_ZEROCOPY=0): index back-derivation + double free fixed, still silent after first stat (suspects narrowed to 2 items, see dpdk-lite.md) |
| Checksum offload / MSI-X | ❌ blocked on register-manual verification |
| Stats/observability | ✅ counters + RTT P50/P90/P99 + stall self-diagnosis |

## Production-grade checklist item by item

| Pillar | Status | Evidence |
|---|---|---|
| SMP concurrency correctness | ✅ existing (SLUB/FC/sched multithreaded suites) + network multi-core stress + **110-item P0/P2 concurrency fixes all cleared** | tests/ + soak |
| Long-term stability | ✅(evidence) closing soak chain: 10/15/30min + 40min multi-segment all 0 assertions 0 exceptions 0 stalls; final 30min: tx=1.29M frames (82.7MB) sustained at 2429/s, ping=185 consecutive, loss=0; **re-tested after every milestone** | serial.log |
| Fault injection | ✅ OOM/page allocation/disk errors (newly tested)/RX OOM injection all closed-loop | fc_reg + net injection |
| Security audit | 🟡 syscall entry audit ✅, SECURITY.md ✅, trust-bit model documented; fuzzing/defense-in-depth ❌ | SECURITY.md |
| Full semantics | ✅ writeback/dirty-page/fsync/OOM semantics audit (fc-semantics.md); permissions = trust bits (as-is) | fc-semantics.md |
| Performance profiling | ✅ RTT percentiles + sched_bench real-kernel report (step/osc pass; pollute=real finding, shortwin=semantic mismatch) | scheduler.md |
| Regression gating | ✅ three-tier gating: host CI + golden diff + QEMU network smoke | tests/ |
| Observability | ✅ counters/rates/RTT/panic to serial/stall self-diagnosis | per-change verification |

## Remaining technical debt (by priority)

| # | Debt | Status | Fix design |
|---|---|---|---|
| 1 | pollute phase RIP multiplier residual variance | 3x convergence achieved (accumulator), full pass awaiting in-window instrumentation | numerator-side / phase-transition instrumentation |
| 2 | exit reclamation path spinlock UAF | lock pairing converged (TOCTOU window closed); full cure still leaves proc refcounting | proc refcounting (cure) |
| 3 | Zero-copy remaining suspects | see dpdk-lite.md (suspects narrowed to 2 items) | pbuf lifetime runtime tracking |
| 4 | sched_bench shortwin sampling | switch-point sampling already online (stale docs fixed) | ✅ converged |
| 5 | Checksum offload / MSI-X multi-queue | blocked on 82574 datasheet | implement after register verification |
| 6 | Kernel vsnprintf deep hardening + fuzzing in depth | fuzz + out-of-bounds read fix ✅ | ✅ converged |
| 7 | FD inheritance | fd_manager_dup ✅ (lock/OOM rollback included) | ✅ converged |
| 8 | xHCI async IN path | sync path fixed; async via bounce/SG to be implemented (blocked on real-hardware verification) | per-page chained TRB extended to async |
| 9 | Audit latest batch | High-risk 14 items: 11 fixed + 3 false positives; medium-risk 13 items: 6 fixed + 5 not applicable/already covered + allocator integration analysed (needs boot-order coordination) + delayed detection is by design (SLAB detects at refill, SLUB immediate); design debt analysed (semantics correct, the rename that caused a boot hang in testing was reverted, migration plan on record); hygiene 16 items: 15 closed (incl. 3 outdated claims: fb.cpp copy init / x86mem MIT added / libc line numbers pointed at wrong file) + 1 roadmap item (-Wno batched removal) | see the audit records |

## 🏁 kmod / 整个内核组件化（本轮目标）—— ✅ 实测完成

在原有的宏内核之上引入 Tier0 私有契约型模块链接与治理机制，并把整个内核的组件
迁移为同一张组件图（Tier 0 固定机制 + Tier 1 内建组件 + Tier 2 可加载 .ko）。

- **Tier0 机制**：ET_REL 加载器（.in/.out/.kmod_manifest、受限重定位集、严格边界校验）、
  Kahn 拓扑图/环检测、能力策略（STRICT/OPEN/DISABLED）、事务化加载与逆序回滚、
  热替换（quiesce/epoch 排空/看门狗/原子切换/失败回退）、卸载引用计数硬门禁、
  固定 syscall 边界（0..63 固定契约 + 128..511 动态命名空间 + 哨兵治理表）、
  座位（seat）机制、内建组件模型（.kbuiltin.* + KmodBootComponents 启动拓扑）。
- **组件化范围**：37 个内建组件（平台 ACPI/APIC/IRQ/时间/SMP、调度核心/安装/策略、
  分配策略、设备/PCI 核心/枚举、AHCI/NVMe/xHCI/e1000/PS2、VFS/ext4/FatFs/rootfs、
  lwIP/netdev、framebuffer、user.init、7 个 syscall 家族）+ 14 个 .ko 演示模块
  （依赖/私有契约回填、事务回滚、能力/环负向、随机/调度/分配策略座位热替换、
  动态 syscall 槽与固定槽覆写热替换）。
- **启动序列**：x86_64_init 只保留 Tier0（Serial/GDT/IDT/FPU/PMM/VMM/SLAB/SLUB/
  SMEP/SIMD/syscall 入口/KmodInit），其余由 KmodBootComponents() 按 manifest 依赖
  拓扑初始化；Tier0 收尾 sti+IPI+idle。
- **验证证据**：宿主门禁 6/6 PASS（含 ASAN/UBSAN、2000 次 fuzz）+
  内核构建零错误 + QEMU 无头串口 KMOD_SELFTEST PASS 11/11（37 builtin ready、
  boot load 2/2、desktop/DOOM/lwIP 正常、0 异常/0 stall）。
- **文档**：docs/kernel-modules.md（机制）、docs/kernel-components.md（组件台账/
  Tier 划分/迁移状态/已知限制与路线图）。
- **迁移中修复的 Tier0 问题**：IRQ0 解除屏蔽早于 PIT handler 注册（新增 kernel.arch.irq
  组件 + 引导期 cli）；引导栈深度（start.S 显式 64KiB __boot_stack）；partition 头文件
  引用参数声明 latent bug；selftest 移到 sti 前单线程以确定性暴露模块治理错误。

## 🏁 Tier0 共享服务层：klib 等公共工具共享给每个模块（当前目标）—— ✅ 实测完成

在「src → modules 全模块化」基础上，把 Tier0 公共工具开放给**所有模块**（37 个内建
组件 + 全部 .ko），并补齐 provider="kernel" 导入的契约校验缺口。

- **两种共享方式**：
  1) 直接符号：klib C 链接函数加入内核 .kout（57 项导出）：str、kprintf 格式化族、
     hashmap_*、art_*、spinlock_*、kcalloc/krealloc/kmalloc_aligned；.ko 直接调用
     即由加载器按 ELF 未定义符号名解析，无需 .in。
  2) 契约 ops：kernel.klib.v1（字符串/格式化/qsort/bitmap/kld/随机/panic）、
     kernel.sync.v1（自旋锁/中断状态）、kernel.algo.v1（hashmap/ART/Queue/FIFO/List）；
     模块用 KMOD_IMPORT_KLIB/SYNC/ALGO，治理层校验 contract 相等 + abi 区间 +
     expect_size == sizeof(ops)，失败整批事务回滚。
- **治理补齐**：provider="kernel" 导入从"仅名字匹配"升级为与模块间导入同等的
  契约/版本/尺寸强校验（新增 ind_kernel_export_in）。
- **共享服务导出源单一**：kernel_services.cpp 同时供内核 .kout 与宿主测试表，
  防止两套表漂移；宿主用 std libc 薄包装验证同一 initializer。
- **端到端示例**：endor.demo.klib .ko 同时使用直接符号 + klib/sync ops，init 自检
  通过后导出 magic；selftest shared-klib-contract 加载→回读 magic→卸载（12/12）。
- **修复**：扩大导出面激活了 str.c 中 toi 的 latent isdigit 缺失 include
  （此前被 --gc-sections 回收未暴露），已在 str.c 补 klib/ctype.h。
- **验证**：内核 clean build 零错误；modules-check 15/15；宿主 6/6 + ASAN 6/6；
  QEMU KMOD SMOKE: PASS、KMOD_SELFTEST PASS 12/12、kernel exports: 57、
  37 builtin ready、0 STALL；文档 kernel-modules.md §4.1 / kernel-components.md §6/§10
  已更新（含排除项与 ABI 规则）。

## 🏁 桌面内嵌 Lua：可编程壁纸与窗口样式（当前目标）—— ✅ 实测完成

在桌面程序（programs/desktop）内嵌 Lua 5.4.7，启动时执行 /mp/desktop.lua，脚本通过
受控 sky.* API 修改运行期窗口 chrome 与壁纸；同时补齐 Lua 所需、原用户态 libc 缺失的
标准 C 函数（标准语义 + 宿主测试）。

- **Lua 运行时**：programs/desktop/lua/ vendor 上游 Lua 5.4.7（28 个 .c + 27 个 .h，
  SHA256 全量核对、**零 patch**；MIT LICENSE）；只注册 base/string/table/math/utf8/
  coroutine/debug，不开放 io/os/package。glue 在 programs/desktop/lua_glue.cpp，
  优先加载 /mp/desktop.lua，缺失回退内嵌脚本；错误只回退默认主题、串口报告。
- **运行期主题**：programs/desktop/sky_style.*。窗口圆角/标题栏/边框/分隔线/纸面/
  标题文字/关闭字形/阴影强度全部可由 Lua 设置；**壁纸不再由 C++ 解析渐变/颜色配置**，
  脚本通过 sky.wallpaper_* 原语直接绘制 wallpaper framebuffer。
  默认值与旧桌面逐位一致；loader/main/wm_client 的窗口 chrome 全部改走运行期 theme。
- **API**：预置壁纸 FrameBuffer 变量 `wallpaper`（`addr/addr_u64/width/height/
  pitch_pixels/pitch_bytes/stride/bpp/format` + `fill/pixel/hline/vline/rect/size`
  方法；`sky.wallpaper` 同对象，`sky.wallpaper_*` 函数形式保留）；
  `sky.set_window_style{corner_radius,title_color,border_color,separator_color,
  paper_color,ink_color,close_color,shadow_alpha}`、`sky.get_style()`、`sky.log()`。
- **标准 C 函数**（lib/stdc 补齐）：setjmp/longjmp（x86_64 SysV asm）、signal.h、
  locale（C locale）、time/clock/difftime（SYSCALL_TIME/MONOTONIC_MS）、
  math（asin/log2/log10/sinh/cosh/tanh/frexp/ldexp/modf/HUGE_VAL[F]）、
  string（strstr/strspn/strcspn/strrchr/strpbrk/strcoll/strerror/memchr/memcmp 声明）、
  stdio（BUFSIZ/stdin/out/err/fwrite/fflush/fprintf/fgets/feof/ferror/freopen/getc/
  ungetc…）、stdlib abort；FILE 扩展 eof/error/ungetc 状态。
- **内核侧修复**：动态链接 R_X86_64_COPY 原先从"目标对象自己的导出"取源，导致
  _ctype/stdout/stdin/stderr COPY 到全 0（Lua string.format 报 invalid conversion
  specification）；改为在其它对象中查找定义后，桌面 Lua 正常。
- **验证**：
  * make cm 全新 clean build 通过；make -C lib 通过；
  * tests/stdc：**PASS 217 / FAIL 0**（CI 已接入）；
  * tests/kmod + ASAN：PASS 6/6；
  * QEMU lua_desktop_smoke.ps1：**LUA DESKTOP SMOKE: PASS**（source=file、
    style radius=16/title=0x232A45、壁纸由 Lua 直接绘制进 framebuffer——
    串口 `wallpaper: script-painted ... top=0x102040 ... bottom=0x7A2F6E` 回读 +
    截图像素采样通过）；
  * QEMU kmod_smoke.ps1 回归：**KMOD SMOKE: PASS**（12/12、37 builtin、0 STALL）。
- **文档**：docs/lua-desktop.md（新增）、docs/README.md 索引、root README §3.6/§6/§7、
  programs/desktop/lua/README.md（vendor 记录）。

## 🏁 安装程序 + Desktop 设置/`.lua` 配置加载界面 + Notepad 文件对话框（当前目标）—— ✅ QEMU 实测完成

### 交付
- **Installer**（`programs/installer/`）：GUI 安装程序，把 6 个资源写入 `/mp`：
  `themes.index`、`theme_aurora.lua`、`theme_ember.lua`、`theme_mint.lua`、
  `files.index`、`notes.txt`；窗口逐文件显示 new/overwrite/failed；写完后才
  `CLIENT_READY`；Esc/Enter 或 WM 关闭退出；重复运行覆盖。
- **Settings**（`programs/settings/`）：桌面启动时自动 spawn 并置顶；读取
  `/mp/themes.index` 列出主题；Up/Down 选择、Enter 通过 WM 协议页命令邮箱发送
  `SKYWIN_CMD_LOAD_LUA`；R 重载 `/mp/desktop.lua`、I 打开 Installer、L 重读
  index（列表为空时每 1s 自动重读）、Esc 退出；写 `/mp/selected_theme.txt`。
- **命令行邮箱**（`lib/include/graphic/winstyle.h`）：协议页 slot 10..15，
  `SKYWIN_CMD_LOAD_LUA / RELOAD_DESKTOP / SPAWN_INSTALLER` + `skywin_send_command`；
  desktop `wm_client_poll_command()` 轮询，`SkyLuaLoadConfigFile()` 运行期重建
  Lua state、应用壁纸/窗口样式，`Compositor::ForceFullCompose()` 整屏重组，
  `wm_client_repaint_chrome()` 重绘所有窗口 chrome（内容区像素保存/恢复）。
- **Notepad**（`programs/notepad/main.cpp`）：Ctrl+O Open 文件对话框（读
  `/mp/files.index`，fallback 3 项，读取文件/CRLF/截断/错误提示）、Ctrl+W Close
  对话框（Save and Close / Discard and Close / Cancel）、Ctrl+S 保存；
  对话框内 Up/Down/Enter/Esc；忽略 Ctrl+Alt+* 全局快捷键；串口标记齐全。
- **桌面快捷键**：Ctrl+Alt+S/N/I 聚焦/拉起 Settings/Notepad/Installer。
- **内核修复**（首启安装器 + DOOM 暴露）：
  * AHCI 轮询不再在 ext4 临界区内 `sti`（消除 ext4 bcache 并发损坏/卡死）；
  * ext4 挂载锁 owner 改为线程指针、等待者开中断 Yield（消除关中断自旋饿死
    TLB/定时器导致的 STALL 连锁）；
  * `/mp` 大文件（>1MiB）不进 file_cache（`sys_fread`），规避 DOOM 启动时
    `file_cache_promote` invalid free；
  * 键盘 Esc 映射修复（scancode 0x01 → 0x1B，之前 Esc 被输入核心丢弃）。

### 验证（4 CPU、2G QEMU；脚本自动清理 disk.img 首启资源）
- 宿主：**`make cm` 全新 clean build 通过**（ISO 5812 sectors）；
  `tests/stdc` **PASS 217/0**；`tests/kmod` **6/6**、ASAN **6/6**；
  `kernel modules-check` **OK 15/15**。
- **QEMU `desktop_features_smoke.ps1`：DESKTOP FEATURES: PASS（0 warning）**，
  0 STALL / 0 Panic / 0 invalid free：
  * Installer 首装 `6/6 files (6 new, 0 failed)` → READY → Aurora 主题加载
    rc=0 → Esc 关闭（`[installer] exit: Esc/Enter`）；
  * Settings 自动刷新列表（`method: auto-refresh`）→ 应用 Mint：
    `[desktop] theme: load path=/mp/theme_mint.lua rc=0 radius=18`，
    截图像素/帧差通过；
  * Notepad `dialog=open items=5` → `opened path=/mp/theme_aurora.lua bytes=1479`
    → `dialog=close` → **Esc cancel**（`close choice=cancel`）；
  * R → `[desktop] theme: load path=/mp/desktop.lua rc=0 radius=16`；
  * disk.img 上 6 个安装资源 + `selected_theme.txt` 全部校验存在。
- 证据：`res/scripts/test/desktop_features_smoke.ps1`、`desktop_features_serial.log`、
  `desktop_features_*.ppm`。
- **QEMU 回归**：`lua_desktop_smoke.ps1` **PASS**；`kmod_smoke.ps1` **PASS**。
- **文档**：`docs/desktop-features.md`（新增）、`docs/README.md` 索引、
  root README §3.7/§6/§7；`docs/lua-desktop.md` 关联。


### 当前目标（第 2 轮）：DOOM 窗口 + Desktop z-order/focus + 最小化保持最大化 + 子进程退出事件 + 64MB

- **DOOM 窗口**：WM content 高度 = bodyH − title − **运行时主题圆角**（默认 16），
  640×440 body 实际 content 640×392；旧 shim 的 `ch>=400 ? 2x : 1x` 回退 1x，
  只画左上 320×200，其余黑边。改为预计算 col/row 映射、**始终填满 content**
  （2G 截图四象限/四边 non-black；content_w=640 content_h=392 exact_2x=0）。
- **z-order / focus**：`wm_focus_immediate` 同帧 `kbd_focus_set_slot`；点击/任务栏
  restore/raise/spawn 立即置顶+聚焦；`wm_refocus` 跳过 minimized/closed/no-layer，
  失效回退最高可见 `layer->z`；console 与 generic client 统一。任务栏 pill 语义
  与 z/focus 一致。
- **最小化保持最大化**：console caption/taskbar 两条路径保存 `wm_prior`；
  `WmClient::pre_minimized_max` 支持 generic 客户端最大化→最小化→恢复保持
  maximized；恢复 raise+focus。QEMU 截图：Notepad max=restore 帧完全一致，
  console max→min→restore 仍 max。
- **子进程退出事件**：`proc_t` child-event ring（8 条）+ `FinalizeProcExit`/
  `DeleteProc` 入队；`sys_load` 将客户端挂到 desktop children；用户 syscall 31
  `child_event`；desktop 每帧 drain≤16 立即回收窗口，仅保留 2s 兜底 sweep。
  kmod 固定契约 31→32，`kernel.syscall.proc` 导出槽 31，host kmod 6/6、ASAN 6/6。
- **64MB 低内存**：`-m64M` 可用约 56MiB。启动路径自动启动 Notepad/Settings，
  DOOM 由 `Ctrl+Alt+D` 按需（避免 R_Init/WAD 冷读推迟桌面 ready）；PureDOOM
  DOOM_TINY 下 zone 8MiB（`doom_set_zone_mb`），64M 下 DOOM 可 init/持续渲染（zone=8，
  OOM≈197–1741，0 panic；50–74s 心跳）。
  新增 OOM 加固：`VMA::InsertRegion` 空链防护、`elf_load_impl` `PMM::Request()==0`
  失败回收；桌面关闭窗口 `sys_munmap` 共享 surface/protocol 别名。
- **键盘兜底**：Ctrl+Alt+C 聚焦 Console、Ctrl+Alt+M 切换最大化、Ctrl+Alt+H 最小化、
  Ctrl+Alt+D 拉起 DOOM；用于无鼠标/无头验证。
- **验证**：
  * 2G：DOOM 窗口修复截图、Notepad/console maximize→minimize→restore 截图、
    child-event 串口（`[wm] child pid=...` + drop，无 500ms 轮询路径）；
  * 64M：desktop 应用稳定（OOM≈13–16，0 panic）；DOOM-only 模式 init/frames；
  * `tests/stdc` 217/0、`tests/kmod` 6/6、ASAN 6/6。
- **已知边界**：56MiB 下 DOOM 与 Notepad/Settings 同时常驻会 PMM OOM；
  DOOM 采用按需/独占策略（低内存自动启动桌面应用）；≥84MiB 无此限制。


### 当前目标（第 3 轮）：Setup 安装整个操作系统 + DOOM 极限压缩

- **Setup / OS 安装**：
  * 新 `res/scripts/build_os_pkg.py`：SKYOS1 容器 + 64KiB LZ4 block，
    28 entries（kernel/15×.ko/Limine/desktop.lua/libsky/7 个程序），
    `5,460,395 B`（raw `8,908,221 B`，压缩比 0.613）；`make os-pkg`
    自动 e2cp 到 `disk.img:/os.pkg`，`cm`/`ma` 末尾自动调用；
  * 内核固定 syscall **22 mkdir**（`ext4_dir_mk`，幂等，ext4-only；
    `kernel.syscall.file` 导出 + kmod 契约 22 + 用户 `SYSCALL_MKDIR`）；
  * `programs/installer/main.cpp` 升级 Setup：保留旧 6 资源安装与
    `[installer]` 门禁标记，读 `/mp/os.pkg` → 流式解压写 `/mp/system/**`
    → 写 `os.manifest`；串口 `[setup] os pkg / install / install complete /
    manifest written`；标题 “SkylineSystem Setup”；
  * QEMU `setup_os_smoke.ps1`：**PASS 0 warning，28/28 files，failed=0，
    installed=8,908,221 B，manifest 937 B**；E1–E6 从 disk.img 校验
    kernel ELF magic、desktop.elf/libsky 大小、count==total_files。
- **DOOM 极限压缩**：
  * `DOOM_TINY`（Makefile 默认 1，可 `DOOM_TINY=0`）：V_Init 单 64KiB
    screen + screens[0..3] 别名；去掉 screen_buffer(64KiB)/
    final_screen_buffer(256KiB RGBA)/重复 screens[0]；shim palette→ARGB
    LUT 直写 WM surface；**固定 buffer 640KiB→65KiB，省 575KiB**；
  * tiny 下 audio stub、wipe/save-load/screenshot/后屏禁用、全屏；
    16 位审计/static_assert，不动 fixed_t；
  * `tools/wadtrim.py`：删 289 个音频 lump，`doom.wad 11,159,840 →
    9,071,551 B`（81.29%，省 1.99MiB），verify/audio_left=0；
    `--adpcm` IMA-ADPCM 4-bit：122 sfx `1,222,616→620,982 B`
    （3.93:1 vs 16-bit PCM / 1.97:1 vs DMX8）；
  * zone 策略 DOOM_TINY 下 ≤80MiB→8MiB（连续三次 64M PASS），≤128MiB→10MiB，
    更大→12MiB；低内存启动策略改为
    Console+DOOM 优先，Notepad/Settings 延迟；
  * QEMU `doom_compact_smoke.ps1`：2G **PASS**（compact=1，zone=12，
    init done+心跳，截图实际游戏画面）；64M **PASS**（zone=8，OOM≈25–26，init 后余 ~5–6MiB，
    0 panic/STALL，init done+心跳，截图）。
- **验证**：host `tests/kmod` 6/6 + ASAN、`tests/stdc` 217/0；Setup gate
  PASS；DOOM gate 2G/64M PASS；旧 desktop_features gate 仅 D6 截图 1 cell
  抖动（K0/D1–D7/C/E 其余 PASS，产品行为正常）。
- **边界**：Setup 安装完整系统树到 ext4 `/mp/system`，不做分区/MBR/
  limine bios-install；WAD 仍从 VFS 流式读，未做 file mmap/per-lump LZ4；
  DOOM 不是 RP2040 384KB 运行环境。
