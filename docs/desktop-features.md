# 桌面安装程序、Settings 与 Notepad 文件对话框

本文记录桌面可用性增强：**Installer 安装程序**、桌面 **Settings / Lua 配置加载界面**、
以及 Notepad 的 **Open / Close 文件对话框**。三者都运行在现有 WM client 模型上，
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

主题脚本只使用预置的 **`wallpaper` FrameBuffer 变量**（`width/height/format` +
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

命令通过协议页 **client -> WM 命令邮箱**传递（`lib/include/graphic/winstyle.h`）：

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
  **Ctrl+Alt+S / Ctrl+Alt+N / Ctrl+Alt+I** 分别聚焦 Settings / Notepad /
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

1. **AHCI 轮询在 ext4 临界区内开中断**：`Port::Read/Write` 曾 `sti` 后轮询，
   而 ext4 挂载点锁是 `spin_lock_irqsave` 的 CPU 递归锁；中断开启后线程可能
   被抢占/迁移，owner 判定失效，导致 ext4 bcache 并发访问、`ext4_buf_free`
   invalid free 或 CPU 卡死。修复：轮询期间不再 `sti`
   （`kernel/modules/builtin/common/drivers/ahci/ahci.cpp`）。
2. **ext4 挂载锁改为线程 owner + 可让出等待**：`ext4.cpp` 的递归 owner 从
   “CPU id” 改为 “当前线程指针”，等待者在开着中断的状态下 `Schedule::Yield()`
   而不是关中断自旋；这样即使持锁线程等磁盘 I/O，其他 CPU 的 TLB shootdown /
   定时器/调度也不会被饿死（之前表现为 STALL WATCHDOG 连锁）。
3. **大文件不进 file_cache**：`sys_fread` 对 `file_size > 1 MiB` 跳过块级
   `file_cache_promote`（DOOM WAD、字体等）；小文件（程序/配置/主题）仍缓存。
   这规避了实测 DOOM 启动阶段 `file_cache_promote` 的 invalid free（缓存条目
   生命周期竞态），同时显著降低大文件读取的内存/淘汰 churn。
4. **键盘 Esc 映射**：`kernel/include/drivers/keyboard/x86/keyboard_map.h` 的
   `kb_map_keys[1]`（scancode 0x01）原为 0，Esc 被输入核心丢弃；改为 `0x1B`
   后 Installer/Settings/Notepad 的 Esc 关闭/取消路径才真实可用
   （kbdshare 注释约定 Esc 携带 ASCII 0x1B）。注意 kernel 构建对头文件依赖
   不完全，改完该头文件后需要 touch `keyboard.cpp` 或 clean build。

QEMU 最终验证：`desktop_features_smoke.ps1` **PASS, 0 warning, 0 STALL,
0 panic**；覆盖 Installer 首装、Esc 关闭、Settings 自动刷新 + Mint 切换、
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

- console：caption 最小化按钮与任务栏 pill **两条路径**都保存 `wm_prior`
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

QEMU `-m 64M` 的可用内存只有约 **56MiB**（Limine/固件保留）。实测结论：

| 配置 | 结果 |
|---|---|
| 默认 2G | Notepad/Settings 自动常驻，DOOM 由 Ctrl+Alt+D 按需启动 |
| 64M：Notepad + Settings 自动启动、DOOM 延迟 | 稳定，OOM≈13-16 行，无 panic；桌面应用可用 |
| 64M：DOOM_TINY + trimmed WAD | DOOM 可 init 并持续渲染（zone=8MiB，OOM≈25–26，0 panic/STALL，init 后余 ~5–6MiB） |
| 64M：DOOM + Notepad/Settings 同时 | 56MiB 不足，PMM OOM；DOOM 采用按需/独占 |

策略：DOOM **不在启动路径自动拉起**（其 R_Init/WAD 冷读会把桌面 ready 推迟
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
**SkylineSystem Setup**：在保留旧 6 个主题/配置资源安装（旧 QEMU 门禁
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

- LZ4 是标准 **block** 编码（无 frame 头），每块 ≤64KiB；
- Python 构建器带独立解码器 roundtrip 自检；C 安装器有 ~75 行 LZ4 block
  解码器，按块解压后 `fwrite`，**不把整包读入内存**。

当前包（默认不含 doom.wad）：

```
entries=28, compressed=5,460,395 B, raw=8,908,221 B
内容：
  boot/kernel                      (kernel ELF)
  boot/modules/*.ko                (15 个 kmod 演示/系统模块)
  boot/limine/{limine.conf,limine-bios.sys,BOOTX64.EFI}
  config/desktop.lua
  lib/libsky.so
  programs/{desktop,notepad,settings,installer,doom,hw,hw2}.elf
```

`--with-doom-wad` 可把 IWAD 也打进包（raw ≈20MB）；默认不打包专有 IWAD，
安装后 DOOM 仍从 `/mp/doom.wad` 读取。

### 10.2 内核 mkdir（syscall 22）

安装器需要建 `/mp/system/boot/modules` 等目录树：

- 固定 syscall 槽 **22** 从 `reserved` 改为 `mkdir`：
  `sys_mkdir(path, mode) -> 0 / -errno`；
- `kernel/modules/builtin/x86_64/syscall/fops.cpp::sys_mkdir`：
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
   `[setup] os install complete: 28/28 files, compressed=5,460,395 B,
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
[setup] os install complete: 28/28 files ... failed=0
[setup] manifest written: /mp/system/os.manifest (937 bytes)
```

截图：`setup_os_installer.png`（Setup 窗口 + 安装日志）。

### 10.6 边界（重要）

- Setup 安装的是 **完整 OS 系统树到 ext4 `/mp/system`**（kernel/modules/
  bootloader 文件/程序/库/配置），不是从零分区、写 MBR 与
  `limine bios-install` 的“裸机装系统”流程；当前内核/用户态没有原始块设备
  写接口，bootloader 安装仍由构建期 ISO/HDD 镜像负责。
- 目标盘是 QEMU 的 ext4 `disk.img`；FAT/SAF 目标返回 `-ENOSYS`。
- 默认不复制专有 `doom.wad` 和可选 TTF 字体；需要时用
  `build_os_pkg.py --with-doom-wad` 或安装后手动 e2cp。
- 重复运行 Setup 会覆盖 `/mp/system` 文件（ext4_dir_mk 幂等，文件 wb 截断）。

## 11. 开始菜单 + Explorer 文件浏览器（2026-11）

本轮给桌面补上"从任务栏启动应用"与"真正浏览文件系统"两条能力。此前
`docs/desktop-features.md` 第 7 节记录的 **"没有目录枚举 syscall，新增文件需
更新 files.index"** 限制就此解除。

### 11.1 组成与入口

| 模块 | 位置 | 入口 |
|---|---|---|
| 开始菜单 | `programs/desktop/main.cpp`（WM 内嵌 overlay） | 任务栏最左按钮点击 / **Ctrl+Alt+B** |
| Explorer | `programs/explorer/`（动态 WM client） | 菜单首项 / **Ctrl+Alt+E** |
| readdir | 内核固定 syscall **23** | `syscall(23, path, buf, cap, flags)` |

菜单项（7 条，固定顺序）：Explorer、Notepad、Settings、Installer / Setup、
DOOM、Console、About...。启动时**不自动拉起 Explorer**（与 DOOM 相同的
低内存策略），只在用户点菜单/按快捷键时 spawn。

### 11.2 开始菜单（WM 自绘 overlay）

- 任务栏最左侧固定 46px 开始按钮（四格窗口标记，不依赖字体）；
  `TB_LEFT_MARGIN` 已改为 `按钮左 + 按钮宽 + gap`，药丸布局与命中测试共用
  同一个宏，不会出现"画在 A、点在 B"的漂移。
- 菜单是一个 **WM 独占的 ARGB 窗口**（300×376，`RegisterWindowAuto` → 最高的
  auto layer），没有客户端进程与协议页；surface 由桌面 `malloc`，用
  `BasicDraw` 画 SDF 圆角 + 描边，条目文字用 `console_font()`。
- 打开/关闭：`start_menu_open` / `start_menu_close` 注册/注销窗口、
  重画任务栏高亮、`Compositor::ForceFullCompose()`；关闭菜单时点击外部会被
  本次 press 吞掉，不穿透到下层窗口。
- **键盘焦点黑洞**：菜单打开期间 `wm_refocus()` 把 `focus_owner` 钉在 WM
  自己的 reader slot，所有客户端都收不到键；关闭时把其它 reader 的
  `consumer[]` 游标推进到当前 `head`，丢弃菜单期间堆积的事件。否则旧焦点
  窗口会"补读"菜单的 Enter/Esc —— 实测过 Settings 把菜单的 Enter 当成
  "应用主题"、Esc 直接退出，这个坑必须显式处理。
- 导航：Up/Down/Home/End 选择、Enter 启动、Esc 关闭；About 页面 Enter/Esc
  返回列表。菜单项激活**先关菜单再 spawn**，否则新窗口的焦点会被黑洞吃掉。
- 串口标记：`[wm] start menu: open items=7`、`select=<i> label=...`、
  `launch=<title>`、`close`、`toggle open|close`。

### 11.3 固定 syscall 23 `readdir`

- `kernel/modules/builtin/x86_64/syscall/fops.cpp::sys_readdir`：`GetMount()`
  解析挂载点，**仅 ext4**（其它返回 `-ENOSYS`），用 in-tree lwext4 的
  `ext4_dir_open` / `ext4_dir_entry_next` / `ext4_dir_close` 迭代，不额外持
  挂载点锁。
- 输出是**文本行**而不是二进制 struct（内核与用户态不需要共享布局）：
  `d|<name>\n` / `f|<name>\n` / `?|<name>\n` + 结尾 NUL；`.` / `..` 与
  `inode==0` 空洞由内核过滤；`cap` 上限 64 KiB，缓冲不足时截断到**完整行**；
  返回写入字节数（不含 NUL）或负 errno。完整 ABI 见 `docs/syscall.md`。
- 配套改动：`syscall.h` 声明、`kernel.syscall.file` 组件导出槽 23、
  kmod 固定契约表 23 从 `reserved` 改为 `readdir`、
  `tests/kmod/syscall_test.cpp` 契约断言、用户头 `SYSCALL_READDIR 23`。
  固定契约表 0..31 至此没有保留空洞。

### 11.4 Explorer 功能

`programs/explorer/main.cpp` 是与 Settings 同构的 WM client（0x400000 协议页
+ 共享键盘环 + `CLIENT_READY`）：

- `load_dir()` 一次 syscall 拿到目录项，解析后**目录优先 + 名称升序**排序，
  非根目录在最前插入 `..`；文件大小由用户态 `fopen`+`fsize` 按需查询
  （内核枚举路径不额外 open inode，避开 ext4 临界区）。
- 界面：路径栏、图标色块 + 名称、右列文件大小、选中高亮、滚动窗口、
  底部快捷键与状态行；窄窗口（contentH < 240）退化为单行提示。
- 操作：Up/Down/PgUp/PgDn/Home/End 选择，Enter/Right 进入目录或预览文件，
  Backspace/Left 返回上级，R 重新枚举，Esc 在预览时返回列表、列表时退出。
- 文件预览：读前 2 KiB，按 CR/LF 拆最多 12 行；ELF/含 NUL 的文件显示
  `(binary file - <size>, text preview skipped)`，不做乱码预览。
- **兼容降级**：内核较旧（槽 23 仍 reserved，返回 `-ENOSYS`）时自动改读
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
- **镜像拷贝坑（实测）**：Explorer 是新增文件，`e2cp` 覆盖一个刚由 `e2cp`
  新建的文件时，第二次写入把 inode 截成 **0 字节**，内核 `sys_load` 随后
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
打开菜单（截图差异）→ Enter 启动 Explorer → **readdir 真实枚举**
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

> **运行前建议**：`wsl e2fsck -fn disk.img` 应显示文件系统干净。调试期间
> 反复 `e2cp` 覆盖/删除曾把 ext4 元数据写坏（未连接 inode / 文件内容
> md5 不一致），内核会报 `LOAD FILE NOT ELF!` 甚至触发 STALL WATCHDOG；
> 用 `e2fsck -fy disk.img` 修复后重新灌入两个 ELF 即恢复。**绝不要在 QEMU
> 正在使用 disk.img 时执行 e2cp/e2rm/e2fsck**。

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

