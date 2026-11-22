<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# 桌面安装程序、Settings 与 Notepad 文件对话框

本文记录桌面可用性增强：Installer 安装程序、桌面 Settings / Lua 配置加载界面、
以及 Notepad 的 Open / Close 文件对话框。三者都运行在现有 WM client 模型上，
不改变窗口几何/协议页布局；桌面内嵌 Lua 仍然是壁纸与窗口主题的执行引擎。

## 1. 组成与入口

| 程序/模块 | 位置 | 入口 |
|---|---|---|
| Installer | `programs/installer/` | 桌面全局快捷键 **Ctrl+Alt+I**；或聚焦 Settings 后按 **I** |
| Settings | `programs/settings/` | 桌面启动时自动 spawn（置顶）；**Ctrl+Alt+S** 重新聚焦 |
| Notepad | `programs/notepad/` | 桌面启动时自动 spawn；**Ctrl+Alt+N** 重新聚焦 |
| Wallpaper / 主题 | `programs/desktop/`（内嵌 Lua 5.4） | 启动时执行 `/mp/desktop.lua`；Settings 可选择其它 `.lua` |

Settings 的 `I` 与 Ctrl+Alt+I 都会让桌面调用 `wm_client_spawn("/mp/installer.elf")`。
首启（`/mp/themes.index` 不存在）时 Settings 显示
`(no themes installed - press I to run Installer)`，避免安装写盘与启动期大量并发
FS 读叠加。

## 2. Installer（安装程序）

`programs/installer/main.cpp` 是动态链接 WM client，启动时先写资源、最后才
`CLIENT_READY`（因此桌面的 spawn 返回时资源已就绪）。重复运行覆盖旧文件并在
UI/串口标记 overwrite。

安装文件（全部在 `/mp/` 根，无需 mkdir）：

| 文件 | 内容 |
|---|---|
| `/mp/themes.index` | `显示名|/mp/路径` 行；Aurora / Ember / Mint |
| `/mp/theme_aurora.lua` | 脚本自绘壁纸（0x102040→0x7A2F6E + 极光带/星点）+ `sky.set_window_style{...}` |
| `/mp/theme_ember.lua` | 0x2A0F14→0x9C3B2E + 余烬/火星，radius=6 |
| `/mp/theme_mint.lua` | 0x0F2A24→0x2E7A5E + 棋盘/叶脉，radius=18 |
| `/mp/files.index` | Notepad Open 列表：`desktop.lua` / 3 个主题 / `notes.txt` |
| `/mp/notes.txt` | 示例笔记（UTF-8） |

主题脚本只使用预置的 `wallpaper` FrameBuffer 变量（`width/height/format` +
`fill/pixel/hline/vline/rect/size`）自己绘制壁纸，并调用
`sky.set_window_style{corner_radius,title_color,border_color,separator_color,
paper_color,ink_color,close_color,shadow_alpha}` 设置窗口 chrome；C++ 不解析
渐变/颜色配置。安装完成后 Installer 通过命令邮箱请求桌面立即加载
`/mp/theme_aurora.lua`。

## 3. Settings / `.lua` 配置加载界面

`programs/settings/main.cpp` 是 WM client；内容区显示：

- 当前主题（读 `/mp/selected_theme.txt`，兼容 `显示名|路径` 或只有显示名两种格式）；
- `/mp/themes.index` 中的主题列表（缺失/空时提示去运行 Installer）；
- 选中项路径、底部快捷键与最近一次请求状态。

按键：

| 键 | 行为 |
|---|---|
| Up / Down | 选择（PgUp/PgDn/Home/End 附加滚动） |
| Enter | 写 `/mp/selected_theme.txt`，发送 `SKYWIN_CMD_LOAD_LUA` + 主题路径 |
| R | 发送 `SKYWIN_CMD_RELOAD_DESKTOP`（重载 `/mp/desktop.lua`） |
| I | 发送 `SKYWIN_CMD_SPAWN_INSTALLER` |
| L | 重新读取 `themes.index` / `selected_theme.txt`（列表为空时也会每秒自动重读） |
| Esc | 退出（WM 回收窗口） |

命令通过协议页 client -> WM 命令邮箱传递（`lib/include/graphic/winstyle.h`）：

```
[10] CMD      [11..14] 32-byte arg（/mp/... 路径）      [15] CMD_SEQ
SKYWIN_CMD_LOAD_LUA / SKYWIN_CMD_RELOAD_DESKTOP / SKYWIN_CMD_SPAWN_INSTALLER
skywin_send_command(q, cmd, arg);
```

desktop 在每帧轮询所有 WM client 的 `CMD_SEQ`；收到 `LOAD_LUA` 后：

1. `SkyLuaLoadConfigFile(path)` 关闭旧 `lua_State`、重置窗口样式、清空壁纸缓冲、
   重新建立受限库 + `wallpaper` 变量并执行新脚本；
2. 用新主题重绘任务栏、控制台与所有窗口 chrome（内容区像素保存/恢复）；
3. `Compositor::ForceFullCompose()` 强制下一帧整屏重组（壁纸缓冲指针不变，
   否则增量合成会保留旧壁纸行）。

串口证据：`[desktop] theme: load path=/mp/theme_xxx.lua rc=0 radius=<n>`，
以及重载后的 `[desktop] lua: style ...` / `[desktop] lua: wallpaper: script-painted ...`。

## 4. Notepad Open / Close 对话框

`programs/notepad/main.cpp` 增加模态文件对话框（键盘操作，不改窗口几何）：

| 快捷键 | 行为 |
|---|---|
| Ctrl+O | Open 对话框：读 `/mp/files.index` 的 `显示名|/mp/路径` 列表（缺失时内置 3 项 fallback）+ Cancel；Up/Down/Enter/Esc |
| Ctrl+W | Close 对话框：**Save and Close / Discard and Close / Cancel**；Up/Down/Enter/Esc |
| Ctrl+S | 直接保存到当前路径（无路径时 `/mp/notes.txt`），状态栏短暂显示 `Saved: ...` |

Open 读取文件按 `\n` 拆行、处理 CRLF、超长行/超行数截断；失败留在对话框并显示原因。
Close 的 Save 失败也不会退出；Discard 直接 `sys_exit(0)`，WM 自动回收窗口。
串口标记（供无头门禁）：
`[notepad] start content=WxH`、`[notepad] dialog=open items=N`、
`[notepad] opened path=... bytes=...`、`[notepad] dialog=close`、`[notepad] saved ...`、
`[notepad] close choice=save|discard|cancel`、`[notepad] exit saved=0|1`。

## 5. 构建与集成

- `programs/Makefile` 构建/拷贝：`installer.elf`、`settings.elf`、`notepad.elf`、
  `desktop.elf` 全部经 `e2cp` 进入 `disk.img` 根（`/mp/`）；
- 桌面 `main.cpp`：Settings 在启动时 spawn 并置顶；全局快捷键
  Ctrl+Alt+S / Ctrl+Alt+N / Ctrl+Alt+I 分别聚焦 Settings / Notepad /
  spawn-or-focus Installer；
- 主题重载后调用 `Compositor::ForceFullCompose()`（`synthesizer/window.cpp`）；
- 命令邮箱常量/helper 在 `lib/include/graphic/winstyle.h`，`wm_client.cpp` 提供
  `wm_client_poll_command()` 与 `wm_client_repaint_chrome()`。

## 6. 运行验证

```powershell
# 1) 清空已安装资源（触发首启），然后启动 QEMU
#    wsl e2rm disk.img:/themes.index ...（或直接使用预清理的 disk.img）
# 2) Installer：等待桌面 ready，发 Ctrl+Alt+I
#    sendkey ctrl-alt-i
#    串口应出现 [installer] Install complete: 6/6 files ...，
#    以及 [desktop] theme: load path=/mp/theme_aurora.lua rc=0 ...
# 3) Settings：Ctrl+Alt+S，Down/Enter
#    串口应出现 [desktop] theme: load path=/mp/theme_mint.lua rc=0 radius=18
# 4) Notepad：Ctrl+Alt+N，Ctrl+O，Down/Enter；再 Ctrl+W
#    串口应出现 [notepad] dialog=open items=5 / opened path=... / dialog=close
```

自动验证脚本：`res/scripts/test/desktop_features_smoke.ps1`
（QEMU + HMP `sendkey` + 串口标记 + screendump 像素差异），详见脚本头注释。

## 7. 已知限制

- Installer 不随桌面自动启动：首启由 Settings 提示 + `I`/Ctrl+Alt+I 触发，避免与
  启动期并发 FS 负载叠加；重复运行会覆盖安装资源（包括 notepad 保存的 notes.txt）。
- Notepad 的 Open 列表仍读静态 `/mp/files.index`（Notepad 未改用 readdir）；
  Explorer 通过新增 syscall 23 真实枚举目录，见第 11 节。
- 主题脚本只允许 `/mp/` 路径；命令邮箱参数区 32 字节（路径 ≤31 字节）。
- 窗口几何固定（宽高/标题栏/客户端协议页），主题只改视觉参数；脚本启动/重载时
  执行一次，修改文件后需按 R/重新选择主题或重启桌面。
- Notepad 保存把 CRLF 规范化为 LF；Open 列表上限 16 项。
- `printf_` 不支持 `%a` 等既有 libc 限制仍适用（见 docs/lua-desktop.md）。
- 内核 FS 写路径与 AHCI 轮询的 SMP 兼容修复见下节；安装器假设 `/mp` 可写。

## 8. 关联内核修复

首启安装器写盘 + DOOM 启动暴露了三条 SMP/FS 问题，均已修复并通过 QEMU
（4 CPU，`desktop_features_smoke.ps1`）验证：

1. AHCI 轮询在 ext4 临界区内开中断：`Port::Read/Write` 曾 `sti` 后轮询，
   而 ext4 挂载点锁是 `spin_lock_irqsave` 的 CPU 递归锁；中断开启后线程可能
   被抢占/迁移，owner 判定失效，导致 ext4 bcache 并发访问、`ext4_buf_free`
   invalid free 或 CPU 卡死。修复：轮询期间不再 `sti`
   （`kernel/modules/common/drivers/ahci/ahci.cpp`）。
2. ext4 挂载锁改为线程 owner + 可让出等待：`ext4.cpp` 的递归 owner 从
   “CPU id” 改为 “当前线程指针”，等待者在开着中断的状态下 `Schedule::Yield()`
   而不是关中断自旋；这样即使持锁线程等磁盘 I/O，其他 CPU 的 TLB shootdown /
   定时器/调度也不会被饿死（之前表现为 STALL WATCHDOG 连锁）。
3. 大文件不进 file_cache：`sys_fread` 对 `file_size > 1 MiB` 跳过块级
   `file_cache_promote`（DOOM WAD、字体等）；小文件（程序/配置/主题）仍缓存。
   这规避了实测 DOOM 启动阶段 `file_cache_promote` 的 invalid free（缓存条目
   生命周期竞态），同时显著降低大文件读取的内存/淘汰 churn。
4. 键盘 Esc 映射：`kernel/include/drivers/keyboard/x86/keyboard_map.h` 的
   `kb_map_keys[1]`（scancode 0x01）原为 0，Esc 被输入核心丢弃；改为 `0x1B`
   后 Installer/Settings/Notepad 的 Esc 关闭/取消路径才真实可用
   （kbdshare 注释约定 Esc 携带 ASCII 0x1B）。注意 kernel 构建对头文件依赖
   不完全，改完该头文件后需要 touch `keyboard.cpp` 或 clean build。

QEMU 最终验证：`desktop_features_smoke.ps1` PASS, 0 warning, 0 STALL,
0 panic；覆盖 Installer 首装、Esc 关闭、Settings 自动刷新 + Mint 切换、
Notepad Open/Close/Cancel、R 重载。证据：`desktop_features_serial.log` 与
`desktop_features_*.ppm`。

## 9. WM 焦点/z-order、最小化先态与子进程退出事件（2026-11）

### 9.1 z-order / 键盘焦点

- 栈序统一使用 `CompLayer->z`（console 独占 layer，通用客户端各独占 auto layer）。
- 点击任意窗口（caption / 内容 / 缩放带）→ `Compositor::RaiseWindow` + 同帧
  `wm_focus_immediate(pid)`（记录 `press_focus_pid` 并调用
  `kbd_focus_set_slot`，不再等下一帧 `wm_refocus`）。任务栏 restore/raise、
  spawn、设置命令同样立即聚焦。
- `wm_refocus()` 只接受仍可见、未最小化、未关闭的显式焦点；失效时回退到
  console/client 中 `layer->z` 最高的可见窗口，无可见窗口则广播。
- 隐藏窗口不参与命中测试；最小化不改 z 序，恢复时 raise。

### 9.2 最小化保持最大化

- console：caption 最小化按钮与任务栏 pill 两条路径都保存 `wm_prior`
  （NORMAL/MAX）；恢复按 prior 回到最大化/普通，最小化状态下最大化按钮 no-op。
- 通用客户端：`WmClient::pre_minimized_max` 记录最小化前是否最大化；
  `wm_client_restore` 先恢复最大化几何，再 SetVisible + Raise + focus。
- 键盘兜底：`Ctrl+Alt+C` 聚焦/恢复 Console，`Ctrl+Alt+M` 切换焦点窗口最大化，
  `Ctrl+Alt+H` 最小化焦点窗口；用于无鼠标/无头验证。

### 9.3 子进程退出事件（syscall 31）

- `proc_t` 尾部固定 ring（8 条 `{int32 pid; int32 code;}` + head/tail/overflow），
  锁复用 `PROC_LIST_LOCK`；`FinalizeProcExit`（自退出）与 `DeleteProc`（外部
  kill）在摘除 `parent->children` 之前入队；父进程 exiting 时跳过。
- 用户 ABI：`SYSCALL_CHILD_EVENT = 31`，
  `syscall(31, (uint64_t)&ev, 0,0,0,0,0)` → 1=复制一条，0=空，
  -EINVAL/-EFAULT；对应 kmod 固定契约 `child_event`、
  `kernel.syscall.proc` 组件槽 31。
- `sys_load()` 现在把新客户端挂到调用者（desktop）`children` 尾部，使事件可达；
  `sys_load_fail` 摘链，避免失败路径悬空指针。
- desktop 每帧有界 drain（≤16 条），按 pid 立即 `wm_client_reap_pid`/控制台
  卸载；仅在 2s 低频 sweep 保留 `sys_kill(pid,0)` 兜底 ring 溢出，取代旧
  500ms 轮询。

### 9.4 64MB 低内存模式

QEMU `-m 64M` 的可用内存只有约 56MiB（Limine/固件保留）。实测结论：

| 配置 | 结果 |
|---|---|
| 默认 2G | Notepad/Settings 自动常驻，DOOM 由 Ctrl+Alt+D 按需启动 |
| 64M：Notepad + Settings 自动启动、DOOM 延迟 | 稳定，OOM≈13-16 行，无 panic；桌面应用可用 |
| 64M：DOOM_TINY + trimmed WAD | DOOM 可 init 并持续渲染（zone=8MiB，OOM≈25–26，0 panic/STALL，init 后余 ~5–6MiB） |
| 64M：DOOM + Notepad/Settings 同时 | 56MiB 不足，PMM OOM；DOOM 采用按需/独占 |

策略：DOOM 不在启动路径自动拉起（其 R_Init/WAD 冷读会把桌面 ready 推迟
数十秒并与低内存抢页），统一由 `Ctrl+Alt+D` 按需启动；`<80MiB` 时自动启动
Notepad/Settings，把 RAM 优先给桌面应用。PureDOOM `doom_set_zone_mb()`
按内存分档取 zone（DOOM_TINY 下 ≤80MiB → 8MiB，≤128MiB → 10MiB，更大 → 12MiB），
shim 日志 `[doom] zone_mb=... mem_total_mb=... mem_free_mb=...`。
DOOM 窗口 blit 改为始终填满 WM content（实测 content 640×392，旧 2x/1x
判据会因主题圆角 16 回退 1x 留下大片黑边）。

### 9.5 低内存 OOM 加固

- `VMM::VMA::InsertRegion`：PMM 耗尽/哨兵链不完整时返回 nullptr，避免
  `after->next->prev` 在 0x20 处内核 #PF。
- `elf_load_impl`：`PMM::Request()` 返回 0 时终止加载并走失败回收，避免把
  物理 0 映射后向未映射用户页写 128-bit SSE（64MB spawn DOOM 实测崩溃）。
- 桌面关闭客户端时 `wm_drop`/`release_console_shared` 显式 `sys_munmap`
  桌面侧 surface/protocol 别名，减少低内存下关窗后的物理页滞留。

## 10. Setup：安装整个 SkylineSystem（OS 安装）

2026-11 起 `programs/installer/main.cpp` 不再只是“主题资源安装器”，而是
SkylineSystem Setup：在保留旧 6 个主题/配置资源安装（旧 QEMU 门禁
兼容）的同时，把构建期生成的整个 OS 树解包安装到 `/mp/system`。

### 10.1 OS 包（SKYOS1 + LZ4 block）

构建目标：

```sh
make os-pkg          # 生成 programs/bin/os.pkg，并 e2cp 到 disk.img:/os.pkg
```

`cm` / `ma` 在 kernel 与 programs 构建完成后自动调用 `os-pkg`。包由
`res/scripts/build_os_pkg.py` 生成，格式：

```
magic "SKYOS1\0\0"
u32 version=1, entry_count, flags, reserved
每条 entry:
  u16 path_len, u16 reserved
  u32 method (0=raw, 1=lz4)
  u32 orig_size, u32 nblocks
  char path[path_len]
  每个 block: u32 orig_len, u32 comp_len, data
```

- LZ4 是标准 block 编码（无 frame 头），每块 ≤64KiB；
- Python 构建器带独立解码器 roundtrip 自检；C 安装器有 ~75 行 LZ4 block
  解码器，按块解压后 `fwrite`，不把整包读入内存。

当前包（默认不含 doom.wad）：

```
entries=28, compressed=5,460,395 B, raw=8,908,221 B
内容：
  boot/kernel                      (kernel ELF)
  boot/modules.saf                 (58 个 DynModule：40 组件 + 18 demo)
  boot/limine/{limine.conf,limine-bios.sys,BOOTX64.EFI}
  config/desktop.lua
  lib/libsky.so
  programs/{desktop,notepad,settings,installer,doom,hw,hw2}.elf
```

`--with-doom-wad` 可把 IWAD 也打进包（raw ≈20MB）；默认不打包专有 IWAD，
安装后 DOOM 仍从 `/mp/doom.wad` 读取。

### 10.2 内核 mkdir（syscall 22）

安装器需要建 `/mp/system/boot/modules` 等目录树：

- 固定 syscall 槽 22 从 `reserved` 改为 `mkdir`：
  `sys_mkdir(path, mode) -> 0 / -errno`；
- `kernel/modules/x86_64/syscall/fops.cpp::sys_mkdir`：
  校验用户 path、`strncpy_from_user`、`GetMount`，仅 ext4 支持，
  调用 `ext4_dir_mk()`（对已存在目录幂等），其它 FS 返回 `-ENOSYS`；
- `kernel.syscall.file` 组件导出槽 22；kmod 固定契约表 22 改为 `mkdir`；
- 用户头 `lib/include/base/arch/x86_64/syscalln.h`：
  `SYSCALL_MKDIR 22`；host kmod 测试同步（22=mkdir，23 仍 reserved）。

### 10.3 安装流程（Setup/Installer 串口）

1. 旧资源（保持门禁兼容）：
   `/mp/themes.index`、`theme_aurora.lua`、`theme_ember.lua`、
   `theme_mint.lua`、`files.index`、`notes.txt`，打印
   `[installer] Install complete: 6/6 files (...)`；
2. 读 `/mp/os.pkg`：校验 magic/version/entry_count，先扫描 block 表统计
   raw 总量并回到 header 后，输出
   `[setup] os pkg: N entries, compressed=X B, raw=Y B`；
3. 逐级 `mkdir` `/mp/system/...`，逐 entry/block 流式解压写文件；
   每个文件输出 `[setup] install <path> <bytes> B ok`；
4. 在内存中收集 manifest 文本，所有 package 文件关闭后一次性写
   `/mp/system/os.manifest`（避免 libc FILE 槽复用导致跨多次
   `fopen/fclose` 的 manifest 写丢失）：
   `[setup] manifest written: /mp/system/os.manifest (937 bytes)`；
5. 汇总：
   `[setup] os install complete: 18/18 files, compressed=5,793,850 B,
   installed=8,908,221 B, failed=0`；
6. 保留 `[installer] READY: window mounted`、Aurora LOAD_LUA 请求、
   Esc/Enter 退出；UI 标题为 “SkylineSystem Setup”，副标题
   “Install entire operating system”。

缺 `/mp/os.pkg` 时：`[setup] no /mp/os.pkg (legacy resource-only install)`，
旧 6 资源安装仍成功，不失败。

### 10.4 Manifest

`/mp/system/os.manifest`（纯文本）：

```
SKYOS1 installed
version=1
boot/kernel=3432280
boot/limine/BOOTX64.EFI=376832
...
programs/settings.elf=76992
total_files=28
installed_bytes=8907853
```

### 10.5 QEMU 验证

`res/scripts/test/setup_os_smoke.ps1`（2G、4 vCPU、headless HMP）：

- 预处理 disk.img：`e2rm -r /system`，保留 `/mp/os.pkg`；
- 启动 desktop，`Ctrl+Alt+I` 拉起 Setup；
- 断言 `[installer] start`、`[setup] os pkg`、`os install complete ... failed=0`、
  `manifest written`、`READY`、Esc 退出；截图 Setup 窗口；
- 关闭 QEMU 后从 disk.img 读回 `/mp/system` 校验：
  - `os.manifest` 有 SKYOS1/totals；
  - `boot/kernel` >1MiB 且首 4 字节 `7f454c46`（ELF）；
  - `programs/desktop.elf`、`lib/libsky.so` >100KiB；
  - setup 文件计数 == manifest total_files（容差 0）。

实测（2026-11，最终包 5,460,395 B）：

```
SETUP OS SMOKE: PASS (0 warning(s))
A1..A8 PASS；E1..E6 PASS
[setup] os install complete: 18/18 files ... failed=0
[setup] manifest written: /mp/system/os.manifest (937 bytes)
```

截图：`setup_os_installer.png`（Setup 窗口 + 安装日志）。

### 10.6 边界（重要）

- Setup 安装的是 完整 OS 系统树到 ext4 `/mp/system`（kernel/modules/
  bootloader 文件/程序/库/配置），不是从零分区、写 MBR 与
  `limine bios-install` 的“裸机装系统”流程；当前内核/用户态没有原始块设备
  写接口，bootloader 安装仍由构建期 ISO/HDD 镜像负责。
- 目标盘是 QEMU 的 ext4 `disk.img`；FAT/SAF 目标返回 `-ENOSYS`。
- 默认不复制专有 `doom.wad` 和可选 TTF 字体；需要时用
  `build_os_pkg.py --with-doom-wad` 或安装后手动 e2cp。
- 重复运行 Setup 会覆盖 `/mp/system` 文件（ext4_dir_mk 幂等，文件 wb 截断）。

## 11. 开始菜单 + Explorer 文件浏览器（2026-11）

本轮给桌面补上"从任务栏启动应用"与"真正浏览文件系统"两条能力。此前
`docs/desktop-features.md` 第 7 节记录的 "没有目录枚举 syscall，新增文件需
更新 files.index" 限制就此解除。

### 11.1 组成与入口

| 模块 | 位置 | 入口 |
|---|---|---|
| 开始菜单 | `programs/desktop/main.cpp`（WM 内嵌 overlay） | 任务栏最左按钮点击 / **Ctrl+Alt+B** |
| Explorer | `programs/explorer/`（动态 WM client） | 菜单首项 / **Ctrl+Alt+E** |
| readdir | 内核固定 syscall **23** | `syscall(23, path, buf, cap, flags)` |

菜单项（7 条，固定顺序）：Explorer、Notepad、Settings、Installer / Setup、
DOOM、Console、About...。启动时不自动拉起 Explorer（与 DOOM 相同的
低内存策略），只在用户点菜单/按快捷键时 spawn。

### 11.2 开始菜单（WM 自绘 overlay）

- 任务栏最左侧固定 46px 开始按钮（四格窗口标记，不依赖字体）；
  `TB_LEFT_MARGIN` 已改为 `按钮左 + 按钮宽 + gap`，药丸布局与命中测试共用
  同一个宏，不会出现"画在 A、点在 B"的漂移。
- 菜单是一个 WM 独占的 ARGB 窗口（300×376，`RegisterWindowAuto` → 最高的
  auto layer），没有客户端进程与协议页；surface 由桌面 `malloc`，用
  `BasicDraw` 画 SDF 圆角 + 描边，条目文字用 `console_font()`。
- 打开/关闭：`start_menu_open` / `start_menu_close` 注册/注销窗口、
  重画任务栏高亮、`Compositor::ForceFullCompose()`；关闭菜单时点击外部会被
  本次 press 吞掉，不穿透到下层窗口。
- 键盘焦点黑洞：菜单打开期间 `wm_refocus()` 把 `focus_owner` 钉在 WM
  自己的 reader slot，所有客户端都收不到键；关闭时把其它 reader 的
  `consumer[]` 游标推进到当前 `head`，丢弃菜单期间堆积的事件。否则旧焦点
  窗口会"补读"菜单的 Enter/Esc —— 实测过 Settings 把菜单的 Enter 当成
  "应用主题"、Esc 直接退出，这个坑必须显式处理。
- 导航：Up/Down/Home/End 选择、Enter 启动、Esc 关闭；About 页面 Enter/Esc
  返回列表。菜单项激活先关菜单再 spawn，否则新窗口的焦点会被黑洞吃掉。
- 串口标记：`[wm] start menu: open items=7`、`select=<i> label=...`、
  `launch=<title>`、`close`、`toggle open|close`。

### 11.3 固定 syscall 23 `readdir`

- `kernel/modules/x86_64/syscall/fops.cpp::sys_readdir`：`GetMount()`
  解析挂载点，仅 ext4（其它返回 `-ENOSYS`），用 in-tree lwext4 的
  `ext4_dir_open` / `ext4_dir_entry_next` / `ext4_dir_close` 迭代，不额外持
  挂载点锁。
- 输出是文本行而不是二进制 struct（内核与用户态不需要共享布局）：
  `d|<name>\n` / `f|<name>\n` / `?|<name>\n` + 结尾 NUL；`.` / `..` 与
  `inode==0` 空洞由内核过滤；`cap` 上限 64 KiB，缓冲不足时截断到完整行；
  返回写入字节数（不含 NUL）或负 errno。完整 ABI 见 `docs/syscall.md`。
- 配套改动：`syscall.h` 声明、`kernel.syscall.file` 组件导出槽 23、
  kmod 固定契约表 23 从 `reserved` 改为 `readdir`、
  `tests/kmod/syscall_test.cpp` 契约断言、用户头 `SYSCALL_READDIR 23`。
  固定契约表 0..31 至此没有保留空洞。

### 11.4 Explorer 功能

`programs/explorer/main.cpp` 是与 Settings 同构的 WM client（0x400000 协议页
+ 共享键盘环 + `CLIENT_READY`）：

- `load_dir()` 一次 syscall 拿到目录项，解析后目录优先 + 名称升序排序，
  非根目录在最前插入 `..`；文件大小由用户态 `fopen`+`fsize` 按需查询
  （内核枚举路径不额外 open inode，避开 ext4 临界区）。
- 界面：路径栏、图标色块 + 名称、右列文件大小、选中高亮、滚动窗口、
  底部快捷键与状态行；窄窗口（contentH < 240）退化为单行提示。
- 操作：Up/Down/PgUp/PgDn/Home/End 选择，Enter/Right 进入目录或预览文件，
  Backspace/Left 返回上级，R 重新枚举，Esc 在预览时返回列表、列表时退出。
- 文件预览：读前 2 KiB，按 CR/LF 拆最多 12 行；ELF/含 NUL 的文件显示
  `(binary file - <size>, text preview skipped)`，不做乱码预览。
- 兼容降级：内核较旧（槽 23 仍 reserved，返回 `-ENOSYS`）时自动改读
  `/mp/files.index`，界面仍可用，状态栏标记 `source=index`。
- 串口标记：`[explorer] boot` / `start content=WxH` /
  `readdir: call path=...` / `readdir path=... rc=... entries=N source=...` /
  `enter dir=...` / `up path=...` / `preview path=... bytes=... lines=...` /
  `exit`。

### 11.5 构建与集成

- `programs/Makefile`：`make -C explorer` + `e2cp explorer.elf disk.img:/`；
  `clean` 同时清理 Explorer 产物。
- `res/scripts/build_os_pkg.py` 的 `PROGRAM_ELFS` 早已包含 `explorer`
  （此前缺程序，Setup 包会跳过该文件），现在 `make os-pkg` 会把它打进
  `/mp/system/programs/explorer.elf`（包从 28 变为 29 个 entry）。
- 镜像拷贝坑（实测）：Explorer 是新增文件，`e2cp` 覆盖一个刚由 `e2cp`
  新建的文件时，第二次写入把 inode 截成 0 字节，内核 `sys_load` 随后
  返回 `-EINVAL`（`FSize < sizeof(Elf64_Ehdr)`），桌面表现为"菜单点了没反应"
  且没有任何窗口日志。`programs/Makefile` 因此对 `explorer.elf` 先
  `e2rm` 再 `e2cp`；`loader.cpp` 也补了各失败点的串口诊断
  （`[wm] spawn: sys_load(...) rc=...`），便于下次一眼定位。
- 开始菜单不需要新程序，改动全部在 `desktop/main.cpp` 与 `loader.cpp`
  （后者补了 spawn 失败诊断日志）。

### 11.6 运行验证

```powershell
python -u res/scripts/test/start_explorer_smoke.py --wait 180 --action-wait 35
```

脚本用 headless QEMU + HMP `sendkey` 驱动，断言：桌面 ready → Ctrl+Alt+B
打开菜单（截图差异）→ Enter 启动 Explorer → readdir 真实枚举
（`source=syscall`，rc ≥ 0）→ Enter 进目录 → Backspace 回 `/mp` → Esc 退出
→ 菜单可重开/Esc 关闭 → 无 panic/STALL/#PF。截图：
`start_explorer_{base,menu,explorer,enter}.ppm`，串口：
`start_explorer_serial.log`。

实测（2026-11-13，最终构建）：

```
START MENU / EXPLORER SMOKE: PASS (0 warning(s))
A1..A8 / E1..E2 全部 PASS；E1 diff cells=2662，E2 diff cells=6774
[explorer] readdir path=/mp rc=<bytes> entries=... source=syscall
[explorer] enter dir=/mp/lost+found / up path=/mp / exit
0 panic、0 STALL WATCHDOG、0 #PF
```

宿主契约测试 `make -C tests -B kmod && ./tests/bin/kmod_test`：
`KMOD-TEST RESULT PASS 6/6`（固定契约表 23=`readdir` 通过）。

> 运行前建议：`wsl e2fsck -fn disk.img` 应显示文件系统干净。调试期间
> 反复 `e2cp` 覆盖/删除曾把 ext4 元数据写坏（未连接 inode / 文件内容
> md5 不一致），内核会报 `LOAD FILE NOT ELF!` 甚至触发 STALL WATCHDOG；
> 用 `e2fsck -fy disk.img` 修复后重新灌入两个 ELF 即恢复。绝不要在 QEMU
> 正在使用 disk.img 时执行 e2cp/e2rm/e2fsck。

### 11.7 已知限制

- `readdir` 只支持 ext4 挂载；`/mp` 之外的 FAT/SAF 目标返回 `-ENOSYS`。
- 一次调用最多 64 KiB 文本输出；极端大目录会被截断到完整行（Explorer 上限
  96 项，超出部分不显示）。
- 文件名按字节序排序（不做 Unicode 规整），路径拼接只做 `/` 归一化，
  没有 realpath / 符号链接解析；`..` 不会越过 `/mp` 挂载点。
- 预览只读前 2 KiB，不支持编辑/另存；打开文件到其它应用（如 Notepad）仍需
  走各自的静态文件对话框，Explorer 目前不发送"打开方式"命令。
- 开始菜单项表是编译期常量；新增应用要同时改 `kStartItems[]` 与
  `SKY_START_ITEMS`（二者由 `static_assert` 之外的代码约定保持一致）。



## 12. 输入延迟：量化 + 局部重绘（P0-2，2026-11）

### 12.1 量化手段

`lib/include/base/prof.h`（纯 header，static inline，不新增 libsky 导出）：

* `sky_rdtsc()` —— 用户态直接读 TSC（与桌面主循环同一时基）；
* `sky_tsc_per_ms()` —— 优先取内核用 HPET 校准过的值
  （`SysInfo.reserved[0]`，与 `programs/desktop/main.cpp` 的 probe 一致），
  取不到时用 `SYSCALL_MONOTONIC_MS` 现标 50 ms 窗口；
* `SkyLat` —— 32 样本环形缓冲，满了算 p50/p99 再清零。

`notepad` / `explorer` 在事件循环里记录两项（每 32 个"按键批次"打一行到串口）：

```
[notepad]  input_lat  n=32 p50=<us> p99=<us>   # 事件取出 -> 渲染完成
[notepad]  render_lat n=32 p50=<us> p99=<us>   # 纯绘制耗时
[explorer] input_lat / render_lat ...
```

> p99 说明：32 个样本的 99 分位就是窗口最大值，读作"最差一批"，不要当成
> 真正的 1/100 分位。
>
> 为什么用 TSC 而不是 `SYSCALL_MONOTONIC_MS`：实测后者刻度与真实时间不
> 一致（同一次点击序列里它报 3735 ms，真实约 0.37 s），拿它做时间窗判断会
> 直接判错（Explorer 的双击窗口就踩过）。

探针：`res/scripts/test/input_latency_probe.py`。它 Ctrl+Alt+N 聚焦 Notepad →
铺 10 行文本 → 注入字母键；再 Ctrl+Alt+B → Enter 拉起 Explorer → 注入 40 个
方向键，然后从串口日志里取最后一组 p50/p99。

### 12.2 优化前的开销来自哪里

* 每个按键批次都整页重绘：Notepad 560×380 内容区一次 ~850 KB 填充 +
  每个非空行一次 `TTF_DrawText`；Explorer 还多一列右对齐文件大小（每行一次
  `TTF_GetTextSize`）。
* 更贵的是字形栅格化：字体是 `TTF_ReadFontGlyfLazy()` 按需加载 glyf，
  一个字形在 TCG 下约 0.2~2 ms（冷/热缓存差 10 倍）。所以"画了多少字形"
  才是真正的成本函数，而不是"填了多少像素"。

### 12.3 改法

* Notepad：`render_band(y0, y1)` 按像素行带重绘 —— 只清掉变化的行、
  只重画与行带相交的行 + 光标 + 状态条。损伤行带由"旧光标行 ∪ 新光标行"
  决定；行数变化（Enter/行首退格/Delete 合并行）时才重画到文本区末尾；
  弹窗（Ctrl+O / Ctrl+W）是模态覆盖层，仍走整页重绘。
* Explorer：把"布局 + 滚动收敛"抽成 `exp_layout()`，绘制按行带裁剪；
  选中移动只重画"旧选中行 + 新选中行"两条带 + 页脚带；滚动位置变化时重画
  整个列表区（否则会留下整体位移的残影）；换目录/预览/重载走整页重绘。
  不要用 `min(old,new)..max(old,new)` 把中间行一起重画 —— 一批按键可能
  一次移动好几行，而中间行的内容没变，重画它们等于白栅格化几十个字形
  （实测 p50 会因此翻 2~3 倍）。
* 两个程序都保留了 `SKY_FULL_REDRAW` 编译期开关（0/1），用同一份源码编出
  "优化前/优化后"两个二进制跑同一个探针，A/B 数据才可比。

### 12.4 实测（QEMU TCG，`-smp 4 -m 2048M`，40 键批次，Notepad 铺 10 行文本）

| 客户 | 指标 | 优化前 | 优化后 |
|---|---|---|---|
| Notepad | render p50 | 15.2 / 20.9 / 27.2 ms（中位 20.9） | 15.5 / 17.0 ms（中位 ~16）；热缓存批次可到 **0.17 ms** |
| Notepad | render p99 | 51 / 55 / 67 ms | 49 / 73 ms |
| Explorer | render p50 | 298 / 305 / 363 / 529 ms（中位 ~334） | 113 / 117 / 139 / 184 / 248 ms（中位 ~139） |
| Explorer | render p99 | 497 / 663 / 708 ms | 323 / 346 / 472 / 580 ms |

* `input_lat` 与 `render_lat` 基本重合（差值 < 1%），说明这段延迟几乎全部
  是绘制本身，不是调度/队列。
* Explorer 约 2.4 倍（中位 334 ms → 139 ms）；Notepad 的收益强依赖
  "文档里有多少非空行"和字形缓存是否热：单行文档下两条路径要画的字形数量
  一样，所以看不出差别；多行文档 + 热缓存时可到亚毫秒。
* 方差很大（同二进制 run-to-run 可差 3 倍），主因是 TCG 下的字形栅格化
  成本 + 宿主负载，所以上表给的是多次运行的中位/区间，不是单次数字。

### 12.5 已知限制 / 下一步

* Explorer 滚动时仍然重画整个列表区（15 行 ≈ 375 个字形）；把列表改成
  "行缓存位图 + 位移拷贝"可以再降一个数量级。
* 真正的杠杆是字形缓存（按 (font,size,codepoint) 缓存已栅格化的位图），
  现在每次绘制都要重新走 glyf 解析；这条属于渲染层重构，未在本次范围内。
* 客户端是轮询 `sys_yield()` 的事件循环，按键批量大小取决于当时负载；
  延迟指标应理解为"这一批按键从被取出到画完"的时间，不含内核中断到入环的
  时间（`KbdEvent` 没有时间戳）。

## 13. Explorer / 开始菜单的鼠标操作（P1，2026-11）

### 13.1 协议：WM -> 客户端 鼠标投递

`lib/include/graphic/winstyle.h` 的客户端协议页从 16 槽扩到 21 槽，新增
WM 写 / 客户端读的鼠标事件区（与 [10..15] 的命令邮箱方向相反）：

| 槽 | 名称 | 含义 |
|---|---|---|
| [16] | `SKYWIN_PROTO_MOUSE_SEQ` | 每投递一个事件 +1（客户端据此判断有新事件） |
| [17] | `SKYWIN_PROTO_MOUSE_X` | content-local X |
| [18] | `SKYWIN_PROTO_MOUSE_Y` | content-local Y |
| [19] | `SKYWIN_PROTO_MOUSE_BTN` | 按下位图：bit0 左键、bit1 右键、bit2 中键 |
| [20] | `SKYWIN_PROTO_MOUSE_WHEEL` | 累积滚轮档数（当前恒 0，见限制） |

语义：
* 只投给指针所在的那个客户端（`wm_client_push_mouse()`），不做全局广播；
* 只在指针位于内容区（body 去掉标题栏与圆角行）时投递，标题栏/边框/窗口
  外的点击不投递（那些是 WM 自己的交互）；
* 坐标用 `WmClient.bodyX/bodyY/bodyW/bodyH` 现算，所以拖动/缩放/最大化后
  坐标自动跟随几何；`seq` 最后自增（release 语义），客户端看到变化时其余
  四个槽一定已写好；
* 30 Hz 重活循环里只在"位置或按键变化"时投递，纯移动日志最多 2 Hz。

串口打点：`[wm] mouse->client pid=<pid> x=<x> y=<y> btn=<n>`（按键沿必打）。

### 13.2 Explorer 的鼠标行为

* 左键单击：选中该行（`[explorer] mouse click idx=N name=... dt=<ms>`）。
* 双击（700 ms 内同一行）：打开该条目 —— 目录进入、文件预览
  （`[explorer] mouse open idx=N name=... dt=<ms>`）。
  窗口取 700 ms 而不是常见的 500 ms：WM 以 30 Hz 采样鼠标，press/release 各
  占一个采样点，加上门禁 HMP 注入的间隔，500 ms 在 TCG 下太紧。
* 键盘完全保留：Up/Down/PgUp/PgDn/Home/End/Enter/Backspace/R/Esc 全部
  仍然可用，且与鼠标共用 `g_sel`/`g_scroll`。
* 开始菜单支持鼠标 hover 高亮（指针停在哪个条目就把 `g_startSel` 移过去，
  与键盘 Up/Down 共用同一状态）。

### 13.3 门禁

`res/scripts/test/start_explorer_smoke.py` 在原 A1..A8 / E1..E2 之外新增
鼠标阶段（用 HMP `mouse_move` + `mouse_button` 注入真实 PS/2 事件）：

```
PASS M0)  Start menu reopened for mouse phase
PASS M0b) Explorer relaunched
PASS M0c) Explorer mounted again
PASS M1)  click delivered to the client        click idx=1, [wm] mouse->client seen
PASS M2)  double click opens the entry         open idx=1 (same as click: True)
PASS M3)  keyboard Esc still exits Explorer    [explorer] exit x2
PASS M4)  no panic / STALL / #PF after mouse input
START MENU / EXPLORER SMOKE: PASS (0 warning(s))
```

注入细节：HMP 的 `mouse_move dx dy` 对 PS/2 是相对增量，所以脚本先把指针
用几次 `-200,-200` 顶到 (0,0)，再以 ≤120 px 的步长走到目标；两次点击之间不能
插截图（一次 `screendump` 要 1.5 s，会超出双击窗口）。

E1（"菜单帧与基准帧不同"）也从"固定等 1.5 s 后截一次"改成轮询直到帧变化
（≤12 s）：菜单由 30 Hz 重活循环合成，启动阶段还要摆放 Notepad/Settings 两个
窗口，固定等待会在快机器上偶发拍到"菜单还没上屏"的那一帧。

### 13.4 已知限制

* 滚轮没做：本内核 PS/2 鼠标报文只解出左/中/右三键
  （`lib/include/graphic/mouseshare.h` 的 `ps2_mouse_state_t` 没有 wheel 字段），
  协议槽 [20] 已预留但恒为 0。要支持滚轮需要先在内核鼠标驱动里解第 4 字节。
* 客户端只在事件循环里轮询 `mouse_seq`，最坏延迟 = 一次渲染时间（见第 12 节）。
* 只有 Explorer 实现了鼠标交互（选中/双击/滚动列表）；Notepad 的"点击定位
  光标"、标题栏双击最大化等还没接。
* 开始菜单 hover 只改选中项，不做 hover 动画/背景高亮渐变。

## 14. 偶发内核页错误（VMA 哨兵页 UAF，2026-11 复核发现）

### 14.1 现象与复现率

QA 复跑 `start_explorer_smoke.py` 16 次，1 次整机 halt（~6%）。崩点固定出现在
鼠标阶段的第二次 Explorer 启动（PID 14，第一次启动 + A1..A8 都正常）：

```
[explorer] start content=640x412 cwd=/mp
KERR Segfault: unmapped (addr=0000000000000008 rip=FFFFFFFF8002E2F3)
KERR Kernel exception caught: Page fault.  CR3=... CR2=0x8  On thread 14
KERR SYSTEM HALTED (hcf)
```

`addr2line` 定位到 `rb_insert_raw()`（`kernel/include/klib/algorithm/rbtree.h`），
调用链 `VMM::Alloc() → VMA::InternalAlloc() → rb_insert()`。

### 14.2 根因：`CleanPM()` 把 VMA 节点池的一整页还给了 PMM

VMA 节点不是 `kmalloc` 出来的，而是 `vma.cpp` 自带的按页切分池：

```c
// vma_alloc_node()
void *pg = PMM::Request();              // 一整页 4 KiB
base = HIGHER_HALF(pg);
n = PAGE_SIZE / sizeof(vma_region_t);   // ≈ 50 个节点
for (i = 1; i + 1 < n; i++) base[i].next = &base[i + 1];
g_node_free_list = &base[1];            // 除 base[0] 外全部进**全局**空闲链表
return &base[0];
```

也就是说：同一页里的 50 个节点会被分给 50 个不同的 pagemap。而 pagemap 的
哨兵 (`vma_head`) 正是通过 `vma_alloc_node()` 拿到的一个节点，于是
`CleanPM()` 里那行

```c
PMM::Free(PHYSICAL(pm->vma_head));   /* ← 整页还给 PMM */
```

等于把这个进程和别的 pagemap 共用的那一页整个释放掉。进程退出（Explorer
按 Esc）后，该页很快被重新分配成页表页/用户页/别的 VMA 节点，于是别的
pagemap 正在用的 VMA 节点的 `parent/left/right/color` 被写成任意值；下一次
`rb_insert` 的修复循环读到"parent 是红色、parent->parent 是 NULL"，
解引用 `grandparent->left` 就在 CR2=0x8 上内核 #PF → `SYSTEM HALTED`。

为什么是"偶发"：需要 (a) 有进程退出、(b) 它释放的那一页恰好被立刻重用、
(c) 受影响的是正在插入节点的 pagemap。门禁里"Esc 关掉第一个 Explorer →
立刻拉起第二个"正好是这个窗口，所以 1/16 命中。

### 14.3 修复

1. `vma.cpp` 新增 `VMA::ReleaseSentinel(pagemap)`：只把哨兵节点还给池
   （`vma_free_node`），不再整页 `PMM::Free`。池页永不单独归还 PMM，池的大小
   由峰值 VMA 数量决定（有界）。
   `CleanPM()` 相应改为 `VMM::VMA::ReleaseSentinel(pm)`。
2. `sys_sysinfo()` 补锁：它是唯一"改 VMA 树却不持 `pm->vma_lock`"的 syscall
   路径（`InternalAlloc` + `AddRegion` + `RemoveRegion`）。而 VMA 树的 rb 内部锁
   在本内核里是空操作（`rb_root_init` 传的 lock/unlock 回调全是 `nullptr`），
   唯一同步手段就是 `pagemap->vma_lock`。现在映射/释放两条路径都在锁内完成
   VMA 操作，且不把 `vma_region_t*` 带出临界区（松手后可能已被摘链/合并），
   释放路径改成"先拆 PTE → shootdown → 再在锁内重新查一次并摘链"
   （`ShootdownFull` 要等其他 CPU 的 ACK，绝不能在持锁时调用）。
3. `rb_insert_raw()` 加防御：`grandparent == NULL` 时把 parent 涂黑后收敛，
   不再解引用空指针——一棵坏树不应该打死整机。正常情况下永远走不到这里；
   走到说明某处仍然在写别人的节点（这就是本次的哨兵页 UAF）。
4. 顺手修掉一个确定的泄漏（P2）：`SpawnWindowedApp()` 成功路径从不
   `sys_munmap`，只有失败回滚才有；于是每开关一次窗口，桌面 pagemap 就永久
   多出 2 个 VMA + 一整块共享页引用（640×460 ≈ 1.2 MB）。
   - `wm_client.cpp::wm_drop()` 里新增 `wm_unmap_shared()`（surface 按
     `w*h*4` 对齐到页 + 协议页 1 页），并在 `RegisterWindowAuto` 失败路径也回滚；
   - 控制台窗口销毁时用 `main.cpp::wm_console_release()` 做同样的事，并把
     `place.desk_surf/proto` 清 0（主循环后面还会轮询 `place.proto` 与
     `place.desk_surf`，因此 4 处用到这两个别名的地方都补了判空）。

### 14.4 验证

| 门禁 | 结果 |
|---|---|
| `start_explorer_smoke.py`（含 A1..A8 / E1..E2 / M0..M4 鼠标阶段）| **7 次连跑全 PASS**（0 warning）|
| `client_churn_gate.py --cycles 10` / `--cycles 15`（同一 boot 内反复开关窗口）| **PASS**（10/10、15/15 全部 spawn→exit→reap，0 panic/STALL/#PF）|
| `boot_stability_5x.py` | **PASS 5/5** |

`client_churn_gate.py` 是为这个 bug 新增的放大器：单次启动只采样一次
"进程退出"窗口，而它在一个 boot 里做 N 次 spawn→exit，把 UAF 的命中概率放大
N 倍，同时也覆盖新加的 munmap 路径。

### 14.5 遗留

* `kernel/src/arch/aarch64/vmm/init.cpp` 里还有同样的 `PMM::Free(PHYSICAL(pagemap->vma_head))`
  （非 x86_64 路径，本次未验证、未改）。
* 门禁脚本的双击相位也修了 flaky：两次点击之间不再夹"等串口标记/截图"
  （一次 screendump 就 1.5 s，远超 700 ms 双击窗），先连点两下再一起等标记。

### 14.6 补充：控制台窗口关闭路径也验证了

`wm_console_release()` 只在 hw2 进程退出时才走到，门禁里的 Explorer 客户端走的是
`wm_client.cpp` 的 `wm_unmap_shared()`，所以另做了一次真实鼠标验证：

1. HMP 注入鼠标点任务栏的 "Skyline Console" pill → 控制台置顶；
2. 点控制台标题栏关闭按钮 (body 左上 (330,200) + 关闭键中心 ≈ (927,216))；
3. 断言：`[wm] child pid=` 出现 1 次（hw2 被杀并回收）、桌面仍响应
   （随后 Ctrl+Alt+B 能正常打开开始菜单 = 1 次）、串口里
   `kernel panic|stall watchdog|#PF|kernel fault|SYSTEM HALTED|Segfault: unmapped`
   0 命中；截图对照：控制台窗口消失、任务栏 pill 少一个、时钟与壁纸完好、
   没有残留黑块。

另外把 munmap 也加到了「点关闭按钮」分支（原来只等 child-exit 事件），
`wm_console_release()` 幂等（别名清 0 后再调是 no-op），所以两条路径都安全。

## 15. 打字"看得见"的延迟：客户端帧提交 OUT_SEQ（2026-11 复核）

### 15.1 现象（与第 12 章的区别）

第 12 章量的是客户端把字符画进自己共享 surface 的耗时；用户实际抱怨的是
"按下去到 Notepad 上看见字"的延迟。两者不是一回事：修复后客户端内部
`input_lat` 已经到微秒级，但屏幕仍然几百毫秒才变。

新增 `res/scripts/test/display_latency_probe.py`：HMP 注入一个键，轮询真实
`screendump` 直到 Notepad 文本行出现新墨迹，测的是端到端上屏延迟。
初版探针（下称旧探针）以"窗口区域像素 hash 变化"为判据，修复前实测
（6 样本）：p50 413.8 ms / p99 447.6 ms。

> 数值适用范围（QA 复核后补充）：413.8/447.6 ms 与早期记录的
> "修复后 115–179 ms" 都出自同一支旧探针，只能作同探针的相对对比。
> 旧探针缺 `screendump` 完成同步、含固定 +20 ms settle、没有逐键基线，
> 绝对数值不能当作可信的上屏时间。15.4 给出修正探针重采的绝对值，
> 15.6 列出整改项。

### 15.2 根因

* `lib/include/graphic/winstyle.h` 的 `SKYWIN_PROTO_OUT_SEQ`（槽 9）是
  "客户端产出新帧" 的唯一通知。`wm_clients_poll_output()`
  (`programs/desktop/synthesizer/wm_client.cpp`) 与控制台镜像路径
  (`programs/desktop/main.cpp` 的 `last_mirror_seq` / `last_client_seq`)
  都只在它变化时才搬运 surface 并置 `wmDirty`。
* 但 `programs/` 下 没有任何客户端递增过 OUT_SEQ：Notepad 源码注释甚至写着
  "OUT_SEQ stays 0"。Explorer / Settings 同样。
* 结果：客户端画完没人通知 WM，只能等 `clock_gap = 500ms` 的 2 Hz 任务栏
  时钟重绘顺带整屏合成 —— 这就是几百毫秒，与实测 414 ms 吻合。

### 15.3 修复

1. `programs/notepad/main.cpp` 的 `render_band()`、`programs/explorer/main.cpp`
   的 `render_band()`、`programs/settings/main.cpp` 的 `render()` 末尾各加一次
   `__atomic_add_fetch(&q[SKYWIN_PROTO_OUT_SEQ], 1, __ATOMIC_RELEASE)`；
   release 语义保证像素写入先于序号对 WM 可见。
   P2 补修（QA 复核发现）：Notepad 的 `render_full()` 原来是
   `render_band(0, contentH)`（内部会递增 OUT_SEQ）→ 再 `note_draw_dialog()`
   画模态层，WM 有可能在弹窗像素写完之前就合成一帧（Ctrl+O/W/S 弹窗晚一帧），
   所以早期"所有绘制路径的最后一步都是帧提交"并不成立。现把帧提交抽成
   独立的 `commit_frame()`，`render_band(y0, y1, commit)` 显式带参；
   `render_full()` 用 `commit=false` 画文本带 → 画模态层 → 最后
   `commit_frame()`。三个 `render_band()` 调用点全部显式传参（2 处 `true`、
   `render_full()` 内 1 处 `false`），打字主路径行为不变。
2. 新增 `wm_clients_new_frame()`（只读探测、不更新 `last_seq`，更新留给
   `wm_clients_poll_output()`）。桌面主循环在 30 Hz 重活限速之前检查；
   有新帧时本轮跳过限速直接走完合成。16 ms 冷却把 DOOM 之类连续渲染客户端
   的新帧合成限到 ~60 Hz，避免重活占满 CPU 又把鼠标采样拖涩。
3. 鼠标投递块整体移到重活限速之前：原来 30 Hz 采样会把双击的第二个按下沿
   合并掉（M2 门禁 2/6 假失败）。每轮只做几次整数比较，不变化就不写协议页。
4. 清掉 `wm_client.cpp` spawn 末尾一行 unreachable 的重复日志。
5. `notepad/main.cpp` 在 READY 之前逐字栅格化常用 ASCII 字形：字形缓存是
   进程内的，新窗口冷缓存会让第一个按键才惰性加载 glyf —— 实测首键上屏
   568 ms，而稳态只有 115 ms。预热把冷成本挪到用户开始打字之前（画在
   (4,4) 单字符不越界，随后 `render_full()` 清屏覆盖），首键降到 167 ms。

### 15.4 实测（QEMU TCG -smp4 -m2048M）

两组数据用途不同，不能混用：旧探针只用于同探针 A/B 相对对比；
修正探针给出可信的绝对上界。

| 指标 | 旧探针（相对对比用） | 修正探针（保守上界，绝对值） |
|---|---|---|
| 修复前 p50 / p99 | 413.8 / 447.6 ms | 未重采（需回退二进制重建镜像） |
| 修复后稳态 p50 | 115–179 ms | **52.4 ms / 114.5 ms**（两次独立运行） |
| 修复后稳态 p99 | 212–272 ms | **135.2 ms / 156.0 ms** |
| 冷启动 warmup max | 568 ms → 167 ms（字形预热） | **128.9 ms / 158.3 ms** |
| Notepad 内部 `input_lat`（20 行） | — | p50 5 µs / p99 155 µs |
| 端到端 `e2e_lat`（内核入环→画完，20 行） | — | p50 195 µs / p99 408 µs |
| TTF 字形缓存命中率 | — | 99 %（`glyf fetch=36`） |

修正探针 `display_latency_probe.py` 的关键改动（对应 15.6 的 B-1..B-6）：

1. 自动定位文本行：先发一个校准键 `'a'`，取整帧 diff 的 bbox 并排除
   底部 60 px 任务栏（2 Hz 时钟）与状态栏，得到紧条带
   `(416,268,692,300)`；支持 `--box` 覆盖。
2. 正向确认：只有条带内出现局部新墨迹簇（`new_ink >= 16` 且
   宽 <= 64 px、高 <= 38 px）才算上屏。实测命中簇恒为 `2×17 px` 的 2 px
   光标条 —— `render_band()` 与字形同一次绘制出来的
   `DrawFillRect(..., 2, ph, CARET)`。这是"本次按键的重绘已上屏"的正向
   证据，不是字符 OCR。
3. 逐键基线：每次 `sendkey` 前重新取一帧完整截图作为基线。
4. 完成同步：PPM 先截为 0，再从 P6 头算出精确完整长度
   `16 + 3*1280*800 = 3,072,016 B`，写满才解码；不信任 HMP 回复
   （上一轮迟到的回复会误"确认"半写文件，第一版修正就因此复现出
   2 ms 的假快样本）。
5. 时间戳：`upper = dump 完成 − 按键发出` 是严格上界（含一次 PPM 写
   与 QEMU HMP 调度延迟）；`lower = 相邻 dump 发出时刻中点` 只是宽松下界。
   两次运行 `lower` p50 从 2.3 ms 跳到 42.5 ms，说明 HMP 调度不可控，
   结论只用 `upper`。
6. 同一次运行并行记录 legacy 判据；两次运行 `legacy − strict` 均为
   +0.0 ms，三种判据在同一帧同时命中（见 15.6）。

判据改为机制判定：`strict upper p50<350 / p99<450 / warmup max<450 ms`，
把"帧提交已接通"与 2 Hz 时钟兜底（~500 ms）分开；不再沿用旧探针标定的
绝对 SLA。两次运行 `RESULT: PASS`，探针轮询开销 86.5–89.8 ms/次。

脚本与镜像：`start_explorer_smoke.py` / `client_churn_gate.py` 的随机端口
改为 bind+listen 预检（原来偶发落到 Windows 保留段，QEMU 直接退出、
表现为 rc=1 且无任何断言输出）；串口日志改用截断而非 `os.remove`
（Windows safe-delete 会拦截删除导致旧数据污染断言），并在 `finally` 里
把含 `STALL / #PF / panic` 的串口自动另存为
`*_fatal_<时间戳>_<pid>.log`。`input_latency_probe.py` 另修三处 bug：
单捕获组被当字符串取 `[0]`（61→6）、`drive_keys` 读到 prefill 旧统计行
提前返回、Ctrl+S 被键盘环淹没后重试。

回归（P2 补修 + 探针整改后重跑，5 次 QEMU 串行、无并发）：
`display_latency_probe --label after-final` PASS；`start_explorer_smoke` ×3
连续全 PASS（M1/M2/M3/M4 全绿、0 warning）；`client_churn_gate --cycles 20`
20/20 PASS（C1..C6 全绿、0 STALL/#PF/OOM）；`e2fsck -fn disk.img` 干净；
`disk.img:/notepad.elf` 与本地 md5 一致，`os.pkg` LZ4 roundtrip self-test OK。
`boot_stability_5x` 5/5 PASS 为此前结果（本轮未改内核，未重跑）。

关于"高负载串跑偶发 STALL"：证据链失效，结论已降级

早期版本在此处写过：串跑 smoke×3 + boot5x 后 churn 第 8 轮出现
`STALL WATCHDOG: CPU 0 waiting on lock=... (acquired at FFFFFFFF80050606)`，
`addr2line` 指向 `file_cache_tick` (`fc_idle.cpp:389`)，其它 CPU 仍打印
`[perf]`，判为单核锁饥饿。QA 复核指出这一段现在无法复核：
`client_churn_serial.log` 已被后续"单独 churn 10 / 0 STALL"那轮按
`open(...,"wb")` 截断覆盖；全仓库 `*.log` 与 `C:/ZSY/qa_verify/` 下
找不到含 `file_cache_tick` 或该锁地址的存档。仓库里确实有 4 份 11-08 的
旧日志含 `STALL WATCHDOG`，但格式是
`CPU N has not run for 3000 ms (reported by CPU M)`，属另一类早期现象，
与本条无关，不能拿来充当证据。

因此本条降级为当时未留证的运行观察：机制归因本身合理
（`fc_idle.cpp:389` 确为 `file_cache_tick()` 里的 `spinlock_lock(&s->lock)`），
但不作为可复核结论；`file_cache` 后台回收锁饥饿仍单独排期。本次回归链
（probe + smoke×3 + churn20 串行）未复现，`*_fatal_*.log` 归档 0 份、
各门禁 fatal 命中 0。今后任何一轮只要出现 fatal，串口会被自动另存，
不会再被下一轮覆盖。

### 15.5 剩余瓶颈（未做，需架构级改动）

* 整屏 `Compositor::Compose()` 在 QEMU TCG 下约 85–100 ms，是剩余 ~100 ms 的
  主体。`ComposeRows()` 是单线程行带路径，对客户端这种覆盖半屏的窗口更慢
  （实测主循环 30~38 it/s → 13~16 it/s，见第 12 章），故未启用。要把上屏压到
  < 50 ms 需要多线程行带合成或客户端 surface 直贴 framebuffer。
* 修正探针每次 `screendump` 的实测开销升到 86.5–89.8 ms（旧探针是
  34.8–45.5 ms）：现在必须等 PPM 写满 3,072,016 B 才解码，不再读半写文件。
  这个开销完全计入 `upper` 上界，所以 15.4 的绝对值是保守的；真实硬件
  （非 TCG）预计显著更低。
* `upper` 与 `lower` 之间仍有约 50–90 ms 的未分离区间，来源是 QEMU HMP
  命令从"发出"到"被主循环处理"的调度延迟（两次运行 `lower` p50 从 2.3 ms
  跳到 42.5 ms 即为此）。TCG 下无法用宿主线程把它扣干净；若要更精确，
  需在 guest 内打时间戳或加 trace 点，而不是继续在 HMP 侧估算。
* `programs/installer/main.cpp` 是静态一次性界面，未加帧提交（无感知需求）。

### 15.6 QA 只读复核（2026-11-14）与整改

独立 QA 对本章做了一次只读复核，结论 `REVIEW: FAIL`：产品代码与文档一致、
未发现代码回归；FAIL 全部落在测量方法学与证据链上。逐项处理：

| # | QA 指出的问题 | 处理 |
|---|---|---|
| A | 5 项产品修复的位置与 release 语义均与文档一致 | PASS，无需改 |
| A-P2 | `render_full()` 的帧提交早于模态层绘制 | 已修，见 15.3-1 |
| B-1 | "任意像素变化"近似**自证循环**：修复后重绘更频繁，判据更早触发，读数被系统性压低 | 改为文本行**局部新墨迹簇**正向确认；同一次运行并行记录 legacy 判据以量化偏差 |
| B-2 | 没有每次按键前重取基线，0.15 s 空闲后的合成会造出假快样本 | 每次 `sendkey` 前重新做一次完整截图并计算基线 |
| B-3 | `shot()` 只 sleep 6 ms 就读 3 MB PPM，可能读到半写文件或上一帧 | 先把 PPM 截为 0，再从 P6 头算出精确完整长度（3,072,016 B），写满才解码；**不信任 HMP 回复**——上一轮迟到的回复会误"确认"半写文件，第一版修正就因此复现出 2 ms 假快样本 |
| B-4 | `t0` 落在含固定 20 ms settle 的 `cmd()` 之前 | `send_key()` 无 settle，`t0` 取 `sendall()` 之后 |
| B-5 | 裁剪框 `(300,200,1100,650)` 硬编码 | 由校准键 `'a'` 的整帧 diff 自动定位文本行（实测 `(416,268,692,300)`，排除任务栏 2 Hz 时钟与状态栏），并支持 `--box` 覆盖 |
| B-6 | 脚本 docstring 写 `p50<150 / p99<250`，代码实际是 `220/320` | 统一为机制判据 `strict upper p50<350 / p99<450 / warmup<450` |
| C | 高负载 STALL 证据链失效：串口日志被后续 PASS 轮覆盖 | 无法恢复，15.4 已降级为"未留证的运行观察"；三个门禁新增 `archive_fatal()` 自动另存 |

一个反直觉的实测结果：QA 预判 B-1 会让"修复后"读数偏低；但补齐
B-2/B-3 后，两次运行的 `legacy − strict` 都是 +0.0 ms —— 三种判据
（固定大框任意像素 / 紧条带任意像素 / 局部新墨迹）在同一帧同时命中。
旧探针 115–179 ms 与修正探针 52–115 ms 的差距主要来自反方向偏差：
半写或过期 PPM 被 `frame_hash()` 的 `except` 静默跳过（多等一轮约 40 ms）
加上固定 +20 ms settle。这不否定 B-1 的风险（修复前后重绘频率不同确实会
破坏 A/B 等 bias），结论是：A/B 的相对方向仍有效，绝对值以 15.4 的
修正探针重采值为准。
