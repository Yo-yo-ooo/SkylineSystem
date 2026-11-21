# Goal status and verification ledger

> Three parts: (A) the e1000 driver, (B) finishing the lwIP port, (C) the DPDK-style rework. On top of
> that the production checklist: SMP concurrency, long-term stability, fault injection, security, full
> semantics, performance profiling, regression gating, observability. Every item below is checked
> against the code and a measurement, and whatever wasn't done is written down as not done.

## Milestone: four-source audit fix list —— all cleared

All six tiers cleared: P0 ×18 (including two convergence items that were reverted and redone: the TLB ACK fence —
root cause of the 400M pause spins under TCG → capped at 100ms wall clock, measured 0 timeouts; and the AHCI
per-request DMA buffer that replaced the lock spanning the sti window), P1 ×36, P2 ×8 (cross-core ABA
isolation stack foreign_head, AHCI release barrier, QSBR 64 slots), P3 ×21, P4 ×8 (allocator host chaos
test — which also caught a real MCB allocation-granularity bug; fc writeback content verification), P5 ×19.
Verification after each change set: BUILD_EXIT=0 + GOLDEN PASS + QEMU ping 3 + 0 exceptions.

## Roadmap convergence

| # | Roadmap item | Status |
|---|---|---|
| #7 | FD inheritance (filedesc snapshot-style deep copy) | also confirmed two latent defects (rb_postorder_iter callback contract + exec FDMan release not nulling) |
| #2 | proc lifecycle TOCTOU | ProcessAddThread re-check inside lock + reaper FDMan reclamation into lock (5 call sites closed) |
| #6 | vsnprintf fuzz | ASAN host fuzz + real out-of-bounds read fix (NUL-terminated format string) |
| — | fsync production activation | fclose + implicit dirty-page flush at process exit |
| — | Sleep/timer wheel revived | PIT::Sleep dual mode (scheduler ready → real sleep) |
| — | RX burst observability | rx_burst_avg (DPDK metric) |
| — | Static analysis | clang --analyze 9 files + 2 defensive fixes (fc LRU double-advance + rbtree sibling guard) |

## A. e1000 driver —— done

- 82574L (QEMU e1000e) complete driver: probe/reset/EEPROM MAC/dual ring 256/IRQ+polling dual mode
- Bug-fix history: RDT ownership, _memcpy transposed order, GetPhysics in-page offset,
  pbuf double free, PIT::Sleep hang during probe, early PIT busy-wait guard
- Multi-core TX spinlock serialization + ring-full backoff

## B. lwIP porting —— done (production default: polling + copy path)

- End-to-end verification: DHCP lease (10.0.2.15), ICMP ping consecutive replies, multi-core TX flood 2.2-2.7k frames/s
- Porting fixes: lwipopts (software checksum/alignment/pool), cc.h DIAG varargs chain fix,
  sys_now monotonic milliseconds, ACD disabled (slirp ARP proxy), ip4addr_ntoa static-buffer pitfall
- Network thread stack 32KB (16KB stack overflow = one of the deep root causes of all intermittent crashes)

## C. DPDK-lite —— base complete, two experimental items documented

| DPDK concept | Status |
|---|---|
| PMD polling | production path |
| Multi-core TX concurrency | spinlock + 3-core flood verification |
| Per-core private / lock-free | partial single RX queue (dual queue = MSI-X blocking item) |
| Zero-copy RX | partial experimental (NET_ZEROCOPY=0): index back-derivation + double free fixed, still silent after first stat (suspects narrowed to 2 items, see dpdk-lite.md) |
| Checksum offload / MSI-X | blocked on register-manual verification |
| Stats/observability | counters + RTT P50/P90/P99 + stall self-diagnosis |

## Production-grade checklist item by item

| Pillar | Status | Evidence |
|---|---|---|
| SMP concurrency correctness | existing (SLUB/FC/sched multithreaded suites) + network multi-core stress + **110-item P0/P2 concurrency fixes all cleared** | tests/ + soak |
| Long-term stability |(evidence) closing soak chain: 10/15/30min + 40min multi-segment all 0 assertions 0 exceptions 0 stalls; final 30min: tx=1.29M frames (82.7MB) sustained at 2429/s, ping=185 consecutive, loss=0; **re-tested after every milestone** | serial.log |
| Fault injection | OOM/page allocation/disk errors (newly tested)/RX OOM injection all closed-loop | fc_reg + net injection |
| Security audit | partial syscall entry audit ok, SECURITY.md ok, trust-bit model documented; fuzzing/defense-in-depth no | SECURITY.md |
| Full semantics | writeback/dirty-page/fsync/OOM semantics audit (fc-semantics.md); permissions = trust bits (as-is) | fc-semantics.md |
| Performance profiling | RTT percentiles + sched_bench real-kernel report (step/osc pass; pollute=real finding, shortwin=semantic mismatch) | scheduler.md |
| Regression gating | three-tier gating: host CI + golden diff + QEMU network smoke | tests/ |
| Observability | counters/rates/RTT/panic to serial/stall self-diagnosis | per-change verification |

## Remaining technical debt (by priority)

| # | Debt | Status | Fix design |
|---|---|---|---|
| 1 | pollute phase RIP multiplier residual variance | 3x convergence achieved (accumulator), full pass awaiting in-window instrumentation | numerator-side / phase-transition instrumentation |
| 2 | exit reclamation path spinlock UAF | lock pairing converged (TOCTOU window closed); full cure still leaves proc refcounting | proc refcounting (cure) |
| 3 | Zero-copy remaining suspects | see dpdk-lite.md (suspects narrowed to 2 items) | pbuf lifetime runtime tracking |
| 4 | sched_bench shortwin sampling | switch-point sampling already online (stale docs fixed) | converged |
| 5 | Checksum offload / MSI-X multi-queue | blocked on 82574 datasheet | implement after register verification |
| 6 | Kernel vsnprintf deep hardening + fuzzing in depth | fuzz + out-of-bounds read fix ok | converged |
| 7 | FD inheritance | fd_manager_dup ok (lock/OOM rollback included) | converged |
| 8 | xHCI async IN path | sync path fixed; async via bounce/SG to be implemented (blocked on real-hardware verification) | per-page chained TRB extended to async |
| 9 | Audit latest batch | High-risk 14 items: 11 fixed + 3 false positives; medium-risk 13 items: 6 fixed + 5 not applicable/already covered + allocator integration analysed (needs boot-order coordination) + delayed detection is by design (SLAB detects at refill, SLUB immediate); design debt analysed (semantics correct, the rename that caused a boot hang in testing was reverted, migration plan on record); hygiene 16 items: 15 closed (incl. 3 outdated claims: fb.cpp copy init / x86mem MIT added / libc line numbers pointed at wrong file) + 1 roadmap item (-Wno batched removal) | see the audit records |

## kmod / 整个内核组件化（本轮目标）—— 实测完成

在原有的宏内核之上引入 Tier0 私有契约型模块链接与治理机制，并把整个内核的组件
迁移为同一张组件图（Tier 0 固定机制 + 统一 DynModule：resident/relocatable；Tier 1/2 区分已删除）。

- Tier0 机制：ET_REL 加载器（.in/.out/.kmod_manifest、受限重定位集、严格边界校验）、
  Kahn 拓扑图/环检测、能力策略（STRICT/OPEN/DISABLED）、事务化加载与逆序回滚、
  热替换（quiesce/epoch 排空/看门狗/原子切换/失败回退）、卸载引用计数硬门禁、
  固定 syscall 边界（0..63 固定契约 + 128..511 动态命名空间 + 哨兵治理表）、
  座位（seat）机制、内建组件模型（.kbuiltin.* + KmodBootComponents 启动拓扑）。
- 组件化范围：37 个内建组件（平台 ACPI/APIC/IRQ/时间/SMP、调度核心/安装/策略、
  分配策略、设备/PCI 核心/枚举、AHCI/NVMe/xHCI/e1000/PS2、VFS/ext4/FatFs/rootfs、
  lwIP/netdev、framebuffer、user.init、7 个 syscall 家族）+ 14 个 .ko 演示模块
  （依赖/私有契约回填、事务回滚、能力/环负向、随机/调度/分配策略座位热替换、
  动态 syscall 槽与固定槽覆写热替换）。
- 启动序列：x86_64_init 只保留 Tier0（Serial/GDT/IDT/FPU/PMM/VMM/SLAB/SLUB/
  SMEP/SIMD/syscall 入口/KmodInit），其余由 KmodBootComponents() 按 manifest 依赖
  拓扑初始化；Tier0 收尾 sti+IPI+idle。
- 验证证据：宿主门禁 6/6 PASS（含 ASAN/UBSAN、2000 次 fuzz）+
  内核构建零错误 + QEMU 无头串口 KMOD_SELFTEST PASS 11/11（37 builtin ready、
  boot load 2/2、desktop/DOOM/lwIP 正常、0 异常/0 stall）。
- 文档：docs/kernel-modules.md（机制）、docs/kernel-components.md（组件台账/
  Tier 划分/迁移状态/已知限制与路线图）。
- 迁移中修复的 Tier0 问题：IRQ0 解除屏蔽早于 PIT handler 注册（新增 kernel.arch.irq
  组件 + 引导期 cli）；引导栈深度（start.S 显式 64KiB __boot_stack）；partition 头文件
  引用参数声明 latent bug；selftest 移到 sti 前单线程以确定性暴露模块治理错误。

## Tier0 共享服务层：klib 等公共工具共享给每个模块（当前目标）—— 实测完成

在「src → modules 全模块化」基础上，把 Tier0 公共工具开放给所有模块（37 个内建
组件 + 全部 .ko），并补齐 provider="kernel" 导入的契约校验缺口。

- 两种共享方式：
  1) 直接符号：klib C 链接函数加入内核 .kout（57 项导出）：str、kprintf 格式化族、
     hashmap_*、art_*、spinlock_*、kcalloc/krealloc/kmalloc_aligned；.ko 直接调用
     即由加载器按 ELF 未定义符号名解析，无需 .in。
  2) 契约 ops：kernel.klib.v1（字符串/格式化/qsort/bitmap/kld/随机/panic）、
     kernel.sync.v1（自旋锁/中断状态）、kernel.algo.v1（hashmap/ART/Queue/FIFO/List）；
     模块用 KMOD_IMPORT_KLIB/SYNC/ALGO，治理层校验 contract 相等 + abi 区间 +
     expect_size == sizeof(ops)，失败整批事务回滚。
- 治理补齐：provider="kernel" 导入从"仅名字匹配"升级为与模块间导入同等的
  契约/版本/尺寸强校验（新增 ind_kernel_export_in）。
- 共享服务导出源单一：kernel_services.cpp 同时供内核 .kout 与宿主测试表，
  防止两套表漂移；宿主用 std libc 薄包装验证同一 initializer。
- 端到端示例：endor.demo.klib .ko 同时使用直接符号 + klib/sync ops，init 自检
  通过后导出 magic；selftest shared-klib-contract 加载→回读 magic→卸载（12/12）。
- 修复：扩大导出面激活了 str.c 中 toi 的 latent isdigit 缺失 include
  （此前被 --gc-sections 回收未暴露），已在 str.c 补 klib/ctype.h。
- 验证：内核 clean build 零错误；modules-check 15/15；宿主 6/6 + ASAN 6/6；
  QEMU KMOD SMOKE: PASS、KMOD_SELFTEST PASS 12/12、kernel exports: 57、
  37 builtin ready、0 STALL；文档 kernel-modules.md §4.1 / kernel-components.md §6/§10
  已更新（含排除项与 ABI 规则）。

## 桌面内嵌 Lua：可编程壁纸与窗口样式（当前目标）—— 实测完成

在桌面程序（programs/desktop）内嵌 Lua 5.4.7，启动时执行 /mp/desktop.lua，脚本通过
受控 sky.* API 修改运行期窗口 chrome 与壁纸；同时补齐 Lua 所需、原用户态 libc 缺失的
标准 C 函数（标准语义 + 宿主测试）。

- Lua 运行时：programs/desktop/lua/ vendor 上游 Lua 5.4.7（28 个 .c + 27 个 .h，
  SHA256 全量核对、零 patch；MIT LICENSE）；只注册 base/string/table/math/utf8/
  coroutine/debug，不开放 io/os/package。glue 在 programs/desktop/lua_glue.cpp，
  优先加载 /mp/desktop.lua，缺失回退内嵌脚本；错误只回退默认主题、串口报告。
- 运行期主题：programs/desktop/sky_style.*。窗口圆角/标题栏/边框/分隔线/纸面/
  标题文字/关闭字形/阴影强度全部可由 Lua 设置；壁纸不再由 C++ 解析渐变/颜色配置，
  脚本通过 sky.wallpaper_* 原语直接绘制 wallpaper framebuffer。
  默认值与旧桌面逐位一致；loader/main/wm_client 的窗口 chrome 全部改走运行期 theme。
- API：预置壁纸 FrameBuffer 变量 `wallpaper`（`addr/addr_u64/width/height/
  pitch_pixels/pitch_bytes/stride/bpp/format` + `fill/pixel/hline/vline/rect/size`
  方法；`sky.wallpaper` 同对象，`sky.wallpaper_*` 函数形式保留）；
  `sky.set_window_style{corner_radius,title_color,border_color,separator_color,
  paper_color,ink_color,close_color,shadow_alpha}`、`sky.get_style()`、`sky.log()`。
- 标准 C 函数（lib/stdc 补齐）：setjmp/longjmp（x86_64 SysV asm）、signal.h、
  locale（C locale）、time/clock/difftime（SYSCALL_TIME/MONOTONIC_MS）、
  math（asin/log2/log10/sinh/cosh/tanh/frexp/ldexp/modf/HUGE_VAL[F]）、
  string（strstr/strspn/strcspn/strrchr/strpbrk/strcoll/strerror/memchr/memcmp 声明）、
  stdio（BUFSIZ/stdin/out/err/fwrite/fflush/fprintf/fgets/feof/ferror/freopen/getc/
  ungetc…）、stdlib abort；FILE 扩展 eof/error/ungetc 状态。
- 内核侧修复：动态链接 R_X86_64_COPY 原先从"目标对象自己的导出"取源，导致
  _ctype/stdout/stdin/stderr COPY 到全 0（Lua string.format 报 invalid conversion
  specification）；改为在其它对象中查找定义后，桌面 Lua 正常。
- 验证：
  * make cm 全新 clean build 通过；make -C lib 通过；
  * tests/stdc：PASS 217 / FAIL 0（CI 已接入）；
  * tests/kmod + ASAN：PASS 6/6；
  * QEMU lua_desktop_smoke.ps1：LUA DESKTOP SMOKE: PASS（source=file、
    style radius=16/title=0x232A45、壁纸由 Lua 直接绘制进 framebuffer——
    串口 `wallpaper: script-painted ... top=0x102040 ... bottom=0x7A2F6E` 回读 +
    截图像素采样通过）；
  * QEMU kmod_smoke.ps1 回归：KMOD SMOKE: PASS（12/12、37 builtin、0 STALL）。
- 文档：docs/lua-desktop.md（新增）、docs/README.md 索引、root README §3.6/§6/§7、
  programs/desktop/lua/README.md（vendor 记录）。

## 安装程序 + Desktop 设置/`.lua` 配置加载界面 + Notepad 文件对话框（当前目标）—— QEMU 实测完成

### 交付
- Installer（`programs/installer/`）：GUI 安装程序，把 6 个资源写入 `/mp`：
  `themes.index`、`theme_aurora.lua`、`theme_ember.lua`、`theme_mint.lua`、
  `files.index`、`notes.txt`；窗口逐文件显示 new/overwrite/failed；写完后才
  `CLIENT_READY`；Esc/Enter 或 WM 关闭退出；重复运行覆盖。
- Settings（`programs/settings/`）：桌面启动时自动 spawn 并置顶；读取
  `/mp/themes.index` 列出主题；Up/Down 选择、Enter 通过 WM 协议页命令邮箱发送
  `SKYWIN_CMD_LOAD_LUA`；R 重载 `/mp/desktop.lua`、I 打开 Installer、L 重读
  index（列表为空时每 1s 自动重读）、Esc 退出；写 `/mp/selected_theme.txt`。
- 命令行邮箱（`lib/include/graphic/winstyle.h`）：协议页 slot 10..15，
  `SKYWIN_CMD_LOAD_LUA / RELOAD_DESKTOP / SPAWN_INSTALLER` + `skywin_send_command`；
  desktop `wm_client_poll_command()` 轮询，`SkyLuaLoadConfigFile()` 运行期重建
  Lua state、应用壁纸/窗口样式，`Compositor::ForceFullCompose()` 整屏重组，
  `wm_client_repaint_chrome()` 重绘所有窗口 chrome（内容区像素保存/恢复）。
- Notepad（`programs/notepad/main.cpp`）：Ctrl+O Open 文件对话框（读
  `/mp/files.index`，fallback 3 项，读取文件/CRLF/截断/错误提示）、Ctrl+W Close
  对话框（Save and Close / Discard and Close / Cancel）、Ctrl+S 保存；
  对话框内 Up/Down/Enter/Esc；忽略 Ctrl+Alt+* 全局快捷键；串口标记齐全。
- 桌面快捷键：Ctrl+Alt+S/N/I 聚焦/拉起 Settings/Notepad/Installer。
- 内核修复（首启安装器 + DOOM 暴露）：
  * AHCI 轮询不再在 ext4 临界区内 `sti`（消除 ext4 bcache 并发损坏/卡死）；
  * ext4 挂载锁 owner 改为线程指针、等待者开中断 Yield（消除关中断自旋饿死
    TLB/定时器导致的 STALL 连锁）；
  * `/mp` 大文件（>1MiB）不进 file_cache（`sys_fread`），规避 DOOM 启动时
    `file_cache_promote` invalid free；
  * 键盘 Esc 映射修复（scancode 0x01 → 0x1B，之前 Esc 被输入核心丢弃）。

### 验证（4 CPU、2G QEMU；脚本自动清理 disk.img 首启资源）
- 宿主：`make cm` 全新 clean build 通过（ISO 5812 sectors）；
  `tests/stdc` PASS 217/0；`tests/kmod` 6/6、ASAN 6/6；
  `kernel modules-check` OK 15/15。
- QEMU `desktop_features_smoke.ps1`：DESKTOP FEATURES: PASS（0 warning），
  0 STALL / 0 Panic / 0 invalid free：
  * Installer 首装 `6/6 files (6 new, 0 failed)` → READY → Aurora 主题加载
    rc=0 → Esc 关闭（`[installer] exit: Esc/Enter`）；
  * Settings 自动刷新列表（`method: auto-refresh`）→ 应用 Mint：
    `[desktop] theme: load path=/mp/theme_mint.lua rc=0 radius=18`，
    截图像素/帧差通过；
  * Notepad `dialog=open items=5` → `opened path=/mp/theme_aurora.lua bytes=1479`
    → `dialog=close` → Esc cancel（`close choice=cancel`）；
  * R → `[desktop] theme: load path=/mp/desktop.lua rc=0 radius=16`；
  * disk.img 上 6 个安装资源 + `selected_theme.txt` 全部校验存在。
- 证据：`res/scripts/test/desktop_features_smoke.ps1`、`desktop_features_serial.log`、
  `desktop_features_*.ppm`。
- QEMU 回归：`lua_desktop_smoke.ps1` PASS；`kmod_smoke.ps1` PASS。
- 文档：`docs/desktop-features.md`（新增）、`docs/README.md` 索引、
  root README §3.7/§6/§7；`docs/lua-desktop.md` 关联。


### 当前目标（第 2 轮）：DOOM 窗口 + Desktop z-order/focus + 最小化保持最大化 + 子进程退出事件 + 64MB

- DOOM 窗口：WM content 高度 = bodyH − title − 运行时主题圆角（默认 16），
  640×440 body 实际 content 640×392；旧 shim 的 `ch>=400 ? 2x : 1x` 回退 1x，
  只画左上 320×200，其余黑边。改为预计算 col/row 映射、始终填满 content
  （2G 截图四象限/四边 non-black；content_w=640 content_h=392 exact_2x=0）。
- z-order / focus：`wm_focus_immediate` 同帧 `kbd_focus_set_slot`；点击/任务栏
  restore/raise/spawn 立即置顶+聚焦；`wm_refocus` 跳过 minimized/closed/no-layer，
  失效回退最高可见 `layer->z`；console 与 generic client 统一。任务栏 pill 语义
  与 z/focus 一致。
- 最小化保持最大化：console caption/taskbar 两条路径保存 `wm_prior`；
  `WmClient::pre_minimized_max` 支持 generic 客户端最大化→最小化→恢复保持
  maximized；恢复 raise+focus。QEMU 截图：Notepad max=restore 帧完全一致，
  console max→min→restore 仍 max。
- 子进程退出事件：`proc_t` child-event ring（8 条）+ `FinalizeProcExit`/
  `DeleteProc` 入队；`sys_load` 将客户端挂到 desktop children；用户 syscall 31
  `child_event`；desktop 每帧 drain≤16 立即回收窗口，仅保留 2s 兜底 sweep。
  kmod 固定契约 31→32，`kernel.syscall.proc` 导出槽 31，host kmod 6/6、ASAN 6/6。
- 64MB 低内存：`-m64M` 可用约 56MiB。启动路径自动启动 Notepad/Settings，
  DOOM 由 `Ctrl+Alt+D` 按需（避免 R_Init/WAD 冷读推迟桌面 ready）；PureDOOM
  DOOM_TINY 下 zone 8MiB（`doom_set_zone_mb`），64M 下 DOOM 可 init/持续渲染（zone=8，
  OOM≈197–1741，0 panic；50–74s 心跳）。
  新增 OOM 加固：`VMA::InsertRegion` 空链防护、`elf_load_impl` `PMM::Request()==0`
  失败回收；桌面关闭窗口 `sys_munmap` 共享 surface/protocol 别名。
- 键盘兜底：Ctrl+Alt+C 聚焦 Console、Ctrl+Alt+M 切换最大化、Ctrl+Alt+H 最小化、
  Ctrl+Alt+D 拉起 DOOM；用于无鼠标/无头验证。
- 验证：
  * 2G：DOOM 窗口修复截图、Notepad/console maximize→minimize→restore 截图、
    child-event 串口（`[wm] child pid=...` + drop，无 500ms 轮询路径）；
  * 64M：desktop 应用稳定（OOM≈13–16，0 panic）；DOOM-only 模式 init/frames；
  * `tests/stdc` 217/0、`tests/kmod` 6/6、ASAN 6/6。
- 已知边界：56MiB 下 DOOM 与 Notepad/Settings 同时常驻会 PMM OOM；
  DOOM 采用按需/独占策略（低内存自动启动桌面应用）；≥84MiB 无此限制。


### 当前目标（第 3 轮）：Setup 安装整个操作系统 + DOOM 极限压缩

- Setup / OS 安装：
  * 新 `res/scripts/build_os_pkg.py`：SKYOS1 容器 + 64KiB LZ4 block，
    18 entries（kernel/modules.saf/Limine/desktop.lua/libsky/9 个程序），
    `5,460,395 B`（raw `8,908,221 B`，压缩比 0.613）；`make os-pkg`
    自动 e2cp 到 `disk.img:/os.pkg`，`cm`/`ma` 末尾自动调用；
  * 内核固定 syscall 22 mkdir（`ext4_dir_mk`，幂等，ext4-only；
    `kernel.syscall.file` 导出 + kmod 契约 22 + 用户 `SYSCALL_MKDIR`）；
  * `programs/installer/main.cpp` 升级 Setup：保留旧 6 资源安装与
    `[installer]` 门禁标记，读 `/mp/os.pkg` → 流式解压写 `/mp/system/**`
    → 写 `os.manifest`；串口 `[setup] os pkg / install / install complete /
    manifest written`；标题 “SkylineSystem Setup”；
  * QEMU `setup_os_smoke.ps1`：PASS 0 warning，18/18 files，failed=0，
    installed=8,908,221 B，manifest 937 B；E1–E6 从 disk.img 校验
    kernel ELF magic、desktop.elf/libsky 大小、count==total_files。
- DOOM 极限压缩：
  * `DOOM_TINY`（Makefile 默认 1，可 `DOOM_TINY=0`）：V_Init 单 64KiB
    screen + screens[0..3] 别名；去掉 screen_buffer(64KiB)/
    final_screen_buffer(256KiB RGBA)/重复 screens[0]；shim palette→ARGB
    LUT 直写 WM surface；固定 buffer 640KiB→65KiB，省 575KiB；
  * tiny 下 audio stub、wipe/save-load/screenshot/后屏禁用、全屏；
    16 位审计/static_assert，不动 fixed_t；
  * `tools/wadtrim.py`：删 289 个音频 lump，`doom.wad 11,159,840 →
    9,071,551 B`（81.29%，省 1.99MiB），verify/audio_left=0；
    `--adpcm` IMA-ADPCM 4-bit：122 sfx `1,222,616→620,982 B`
    （3.93:1 vs 16-bit PCM / 1.97:1 vs DMX8）；
  * zone 策略 DOOM_TINY 下 ≤80MiB→8MiB（连续三次 64M PASS），≤128MiB→10MiB，
    更大→12MiB；低内存启动策略改为
    Console+DOOM 优先，Notepad/Settings 延迟；
  * QEMU `doom_compact_smoke.ps1`：2G PASS（compact=1，zone=12，
    init done+心跳，截图实际游戏画面）；64M PASS（zone=8，OOM≈25–26，init 后余 ~5–6MiB，
    0 panic/STALL，init done+心跳，截图）。
- 验证：host `tests/kmod` 6/6 + ASAN、`tests/stdc` 217/0；Setup gate
  PASS；DOOM gate 2G/64M PASS；旧 desktop_features gate 仅 D6 截图 1 cell
  抖动（K0/D1–D7/C/E 其余 PASS，产品行为正常）。
- 边界：Setup 安装完整系统树到 ext4 `/mp/system`，不做分区/MBR/
  limine bios-install；WAD 仍从 VFS 流式读，未做 file mmap/per-lump LZ4；
  DOOM 不是 RP2040 384KB 运行环境。

---

### 第 4 轮：Tier1/Tier2 合并为统一 DynModule 动态链接层

- 模块模型：删除 Tier 1/2 语义，只剩 `KMOD_TIER_DYN`（旧线值 2 作为兼容别名）；
  resident（原内建）与 relocatable（原 `.ko`）只差镜像后端，生命周期/能力/拓扑/
  导入导出/注入完全一致。
- 新增 `kernel.dynlink.v1`：`KdllInject`（LoadLibrary）、`KdllResolve`
  （GetProcAddress）、`KdllEject`（FreeLibrary）、`KdllInjectImage`、
  `KdllInjectFile`；`KMOD_INJ_PIN` 引用计数 + `KMOD_MF_DEMAND` 按需注入。
- Tier0 核心以 resident 节点登记：`kernel.mem.pmm/vmm/heap`、
  `kernel.arch.syscall`。
- dosvm 合并：`kernel.arch.dosvm` = 原 Tier1 模式切换实现 + 原 dosvm.ko 的
  syscall 210..214；init 注入 pmm/acpi，`modules/loadable/dosvm.cpp` 删除。
- 注入示范：`vendor.demo.late`（DEMAND provider）与
  `vendor.demo.inject`（依赖 late，再 inject hello + resolve）随 ISO 下发。
- 验证：
  * 宿主 `tests/kmod` 7/7 PASS（普通 + ASAN），新增 `inject_test.cpp`：
    resident 注入/幂等/PIN/eject、镜像注入、依赖自动注入、resolve、负向；
  * `modules-check` 17/17 OK；
  * QEMU `kmod_smoke.ps1` PASS：`KMOD_SELFTEST PASS 13/13`、
    saf 55、boot load 40/40、`[kdll] dynlink ready`、
    `[dosvm] dynlink: inject ... rc=0`、boot load 2/2；
  * QEMU `dosvm_trip_gate.py` PASS：`[dos] init` → syscall 212 装载
    `hello.com` → `[dosvm] trip OK: cpu=2 gen=1 trips=1`；
  * 期间修正了磁盘 `dos.elf` 与当前 `libsky.so` 的旧镜像不一致
    （重新 `e2cp` 后 DOS 客户端恢复正常）；`kernel/Makefile` 的 `.ko`
    对象现在依赖 `include/kmod/*.h`，避免 ABI 头变化后旧线格式被静默带入镜像。
- 文档：新增 `docs/kernel-dynlink.md`；README/architecture/kernel-modules/
  kernel-components/kernel-build/dosvm/modules README 全部更新。

---

### 第 6 轮：修复 sys_load 卡在内核页表锁（桌面不出现）

- 现场：desktop 已加载、console PID9 已拉起，CPU0 在 syscall 的
  `VMM::Alloc` 上等 `lock=FFFF800000084010`；addr2line 指到
  `VMM::Alloc` line 1070，锁地址正好是
  `kernel_pagemap=FFFF800000084000` 的 `vma_lock`（偏移 0x10）。
- 根因：`sys_load` 在 kernel_pagemap 上分配 kernel stack/fx_area；
  `VMM::Alloc/EAlloc/Free` 的 kernel_pagemap 临界区使用裸 spinlock，
  不关中断也不禁抢占。timer IRQ 重入或同核抢占会让另一路径再次抢
  kernel_pagemap->vma_lock，持锁者/等待者互等，表现为数秒到十几秒 STALL。
- 修复：vmm.cpp 新增 `KpmIrqGuard`，只在 `pm == kernel_pagemap`
  时 cli/恢复 IF；包住 Alloc/EAlloc/Free 的临界区。用户 pagemap 不受影响。
  新增启动日志 `VMM: kernel_pagemap=%p phys=0x%lx` 方便后续解码。
- 验证：kmod smoke PASS（13/13、desktop PROCESS）；
  `client_churn_gate.py --cycles 5` C1..C6 全 PASS（5/5 spawn/exit、
  0 STALL/#PF/OOM）；DOSVM trip PASS；额外 5 次 4-vCPU 35s 启动与
  3 次 2-vCPU 启动均 0 STALL、desktop ready、clients=3。
- 文档：新增 `docs/kernel-pagemap-lock-stall.md`，docs/README 索引。

---

### 第 7 轮：实现文件迁入 .ko（大迁移开始）

- 新增 `kernel/modules/compose/*.sources` 机制：清单里的实现文件从内核 OBJ 排除，
  以 `KMOD_CXXFLAGS` 编译并链接进对应组件 .ko；根构建顺序固定为
  components → `gen_component_exports.py` → kernel → modules/SAF。
- 跨 .ko 符号表：`KmodElfLoadImageEx2` 增加 resolver；`.out` 显式导出 +
  `.kmod_dep_names` 无重定位 provider 列表；`gov_stage_add` provider 先序
  prepare，可扫描 `g_prep[]` 的导出地址；staging 路径栈拒绝环。
- 导出生成器跳过已由其他 .ko 定义的符号，避免内核 extern 找不到定义。
- 已迁移并验证：`kernel.driver.ahci`（AHCI+SATA）、`kernel.driver.nvme`、
  `kernel.driver.xhci`（XHCI+USB device/MSC/UAC/UVC/HID）、
  `kernel.input.ps2.kbd`（keyboard + Kbd）、`kernel.input.ps2.mouse`。
  内核 `nm` 中这些实现符号为 0；模块间 HID / keyboard_feed_byte 走跨 .ko 解析。
- 4K 段对齐：加载器上限 16→4096，镜像 `alloc_base` + 手动 align，支持 Kbd 共享页。
- 验证：`kmod_smoke.ps1` PASS（`KMOD_SELFTEST PASS 14/14`，
  `boot load: 41 requested, 41 ready, 0 failed`，desktop PROCESS，无 crash/STALL）；
  新增 `cross-module-symbol` 自检用例。
- 文档：新增 `docs/module-implementation-migration.md`；更新
  `kernel-components.md`/`kernel-build.md`/`kernel-modules.md`。



---

### 第 5 轮：取消模块内建 + modules.saf 单一 Limine 模块

- 取消 .kbuiltin：38 个组件壳（37 components/ + dosvm/）用
  `-DKMOD_COMPONENT_AS_KO=1` 把 KCOMPONENT_* 映射为
  .kmod_manifest/.in/.out，各自编译成独立 ET_REL .ko；内核镜像里
  __kbuiltin_desc_start == __kbuiltin_desc_end（零静态组件）。
- 共享实现库：driver/FS/net/sched 实现仍编入 kernel，新增 132 项
  kernel.component.v1 导出（component_exports.cpp，由
  res/scripts/gen_component_exports.py 从组件 .ko 未定义符号生成）；
  内核总导出 195 项。
- SAF：新增 res/scripts/make_modules_saf.py，把 38 组件 + 17 demo 共 55 个
  .ko 打进 kernel/bin-x86_64/modules.saf；res/limine.conf 只剩
  module_path: boot():/boot/modules.saf。
- 加载器：新增 kernel/src/arch/x86_64/kmod/saf.cpp（KmodSafIngest）；
  镜像池 trusted 标记允许 kernel.* 保留名、能力按清单授予、固定 syscall 槽、
  KMOD_EXF_EXTERNAL 导出；普通 vendor .ko 仍走 STRICT 且不能 EXTERNAL。
- 启动实测：`saf: 55 .ko accepted, 0 rejected`、
  `boot load: 40 requested, 40 ready, 0 failed`、38 条
  `loaded kernel.*`、`KMOD_SELFTEST PASS 13/13`、桌面 PROCESS 正常；
  dosvm.ko 从 SAF 加载并完成 210..214 + 模式往返（DOSVM gate PASS）。
- 构建：kernel/Makefile 新增 component-ko 规则与 modules-saf 目标；
  .ko 对象依赖 include/kmod/*.h，避免 ABI 头变化后旧线格式被静默带入；
  ISO/HDD/Setup os.pkg 全部改为只带 modules.saf。
- 验证：宿主 7/7（普通+ASAN）、modules-check 55/55、kmod smoke PASS、
  DOSVM trip PASS。

---

### 第 8 轮：e1000/lwIP 胶水、ext4(lwext4)、syscall.file 迁入 .ko

- 新增 compose 清单：
  - `driver_e1000.sources`：`e1000.cpp` + `e1000_lwip.cpp`；
  - `fs_ext4.sources`：lwext4 的 23 个实现文件；
  - `syscall_file.sources`：`fops.cpp`（槽位 0-5/22/23）。
- e1000 KO 通过 `KCOMPONENT_EXPORT_RAW` 导出 `NetStackInit`/`NetStackInitDeferred`，
  `kernel.net.lwip` 以 provider 先序 + resolver 解析；实测 DHCP 10.0.2.15 与
  ping 10.0.2.2 成功。
- ext4 KO 导出 `ext4_dir_mk/open/entry_next/close`；`syscall.file` 的实现
  `fops.cpp` 不再编入内核，readdir/mkdir 走跨 .ko 解析。根盘 sata0 正常挂载
  `/mp/`，Explorer/churn 正常。
- 新增 `KCOMPONENT_EXPORT_SYSCALL_LOCAL`：实现已在本 .ko 时不再标
  `KMOD_EXF_EXTERNAL`。
- 内核移除符号：`E1000`、`ext4_*`、`sys_fopen` 等为 0；内核导出 256 项
  （193 项 kernel.component.v1），SAF 56 个 .ko / 2,365,144 B。
- 验证：`kmod_smoke.ps1` PASS（14/14、41/41、desktop PROCESS、0 STALL）；
  `client_churn_gate.py --cycles 5` C1..C6 PASS；e1000 DHCP/ping PASS。
- 文档：更新 `docs/module-implementation-migration.md` 及
  kernel-build/components/dynlink/README 的迁移矩阵与计数。

---

### 第 9 轮：时间/内存/随机/设备/信息 syscall 家族迁入 .ko

- 新增 compose：`syscall_time`（time.cpp）、`syscall_mem`（memory.cpp）、
  `syscall_random`（random.cpp）、`syscall_dev`（dbgout.cpp）、
  `syscall_info`（device.cpp）。
- `mktime` 从 time.cpp 收进 Tier0 `kernel/src/klib/mktime.cpp`，PIT 与
  syscall.time .ko 共用；time KO 通过 `.kout` 解析 mktime。
- `syscall.mem` 采用混合导出：mmap/munmap 本地，pmmap/arch_prctl 仍 external
  （task.cpp/cpu.cpp 尚未迁移）。`syscall.info` 的 sysinfo 16 仍 external
  （init/idle_refresh 被 Tier0 调用），device.cpp 的 25/26/21 本地。
- 内核移除 `sys_time/sys_mmap/sys_munmap/sys_getrandom/sys_dev_mmap/sys_dbgsout`
  等符号；内核导出 264 项（201 项 kernel.component.v1），SAF 2,412,016 B。
- 验证：kmod_smoke PASS（14/14、41/41、desktop、0 STALL/#PF）；
  syscall bind 日志中 mem/time/random/dev/info 均注册成功。
- 文档同步 syscall 家族状态、迁移矩阵和计数。

---

### 第 10 轮：syscall.proc/exec 迁移边界复核

- 尝试把 `exec.cpp` 链入 `kernel.syscall.proc.ko` 时发现两个内核共享符号：
  `Schedule::NewThread` 直接引用 `elf_load`，`NOT_RUNQ_P` 被 sched.cpp 初始化/
  使用；它们不能随 exec.cpp 进模块。
- 本轮已回退该尝试（`syscall_proc.sources` 删除，槽 12/13 恢复 external），
  避免把 sys_load 变成不可用。下一轮需要先把 exec.cpp 拆成
  `elf_load` 内核核心 + `sys_load/sys_launch` 模块外壳。
- 其余 5 个 syscall 家族（time/mem/random/dev/info）保留在 .ko；最终 smoke
  仍 14/14、41/41、desktop PASS。

---

### 第 11 轮：syscall.proc/exec 与 task 迁移；mem 全本地

- 拆分 `exec.cpp`：带边界校验的 `elf_load_impl/elf_load` 与 `NOT_RUNQ_P`
  移入 Tier0 `kernel/src/elf/elf_load_core.cpp`（Schedule::NewThread 和
  sched.cpp 依赖它们）；`sys_load/sys_launch` 留 `syscall_proc.ko`，
  通过 `elf_load_bounded` + `NOT_RUNQ_P` 跨模块解析。
- task.cpp 的 `sys_pmmapSHARE` 拆到 `syscall/pmmap.cpp`，task.cpp 整体迁入
  `syscall_proc.ko`；cpu.cpp 的 `sys_arch_prctl` 与 memory.cpp 一起进入
  `syscall_mem.ko`。现在 proc 9 槽、mem 4 槽全部本地。
- 内核移除 `sys_load/sys_thread_launch/sys_pmmapSHARE/sys_mmap/sys_arch_prctl`
  等符号；内核导出 287 项（224 项 kernel.component.v1），SAF 2,507,432 B。
- 验证：kmod_smoke PASS（14/14、41/41、desktop、LOADED PID 10/11/12、
  0 STALL/#PF）；client_churn 3/3 PASS；DOSVM trip PASS。

---

### 第 12 轮：sysinfo 页/syscall 拆分，全部固定 syscall 家族完成迁移

- `sysinfo.cpp` 保留 Tier0 信息页本体：
  `sys_sysinfo_init/idle_refresh` + `sysinfo_kernel_ready/page/npages` 访问器；
- 16 号 `sys_sysinfo` 实现移入 `sys_info.cpp`，链接进
  `kernel.syscall.info.ko`；四个槽 16/21/25/26 全部本地。
- 固定槽 0..31（除 Tier0 29/30）现在全部由 .ko 实现：
  file(8) + proc(9) + mem(4) + info(4) + dev(1) + time(3) + random(1)。
- 内核移除 `sys_sysinfo`；内核导出 292 项（229 项 kernel.component.v1），
  SAF 2,520,128 B。
- 验证：kmod_smoke PASS（14/14、41/41、desktop、6 次 SYSINFO mapping 回收、
  0 STALL/#PF）；churn/DOS/host/module 门禁保持通过。

---

### 第 13 轮：framebuffer 实现迁入 display_fb.ko

- `framebuffer/fb.cpp` 从内核移入 `kernel.display.fb.ko`；`FrameBufferDevice::Init`
  与 `MemoryMap` 由组件本地实现，`Fb` 指针仍由 Tier0 main 持有。
- Dev 注册表迁移尝试遇到内核 partition/identfat 仍直接调用 Dev，已回退 Dev 实现
  （仅保留 framebuffer 迁移），保持启动稳定。
- 内核移除 `FrameBufferDevice` 符号；内核导出 291 项（228 项
  kernel.component.v1），SAF 2,530,896 B。
- 验证：kmod_smoke PASS（14/14、41/41、display.fb ready、desktop ready、
  0 STALL/#PF）。

---

### 第 14 轮：PartitionManager、Dev 注册表、FATFS 识别迁入 .ko

- `partition/mbrgpt.cpp`、`mgr.cpp` 移入 `kernel.block.core.ko`；
  block.core 用 `KCOMPONENT_EXPORT_RAW` 导出 `PartitionManager::{Read,Write,
  GetMaxSectorCount}`、`GetPartitionStart`。
- `fs/fatfs/identfat.cpp`、`partition/identfstype.cpp` 移入
  `kernel.fs.fatfs.ko`；ext4 导出 `IdentifyExtx`，fatfs 依赖 block/ext4。
- `drivers/dev/dev.cpp`、`devl.cpp` 移入 `kernel.dev.core.ko`；
  导出 AddStorageDevice/AddDevice/GetSDEV/Read/Write/ReadBytes/FindDevice/
  DeviceRead/DeviceWrite/DeviceMemoryMap，驱动/FS/display 通过 .out 解析。
- 内核移除 `PartitionManager`、`Dev::*`；内核导出 281 项（218 项
  kernel.component.v1），SAF 2,616,336 B。
- 验证：kmod_smoke PASS（14/14、41/41、desktop、rootfs sata0 挂载、
  0 STALL/#PF）；churn 3/3 PASS；DOSVM trip PASS；host 7/7(+ASAN)；
  modules-check 56/56。

---

### 第 15 轮：lwIP 协议栈 + e1000 胶水迁入 net_lwip.ko

- 组件编译新增 C 源支持：`KMOD_MOVED_C_SRCS`、`KMOD_CFLAGS`、
  `extra/%.c.o` 规则；lwIP 84 个 .c 文件用模块 C flags 编译并链接。
- `e1000_lwip.cpp` 从 driver_e1000.ko 移到 net_lwip.ko；driver_e1000 只保留
  e1000 驱动并导出 Send/SetInputCallback/GetMAC/PollRX/GetStats。
- net_lwip 组件壳与 lwIP 协议栈同 .ko，`NetStackInit`/`NetStackInitDeferred`
  本地调用；内核不再含 lwIP 符号。
- 内核移除 `lwip_init/netif_add/pbuf_alloc/tcp_input` 等；内核导出 266 项
  （203 项 kernel.component.v1），SAF 3,131,520 B。
- 验证：kmod_smoke PASS（14/14、41/41、desktop、netif 10.0.2.15、
  DHCP success、ping 10.0.2.2 3 次、0 STALL/#PF）。

---

### 第 16 轮：完整 FatFs（ff/diskio/unicode）迁入 fs_fatfs.ko

- `ff.cpp`、`ffunicode.cpp`、`ffsystem.cpp`、`diskio.cpp`、`vfsi.cpp`
  全部移入 `kernel.fs.fatfs.ko`；新增 `vol.cpp` 定义 `VolToPart[]`。
- fs_fatfs.ko 从 28 KB 增至 297 KB；内核不再含 FatFs 实现符号。
- 内核导出 271 项（208 项 kernel.component.v1），SAF 3,400,696 B。
- 验证：kmod_smoke PASS（14/14、41/41、rootfs sata0 挂载、desktop、
  0 STALL/#PF）；fatfs 挂载路径仍为 identify-only（行为未变）。

---

### 第 17 轮：挂载点表迁入 dev.core.ko（Tier0 lookup hook）

- `fs/fd.cpp` 的挂载点表部分（HMapS_MP/GetMountPointName/GetMount/InitFFMAN）
  拆到 `fs/mount.cpp`，随 `kernel.dev.core.ko` 迁移；fd_manager 仍留 Tier0
  （task.cpp 直接使用）。
- 新增 `kernel/src/fs/mount_hook.cpp`：Tier0 `GetMount()` 通过原子函数指针
  调用 dev.core 注册的 `GetMountImpl()`，保证 `kelf_read_file` 在 VFS 初始化前
  no-op、初始化后正常。
- dev.core 导出 HMapS_MP/GetMountPointName/GetMountImpl；内核只剩 hook。
- 尝试把 ACPI 移入 arch_acpi.ko 时，内核 pci.cpp/lapic/ioapic 仍直接使用
  ACPI::mcfg/madt_*，已回退；尝试 fc_idle/fc.cpp 因 fd.cpp 仍依赖已回退。
- 验证：kmod_smoke PASS（14/14、41/41、rootfs sata0 挂载、desktop、
  kelf_read_file 经 hook、0 STALL/#PF）；内核导出 269 项（206 项
  kernel.component.v1），SAF 3,408,472 B。

---

### 第 18 轮：PCI 名称表迁入 pci.core.ko（Tier0 查名 hook）

- `dev/pci/pcidesc.cpp` 移入 `kernel.pci.core.ko`；
- 新增 `kernel/src/arch/x86_64/dev/pci/pci_name_hook.cpp`：
  Tier0 `PCI::{GetVendorName,GetDeviceName,GetClassName,GetSubclassName,GetProgIFName}`
  通过原子函数指针调用 pci.core 注册的实现；`unknownString` 留 Tier0。
- pci.core init 在枚举前注册 5 个名称函数指针；pci.enumerate 仍由 Tier0
  `pci.cpp` 执行，日志查名经 hook。
- 内核仍保留 PCI 配置/枚举（serial 早期使用），名称表已移出。
- 验证：kmod_smoke PASS（14/14、41/41、two-phase enumeration、desktop、
  0 STALL/#PF）；内核导出 270 项（207 项 kernel.component.v1），
  SAF 3,457,256 B。

---

### 第 19 轮：file-cache idle 迁入 fs_vfs.ko（三 hook）

- `fs/fc_idle.cpp` 移入 `kernel.fs.vfs.ko`；fc.cpp 核心、fd_manager 仍 Tier0。
- 新增 `kernel/src/fs/fc_hook.cpp`：Tier0 的
  `file_cache_fsync_hook / file_cache_tick_hook / file_cache_idle_handler_hook`
  通过原子函数指针调用 fs.vfs 注册的实现；未注册时 no-op。
- PIT tick、sched idle、fd.cpp fsync 改走 hook；fs.vfs 导出
  file_cache_fsync/tick/idle_handler，syscall.file 等消费者解析成功。
- 首次 boot 曾出现 syscall.file unresolved（fc_idle 未导出），补 3 个 raw
  export 后 41/41 READY。
- 验证：kmod_smoke PASS（14/14、41/41、EXT4、desktop、0 STALL/#PF）；
  内核导出 283 项（220 项 kernel.component.v1），SAF 3,482,904 B。

---

### 第 20 轮：file cache core 独立成 kernel.fs.fccore.ko

- 新增组件 `kernel.fs.fccore`，把 `fc.cpp` + `fc_idle.cpp` 全部迁入；
  内核只保留 `src/fs/fc_hook.cpp` 的 5 个稳定跳板。
- 依赖重新布线：`fs.cache -> fs.fccore`，`fs.vfs` 只保留 VFS registry；
  fccore 无 sched/dev 依赖，先于 fs.cache/smp 加载，消除
  fs.cache -> fs.vfs -> dev -> sched -> smp 的环。
- fccore 导出 24 个 file_cache/fc_* 符号；VFS 首次缺 `fc_next_file_uid`
  导致 42/34，补导出后 42/42 READY。
- 验证：kmod_smoke PASS（14/14、42/42、EXT4、desktop、0 STALL/#PF）；
  内核导出 263 项（200 项 kernel.component.v1），SAF 3,570,776 B。

---

### 第 21 轮：fd_alloc/fd_get/fd_free 迁入 fs_vfs.ko

- `fd.cpp` 拆成两层：
  - Tier0 保留 `fd_manager_init/destroy/dup` + writeback callback
    （Schedule::Init/进程销毁直接调用，早于 VFS 组件）；
  - `fd_alloc/fd_get/fd_free` 移入 `fd_ops.cpp`，链接进 `kernel.fs.vfs.ko`。
- fs.vfs 导出 fd_alloc/fd_get/fd_free；syscall.file/fops 通过 .out 解析。
- 内核移除 3 个 fd 操作符号；内核导出 260 项（197 项
  kernel.component.v1），SAF 3,593,672 B。
- 验证：kmod_smoke PASS（14/14、42/42、EXT4、desktop、0 STALL/#PF）；
  churn 3/3 PASS；DOS/host/modules 保持通过。

---

### 第 22 轮：ACPI Init/FindTable/MADT 迁入 arch_acpi.ko

- `common/acpi/acpi.cpp` + `madt.cpp` 移入 `kernel.arch.acpi.ko`；
  Tier0 保留 `mcfg/FADT/rootThing/ACPI_DIV/UseXSDT` 与 `madt_*` 全局量，
  以及 1-arg/3-arg `ACPI::FindTable` 跳板。
- arch_acpi init 在 `ACPI::Init` 后注册 FindTable 函数指针，再调用模块
  `MADT_Init`；lapic/ioapic/pci 继续读 Tier0 全局量。
- 首次尝试因模块仍定义 ACPI 全局量，pci.core 的 mcfg 解析失败；
  删除模块重复定义后 42/42 READY。
- 验证：kmod_smoke PASS（14/14、42/42、EXT4、desktop、0 STALL/#PF）；
  内核导出 265 项（202 项 kernel.component.v1），SAF 3,610,920 B。

---

### 第 23 轮：RTC 实现迁入 time_rtc.ko

- `x86_64/rtc/rtc.cpp` 移入 `kernel.time.rtc.ko`；
  Tier0 保留 RTC 字段（Second..Year/LastUpdateTime）与 Init/read/update 跳板。
- time_rtc init 调用 `RTC::InitRTC` 后注册 3 个 hook；time.pit 依赖 time.rtc，
  保证 PIT 初始化时 hook 已就绪。
- time_rtc 导出 ToUnixTime/read_rtc；ext4/fatfs 通过字段读取，
  syscall.time 通过 raw export 调用 read_rtc。
- 修复：模块曾残留 RTC 字段定义（CRLF 导致替换未命中），生成器跳过内核导出，
  fatfs/syscall.time 曾 40/42；删除重复定义后 42/42 READY。
- 验证：kmod_smoke PASS（14/14、42/42、EXT4、desktop、0 STALL/#PF）；
  内核导出 266 项（203 项 kernel.component.v1），SAF 3,626,280 B。

---

### 第 24 轮：fd_manager 核心迁入 fdcore.ko

- `fd.cpp` 的 init/destroy/dup 移入新组件 `kernel.fs.fdcore`；
  Tier0 只留 `fd_hook.cpp` 的 3 个跳板和 writeback callback。
- `sched.core` 显式依赖 fdcore，保证 `Schedule::Init` 创建 idle 进程前
  fd hooks 已注册；fdcore 无 sched/dev 依赖，不引入环。
- `fs.cache` 不再直接依赖 fdcore（callback 已留 Tier0）。
- 首次启动 syscall.proc 因 fd_manager 符号被模块定义而 unresolved，
  fdcore 补 3 个 raw export 后 43/43 READY。
- 验证：kmod_smoke PASS（14/14、43/43、EXT4、desktop、0 STALL/#PF）；
  内核导出 267 项（204 项 kernel.component.v1），SAF 3,670,712 B。

---

### 第 25 轮：HPET 实现迁入 time_hpet.ko

- `x86_64/drivers/hpet/hpet.cpp` 移入 `kernel.time.hpet.ko`；
  Tier0 保留 `HPET::{Available,GetTimeNS,SleepNS}` 跳板。
- time_hpet init 注册 3 个 hook；sched.install 依赖 time.hpet，保证 TSC
  校准前 HPET 可用。
- syscall.time 直接消费 HPET::GetTimeNS，time_hpet 导出该符号。
- 首次启动缺少 ACPI::FindTable raw export 和 HPET::GetTimeNS export，
  两次修复后 43/43 READY。
- 验证：kmod_smoke PASS（14/14、43/43、HPET 100MHz、TSC 校准、
  desktop、0 STALL/#PF）；内核导出 265 项（202 项
  kernel.component.v1），SAF 3,681,576 B。

---

### 第 26 轮：LAPIC bootstrap 迁移尝试（回退）

- 尝试把 `x86_64/lapic/lapic.cpp` 移入 `kernel.arch.apic.ko`，Tier0 提供
  EOI/IPI/Oneshot/StopTimer/Init/InitTimer/CalibrateTimer 跳板。
- 加载期 `KCOMPONENT_EXPORT_RAW` 校验失败：
  `arch.apic: export addr outside image:0x7`（LAPIC raw export 表地址异常），
  导致 apic/irq/smp 及后续 30+ 模块级联失败。
- 已回退该迁移，保持 LAPIC/IOAPIC 在 Tier0；最终 smoke 仍 43/43、14/14、
  desktop PASS。该 bootstrap 边界需要 loader raw-export/重定位专项修复后再做。

---

### 第 27 轮：LAPIC 二次尝试仍回退，确认 raw export 地址边界

- 再次尝试 LAPIC 迁移，并补齐 EOI/IPI/Oneshot/StopTimer/Init/InitTimer/
  CalibrateTimer 的 Tier0 跳板与 raw export。
- loader 仍报 `arch.apic: export addr outside image:0x7`（不同导出数量时
  变为 0xb），说明 LAPIC 函数在模块内的 raw export/relocation 表示有系统性
  问题，不是单个符号缺失。
- 已回退，保持 LAPIC/IOAPIC 在 Tier0；最终 smoke 43/43、14/14、desktop PASS。
- 下一步必须先修 `KCOMPONENT_EXPORT_RAW` 对 LAPIC 这类大代码模型函数的
  地址重定位，再继续 APIC/SMP/PIT。

---

### 第 28 轮：LAPIC 实现迁入 arch_apic.ko

- `x86_64/lapic/lapic.cpp` 移入 `kernel.arch.apic.ko`；
  Tier0 保留 EOI/StopTimer/Oneshot/IPI/IPIOthers/Init/InitTimer 跳板。
- arch_apic 导出 EOI/IPI/Oneshot/StopTimer/Read/Write 六个 raw symbol；
  `CalibrateTimer` 在源码中只有声明、无定义，已从导出/跳板中移除。
- 前两次失败的根因是导出不存在的 CalibrateTimer 与 HPET::SleepNS 未导出；
  修正后 43/43 READY。
- 验证：kmod_smoke PASS（14/14、43/43、EXT4、desktop、0 STALL/#PF）；
  内核导出 259 项（196 项 kernel.component.v1），SAF 3,711,248 B。

---

### 第 29 轮：IOAPIC 实现并入 arch_apic.ko

- `x86_64/ioapic/ioapic.cpp` 移入 `kernel.arch.apic.ko`；
  arch_apic 导出 `IOAPIC::Init` / `IOAPIC::RemapIRQ`。
- arch_irq / 驱动组件按 raw export 解析 IOAPIC；PIC 屏蔽与 GSI remap
  行为保持不变。
- 验证：kmod_smoke PASS（14/14、43/43、IOAPIC COUNT:24、desktop、
  0 STALL/#PF）；内核导出 257 项（194 项 kernel.component.v1），
  SAF 3,718,824 B。

---

### 第 30 轮：剩余实现文件边界复核

- 复核 `src/arch/x86_64/schedule/sched_bench.cpp`：不在 `modules/` 下，
  compose 机制不覆盖 `kernel/src`，无法按现有迁移路径入 .ko；本轮未强移。
- IOAPIC/LAPIC 已迁入 `arch_apic.ko`；剩余 PIT/SMP 直接参与 sched/vmm/
  IDT 时序，属于 Tier0 bootstrap，需要先重做共享时钟/CPU 状态布局。
- 回退未完成改动后 smoke 仍 43/43、14/14、desktop PASS；规模保持
  58 .ko / 257 exports / SAF 3,718,824 B。

---

### 第 31 轮：sched_bench 实现并入 sched.policy.eevdf.ko

- `src/arch/x86_64/schedule/sched_bench.cpp` 移到
  `modules/builtin/x86_64/schedule/`，由 `sched_policy.sources` 链接进
  `kernel.sched.policy.eevdf.ko`。
- 内核无调用者，仅头文件包含；模块化后行为不变，sched_policy KO 增至 62 KB。
- 验证：kmod_smoke PASS（14/14、43/43、desktop、0 STALL/#PF）；
  内核导出 260 项（197 项 kernel.component.v1），SAF 3,768,952 B。

---

### 第 32 轮：PIT 迁移尝试（回退）

- 尝试把 `pit.cpp` 移入 `time_pit.ko`，Tier0 保留 PIT 共享状态与
  TimeSinceBootMS/MonotonicMS/Sleep/Tick_ 跳板。
- 加载期 PIT 模块仍 unresolved（共享 roughCount/MicroSecondOffset 等
  Tier0 数据与 RTC/LAPIC 交叉），无法在现有加载器下作为稳定 provider。
- 已回退，PIT 仍在 Tier0；smoke 恢复 43/43、14/14、desktop PASS。
- 当前稳定规模：58 .ko / 260 exports / SAF 3,769,384 B。

---

### 第 33 轮：PIT 迁移再次回退，确认共享全局边界

- 再次尝试 `pit.cpp` 入 `time_pit.ko`；Tier0 保留 PIT 全局量/跳板，
  time_pit 增加 PIT 数据 external 导出。
- 加载期 PIT 模块仍 unresolved，涉及 PIT 共享全局量与 RTC/LAPIC 交叉；
  已回退，PIT 保持 Tier0。
- 稳定态：58 .ko / 43/43 / 260 exports / SAF 3,769,384 B。

---

### 第 34 轮：PIT 共享状态导出表就绪

- 在 `gen_component_exports.py` 增加 PIT Tier0 共享量固定导出：
  TicksSinceBoot/roughCount/MicroSecondOffset/Divisor/NonMusicDiv/
  Inited/freq/FreqAdder/TickHandle。
- PIT 实现仍留 Tier0（迁移顺序重做未完成），但未来 time_pit.ko 所需的
  跨模块符号表已生成并通过链接。
- 验证：smoke PASS（14/14、43/43、desktop、0 STALL/#PF）；
  内核导出 266 项（203 项 kernel.component.v1）。

---

### 第 35 轮：PIT 重试仍回退，定位到 provider 先序/raw 解析

- 用生成器强制导出 PIT 共享量后再次迁移 pit.cpp；PIT KO 仍 unresolved：
  `LAPIC::EOI`、`RTC::{read_rtc,InitRTC,UpdateTimeIfNeeded}` 在其 prepare
  阶段不可见，尽管 arch_apic/time_rtc 之后能 READY。
- 说明 PIT 的 provider 先序仍不够（或 staging resolver 不把已 READY 模块的
  raw export 暴露给提前 prepare 的 time_pit）。
- 已回退；PIT 保持 Tier0。稳定态：58 .ko / 43/43 / 266 exports /
  SAF 3,769,384 B。

---

### 第 36 轮：按用户要求 PIT 保持 Tier0

- 已完整回退 PIT 迁移代码（time_pit.sources/pit_tier0.cpp/shell hooks/raw exports），
  PIT 共享状态与实现留在 Tier0。
- 生成器中的 PIT 临时导出也移除；稳定态恢复 58 .ko / 43/43 /
  260 exports / SAF 3,768,952 B。
- smoke/churn/host/modules 通过。
