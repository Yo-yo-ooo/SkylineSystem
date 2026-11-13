# DOOM 极限压缩模式（DOOM_TINY）

目标不是把 1993 年 DOOM 的完整 PC 数据塞进 2MB/384KB，而是把 **SkylineSystem
上的 PureDOOM port** 按 RP2040/Pico 移植的思路做到可测量的最简：

- 帧缓冲“撒谎”：多个 screen 指针指向同一块；
- 去掉 RGBA 全屏副本，直接把 8-bit palette 索引转 ARGB 写 WM surface；
- 删除非核心功能（audio、wipe 转场、save/load、截图、后屏）；
- 用 WAD trim + ADPCM 工具减少数据体积；
- 用最小稳定 zone 在 64MiB QEMU 目标机运行。

> 诚实边界：原版 DOOM 的关卡几何/纹理/物理数据与 fixed_t 未做有损压缩；
> 本移植仍从 ext4 上的 `/mp/doom.wad` 读取，而不是把 IWAD 编进固件。
> 因此这是“在 Skyline 的 64MiB 目标机上把 DOOM 固定开销压到最低”，
> 不是 RP2040 的 384KB 运行环境本身。

## 1. 实现（`programs/PureDOOM/`）

`DOOM_TINY` 默认在 `programs/PureDOOM/Makefile` 为 **1**；普通模式：

```sh
make -C programs/PureDOOM DOOM_TINY=0
```

### 1.1 帧缓冲别名（“撒谎的帧缓冲”）

`PureDOOM.h::V_Init()`：

- 普通：`I_AllocLow(320*200*4)` → `screens[0..3]` 4 块各 64KiB；
- tiny：只分配 **1 块 64KiB**，`screens[0..3]` 全部别名到同一地址；
  `screens[4]`（ST 状态栏 10KiB）仍由 ST_Init 单独分配。

代价与处理：

- wipe “流血”转场需要 screens[2]/[3] 同时存在 → tiny 下 `D_Display`
  短路 wipe，切关直接切屏（`is_wiping_screen` 不进入）；
- save/load 使用 screens[1] → tiny 下菜单 Save/Load 置灰，`M_SaveGame/
  M_LoadGame/M_QuickSave/M_QuickLoad` 拒绝，F2/F3/F6/F9 同路径；
- 状态栏后屏 `R_FillBackScreen` / `WI_slamBackground` 短路，避免别名脏画；
- `I_InitGraphics` 不再第二次分配 screens[0]；
- `M_SizeDisplay` 拒绝并强制 `screenblocks=11` 全屏，避免 screens[1] 后屏
  别名污染；HUD 状态栏在 tiny 下隐藏（菜单/游戏画面仍可用）。

固定节省：**4×64KiB → 1×64KiB = 192KiB**。

### 1.2 去 RGBA 全屏副本

普通路径 `doom_init()` 分配：

- `screen_buffer` 64KiB（palette 索引，用于 crosshair 等）；
- `final_screen_buffer` 256KiB（320*200*4 RGBA）。

tiny 路径：

- 两者都不分配；`doom_get_framebuffer(1)` 直接返回 `screens[0]`；
- 新增 `doom_get_palette()` 暴露 768B 实时 palette；
- shim `blit_frame` 每帧建 256 项 ARGB LUT，直接从 8-bit 索引映射到
  WM 共享 surface（nearest-neighbour fill-scale，支持主题圆角导致的
  640x392 非精确 2x content）。

固定节省：`screen_buffer` 64KiB + `final_screen_buffer` 256KiB + 第二次
screens[0] 64KiB = **384KiB**。

### 1.3 合计固定内存

| 项 | 普通 | DOOM_TINY | 节省 |
|---|---:|---:|---:|
| screens[0..3] | 256 KiB | 64 KiB | 192 KiB |
| I_InitGraphics screens[0] 重复 | 64 KiB | 0 | 64 KiB |
| screen_buffer | 64 KiB | 0 | 64 KiB |
| final_screen_buffer RGBA | 256 KiB | 0 | 256 KiB |
| ARGB LUT（新增） | 0 | 1 KiB | -1 KiB |
| **合计** | **640 KiB** | **65 KiB** | **575 KiB** |

`doom.elf`：tiny 1,639,200 B vs 普通 1,697,328 B（`-g3` 下差 ~58KB）。

### 1.4 非核心功能剥离

tiny 下额外：

- audio 全剥离：`I_InitSound` 不预缓存 sfx，`S_StartSoundAtVolume/
  S_ChangeMusic/S_UpdateSounds` 直接返回；这是允许 WAD 删除 `DS*/D_*/DP*`
  的前提（否则 `W_GetNumForName("dspistol")` / `d_<song>` 会 I_Error）；
- screenshot 禁用（无写文件路径）；
- 16 位类型审计：`_Static_assert` 冻结 screen 尺寸/64KiB 可寻址假设；
  **fixed_t/angle_t/坐标/WAD offset 未改**（改了会破坏物理/渲染），
  没有做危险的全局类型收缩。

## 2. WAD trim / ADPCM 工具

`programs/PureDOOM/tools/wadtrim.py`（host Python，`make -C programs/PureDOOM wadtrim`）：

### 2.1 默认 trim

解析 IWAD header/directory，删除非核心音频 lump：

- music `D_*` 45 个；
- sound `DS*` 122 个；
- PC speaker `DP*` 122 个；
- 重写合法 WAD header + 目录 + lump 数据，保持其余 lump 顺序/名字。

真实 `doom.wad` 统计：

```
原大小  : 11,159,840 B
删除    : 289 lumps / 2,083,092 B
输出    :  9,071,551 B (81.29%)
节省    :  2,088,289 B (1.99 MiB)
verify  : OK (1905 lumps, audio_left=0)
```

关键 lump 校验：PLAYPAL/COLORMAP/DEMO1/E1M1/S_START/S_END/F_START/F_END/
TEXTURE1/PNAMES 均存在；目录/哈希 roundtrip OK。

### 2.2 ADPCM（RP2040 思路的可审计实现）

`--adpcm`：对保留的 DMX v3 音效做 4-bit IMA-ADPCM、每 256B block 自描述
（`"IMA1" + nsamples + len`）。真实数据：

```
122 个 DMX sfx: 1,222,616 B -> 620,982 B
vs 16-bit PCM: 3.93:1
vs DMX8      : 1.97:1
```

默认部署**不启用 ADPCM**：DOOM 已无音频驱动且 tiny 已剥离音频，保留的
WAD 不含音频；该工具/格式用于量化“如果保留音频，ADPCM 能省多少”，
并可作为后续完整音频 WAD 的第一步。

### 2.3 不能省的部分

- 关卡几何、纹理、sprite、patch、flat、colormap 等渲染/物理数据；
- `fixed_t` 物理精度；
- WAD 仍从 **ext4 文件流式读取**（不是 RP2040 的 XIP/flash 内存映射）。

原因：把 9–11MB WAD 编进 ELF 会把它整体搬进 RAM，反而更差；RP2040 的
XIP 是硬件映射，Skyline 用户态没有等价的安全 file mmap。若未来加
只读 WAD mmap 或 per-lump LZ4 容器，还能进一步降低磁盘/页缓存压力。

## 3. zone 策略与 64MiB 运行

`doom_main.c::sky_pick_zone_mb()`：

| 机器总内存（SysInfo） | zone |
|---|---:|
| ≤ 80 MiB（QEMU -m64M 实际 ~56MiB） | 8 MiB（DOOM_TINY + trimmed WAD；10–12MiB 为普通模式档） |
| ≤ 128 MiB | 10 MiB |
| ≤ 192 MiB | 12 MiB |
| > 192 MiB | 12 MiB（上游默认） |

实测（旧内核、完整 WAD、非 tiny）：12MiB 可跑但 OOM 数千；9MiB R_Init
卡死；8MiB 不稳定；**10MiB 是最小稳定档**。在 DOOM_TINY（固定 buffer
省 575KiB）+ WAD trim 后，**8MiB 连续三次通过 64MiB 测试**（init done +
心跳 + 0 panic/STALL，OOM≈26 行，init 后仍余 ~5MiB），故低内存档最终
取 8MiB；普通/大内存仍 10–12MiB。

Skyline 低内存启动策略：`<80MiB` 时自动启动 **Console + DOOM**，
Notepad/Settings 延迟（Ctrl+Alt+N/S 可再拉起），把 56MiB 优先给 DOOM。
`Ctrl+Alt+D` 始终可 raise/restore/spawn DOOM。

QEMU 验证（`res/scripts/test/doom_compact_smoke.ps1`）：

- 2G：`[doom] compact=1 screens_kb=64 final_rgba_kb=0 screen_copy_kb=0`，
  `zone_mb=12`，`init done`，心跳；截图见 `doom_compact.png`。
- 64M：`-MemMB 64` → `[doom] zone_mb=10`，`mem_free_mb=24`（zone 前），
  `mem_free_mb_post_init=0`，**init done + 心跳持续**，OOM≈145 行，
  0 panic / 0 STALL；截图 `doom_compact_64m.ppm`（由 gate 生成）。
- WAD trim 后 `[doom] wad bytes=9071551 -> 9071551 (trim file=-1)`；
  没有 mirror（正常 VFS 流式读）。

## 4. 与 RP2040 传奇的对应关系

| RP2040/Pico 技术 | Skyline 对应实现 | 是否达成 |
|---|---|---|
| 帧缓冲别名 | screens[0..3] 单块别名 | ✅ 省 192KiB |
| 音效 ADPCM-XQ | wadtrim `--adpcm` IMA-ADPCM 4:1 工具 | ✅ 量化工具（默认音频剥离） |
| 32→16 位变量 | 审计 + screen 尺寸静态断言；不动 fixed_t | ⚠️ 仅审计/安全约束，不做全局有损收缩 |
| 去文件 I/O/网络 | 无音频/网络；WAD 仍 VFS 流式 | ⚠️ 仅非核心模块 |
| WAD 编进固件/XIP | 未做（无用户态 file mmap；编进 ELF 反而更耗 RAM） | ❌ 有明确工程理由 |
| 384KB RAM 运行 | 64MiB QEMU 下 DOOM 可运行，固定开销 65KiB + 10MiB zone | ❌ 不是同一硬件极限 |

## 5. 复现

```sh
make -C programs/PureDOOM                  # tiny 默认
make -C programs/PureDOOM DOOM_TINY=0      # 普通对照
make -C programs/PureDOOM wadtrim          # 生成 doom_trim.wad

# QEMU gate（先构建 ISO/disk.img 并放入 doom.elf/doom.wad）
powershell -File res/scripts/test/doom_compact_smoke.ps1
powershell -File res/scripts/test/doom_compact_smoke.ps1 -MemMB 64
```

串口关键标记：

```
[doom] compact=1 screens_kb=64 final_rgba_kb=0 screen_copy_kb=0
[doom] zone_mb=... mem_total_mb=... mem_free_mb=...
[doom] wad bytes=... -> ... (trim file=...)
[doom] init done
[doom] mem_free_mb_post_init=...
[doom] content_w=... content_h=... exact_2x=...
[doom] f=... ms=... seq=...
```
