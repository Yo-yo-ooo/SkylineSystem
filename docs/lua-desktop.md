# 桌面内嵌 Lua（desktop.lua）：脚本绘制壁纸 + 可编程窗口样式

> 目标：桌面（`programs/desktop`）内嵌 Lua 5.4.7 解释器，启动时执行
> `/mp/desktop.lua`。**壁纸完全由脚本直接画进 wallpaper framebuffer**（C++ 只提供
> framebuffer 原语，不解析渐变/颜色等主题参数）；窗口 chrome 通过受控
> `sky.set_window_style` 配置。Lua 运行时不开放 io/os/package 等能力。

## 1. 组成

| 部分 | 位置 | 说明 |
|---|---|---|
| Lua 运行时 | `programs/desktop/lua/` | vendored Lua 5.4.7（上游 MIT，见 `LICENSE`）；只编译 core + base/string/table/math/utf8/coroutine/debug 库，不启用 io/os/package |
| 宿主胶水 | `programs/desktop/lua_glue.cpp` | 创建 `lua_State`、注册 `sky.*`（含 wallpaper framebuffer 绘制原语）、加载脚本、错误回退、串口日志 |
| 运行期窗口主题 | `programs/desktop/sky_style.{h,cpp}` | 窗口 chrome 颜色/圆角/阴影；默认值来自 `lib/include/graphic/winstyle.h`。壁纸不在此配置 |
| 壁纸缓冲 | `programs/desktop/main.cpp` 的 `wallBuf` | `SkyLuaInit(wallBuf, w, h)` 把该 framebuffer 暴露给脚本绘制；之后作为合成器 layer 0 |
| 默认脚本 | `programs/desktop/desktop.lua` | 构建时 `e2cp` 到用户盘 `/mp/desktop.lua`；缺失时用 `lua_glue.cpp` 内嵌兜底脚本 |
| 用户态 libc 补充 | `lib/stdc/` | Lua 需要的标准 C API（setjmp/longjmp、locale、time、math、stdio、string 等），宿主测试见 `tests/stdc/` |

## 2. 执行时序

```text
desktop main()
  → framebuffer / wallpaper buffer 分配
  → SkyLuaInit(wallBuf, scrW, scrH)
       SkyStyleReset()                       // 编译期默认窗口视觉
       把 wallBuf 清成黑场，并作为预置变量 `wallpaper`（= `sky.wallpaper`）的绘制目标
       luaL_newstate + 注册受限库；建立 `wallpaper` FrameBuffer 变量（addr/宽高/pitch/绘制方法）
       尝试 luaL_loadfile("/mp/desktop.lua")，失败 → 内嵌脚本
       执行脚本：
         · 通过 `wallpaper` 变量绘制壁纸（任意内容；C++ 不做渐变解析）
         · sky.set_window_style{...} 设置窗口 chrome
       串口打印 source / 窗口样式 / 壁纸回读样本
  → 壁纸缓冲作为合成器背景层
  → 创建 console 与其它窗口，chrome 使用运行期 theme 绘制
```

脚本错误（语法/运行时）只回退默认主题并通过串口报告，不阻断桌面启动。

## 3. Lua API

### 3.1 预置变量：`wallpaper`（壁纸 FrameBuffer）

桌面在加载脚本**之前**就把壁纸 FrameBuffer 作为 Lua 变量建好：

- 全局变量 **`wallpaper`**；
- 同一 table 也可通过 `sky.wallpaper` 访问（二者指向同一对象）。

| 字段/方法 | 说明 |
|---|---|
| `wallpaper.addr` | `lightuserdata`，ARGB 像素首地址 |
| `wallpaper.addr_u64` | 同地址的整数形式（方便打印/调试） |
| `wallpaper.width` / `height` | 像素尺寸 |
| `wallpaper.pitch_pixels` / `pitch_bytes` | 行跨度（tight pitch，像素/字节） |
| `wallpaper.stride` | 行跨度字节数（= `pitch_bytes`） |
| `wallpaper.bpp` / `format` | `4` / `"ARGB8888"` |
| `wallpaper.fill(rgb)` | 整屏填充 |
| `wallpaper.hline(x, y, len, rgb)` | 水平线 |
| `wallpaper.vline(x, y, len, rgb)` | 垂直线 |
| `wallpaper.rect(x, y, w, h, rgb)` | 实心矩形 |
| `wallpaper.pixel(x, y, rgb)` | 单像素 |
| `wallpaper.size()` | 返回 `width, height` |

同时保留 `sky.wallpaper_size/info/fill/hline/vline/rect/pixel` 函数形式（与上述方法等价）。
所有壁纸像素都由脚本绘制；C++ 不清屏以外的任何主题解析。越界坐标自动裁剪。

**示例：直接用 `wallpaper` 变量画渐变**（默认 `desktop.lua` 即如此）：

```lua
local fb = wallpaper
local w, h = fb.width, fb.height
sky.log("wallpaper fb: " .. w .. "x" .. h .. " " .. fb.format ..
        " pitch=" .. fb.pitch_bytes .. " bpp=" .. fb.bpp)

local top, bottom = 0x102040, 0x7A2F6E
local tr, tg, tb = (top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF
local br, bg, bb = (bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF
local den = (h > 1) and (h - 1) or 1

for y = 0, h - 1 do
    local t = y / den
    local r = math.floor(tr + (br - tr) * t + 0.5)
    local g = math.floor(tg + (bg - tg) * t + 0.5)
    local b = math.floor(tb + (bb - tb) * t + 0.5)
    fb.hline(0, y, w, (r << 16) | (g << 8) | b)
end

-- 任意绘制都由脚本负责，例如：
-- fb.rect(0, h - 180, 96, 48, 0x1A2C55)
```

### 3.2 `sky.set_window_style{ ... }`

| 字段 | 范围/类型 | 说明 |
|---|---|---|
| `corner_radius` | 0..20 | 窗口圆角半径（超出 clamp；标题栏高度保持固定，协议不变） |
| `title_color` / `border_color` / `separator_color` | `0xRRGGBB` | 标题栏/外发丝/标题分隔线 |
| `paper_color` | `0xRRGGBB` | 窗口纸面（console 内容区域的 chrome 底色） |
| `ink_color` / `close_color` | `0xRRGGBB` | 标题文字/关闭按钮字形 |
| `shadow_alpha` | 0..100 | 软阴影强度百分比（100 = 原版 0.28） |

### 3.3 `sky.get_style()` / `sky.log(msg)`

- `get_style()` 返回当前窗口样式 table，便于脚本自检；
- `sky.log(msg)` 以 `[lua] ` 前缀写到内核串口（QEMU 无头门禁可见）。

## 4. 修改与验证

1. 修改 `programs/desktop/desktop.lua`（或镜像里的 `/mp/desktop.lua`）；
2. `make cm` / `make -C programs` 会把 `desktop.lua` 重新 e2cp 进 `disk.img`；
3. 重启 QEMU 后，桌面启动日志会输出 `source=file`、窗口样式与**壁纸回读样本**：
   `[desktop] lua: wallpaper: script-painted fb=... WxH top=0x... mid=0x... bottom=0x...`
4. 无头验证：

```powershell
pwsh -File res/scripts/test/lua_desktop_smoke.ps1 `
     -Root C:\ZSY\SkylineSystem -UseDisk -WaitSeconds 90 `
     -ExpectWallpaperTop 0x102040 -ExpectWallpaperBottom 0x7A2F6E `
     -ExpectTitleColor 0x232A45 -RequireSource file
```

脚本等待串口 `[desktop] lua:` 标记，截取 QEMU 屏幕（PPM）并采样壁纸/标题栏像素；
显式传入 `-ExpectWallpaperTop/Bottom` 时校验对应像素（默认脚本为线性渐变，采样点与
期望色一致），否则只检查壁纸非均匀/与窗口对比，不假设脚本画的是什么图案。
截图默认保存在 `C:\ZSY\SkylineSystem\lua_desktop.ppm`。

> 说明：脚本在桌面启动时执行一次；修改后重启生效。窗口几何（宽度/标题栏高度/客户端
> 协议页）保持固定，只开放视觉参数，避免客户端与合成器布局漂移。

## 5. 已知限制

- `libc` 的 `printf_` 不支持 `%a` 十六进制浮点输出，因此 `string.format("%a")` 与
  `tostring` 的非整数 `%q` 序列化不完整（`%.14g`/`%f`/`%e` 正常）；`strtod` 的十六进制
  浮点**输入**支持。
- 既有 `math.log()/exp()/pow()` 精度有限（历史实现；新增 `log2/log10` 是独立高精度核）。
- Lua 的 io/os/package 库未被编译/注册；`/mp/desktop.lua` 通过桌面宿主的
  `luaL_loadfile` 读取，而不是 Lua io 库。
- `time.h` 仅提供 `time/clock/difftime`，无 `struct tm/localtime/strftime/mktime`。
- 壁纸 framebuffer 的 raw lightuserdata 在 Lua 中不能直接解引用，只能通过
  `sky.wallpaper_*` 原语访问（避免脚本越界写任意内存）。
- 桌面动态链接的全局数据（`_ctype/stdout/stdin/stderr`）依赖内核动态加载器的
  `R_X86_64_COPY` 重定位；本实现同时修复了 COPY 从"目标对象自身"取源的 bug
  （见 `kernel/src/elf/elf_dyn.cpp`），否则 Lua 的 `string.format` 会因 `_ctype` 全 0
  报 invalid conversion specification。

## 6. 标准 C 依赖与测试

Lua 运行时用到而原用户态 libc 缺失的标准 C 接口已补齐（`lib/stdc/`）：

- 头文件/实现：`setjmp.h`（x86_64 setjmp/longjmp）、`signal.h`（`sig_atomic_t`）、
  `locale.h`（C locale）、`time.h`（`time`/`clock`/`difftime`）；
- `math.h`：`asin/log2/log10/sinh/cosh/tanh/frexp/ldexp/modf/HUGE_VAL(F)`；
- `string.h`：`strstr/strspn/strcspn/strrchr/strpbrk/strcoll/strerror/memchr/memcmp` 等；
- `stdio.h`：`BUFSIZ/stdin/stdout/stderr/fwrite/fflush/fprintf/fgets/feof/ferror/
  freopen/getc` 等；
- `stdlib.h`：`abort`。

宿主标准符合性测试：`tests/stdc/`（`cd tests && make -B stdc && ./bin/stdc_test`），
并接入 CI；QEMU 集成验证用第 4 节脚本。
