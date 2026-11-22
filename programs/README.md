<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# programs/

用户态程序。内核只负责加载 ELF 和提供 syscall，界面、窗口管理和终端都在这里。

| 目录 | 内容 |
|---|---|
| `desktop/` | 桌面进程：窗口管理器、多线程软件合成器、开始菜单、Explorer 入口、内嵌 Lua 主题，自带控制台窗口 |
| `notepad/` | 文本编辑器：Ctrl+O 打开、Ctrl+S 保存、Ctrl+W 关闭对话框 |
| `settings/` | 主题与设置面板，Enter 让桌面立刻加载选中的 `.lua` 主题 |
| `installer/` | Setup：读 `/mp/os.pkg`（SKYOS1 容器 + LZ4），把整套系统解到 `/mp/system` |
| `explorer/` | 文件浏览器，用 syscall 23 `readdir` 真枚举 ext4 目录 |
| `dos/` | DOS 相关程序，说明见 [docs/dosvm.md](../docs/dosvm.md) |
| `helloworld/`、`helloworld2/` | 最小 ELF，用来验证加载器和 syscall 通路 |
| `PureDOOM/` | DOOM 移植，DOOM_TINY 压缩后 64MiB 内存也能跑 |

`bin/`、`object/` 是构建产物，不进版本库。产物由顶层 Makefile 打进 `disk.img` 或 `os.pkg`。
