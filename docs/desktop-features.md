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
- `/mp/files.index` 是静态清单（没有目录枚举 syscall）；新增文件需更新该 index。
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
