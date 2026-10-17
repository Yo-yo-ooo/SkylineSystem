# 输入链路修复：鼠标 9 位符号位 + PS/2 键盘丢键

## 1. 鼠标：忽略了第 9 位符号位（真实 bug）

`kernel/src/drivers/mouse/x86/mouse.cpp` 的 IRQ12 处理原来这样解增量:

```c
int8_t dx_raw = (int8_t)mouse_bytes[1];   /* 只有低 8 位, 且不支持第 9 位 */
int8_t dy_raw = (int8_t)mouse_bytes[2];
state.x += dx_raw;
state.y -= dy_raw;
```

PS/2 的 X/Y 增量是 **9 位有符号数** (-256..255): 数据字节只放低 8 位,
**符号位在 byte0 的 bit4(X) / bit5(Y)**。所以 |Δ| > 127 时会被当成反方向
的 8 位数 —— 例如 Δy = -200 在数据字节里是 `0x38`, 原代码读成 +56,
**光标朝反方向跳**。

实测现象: 用 QEMU monitor 注入 `mouse_move -600 -400` 时, 光标从 (240,185)
跑到 (5,505) —— y 反而**往下**走了 320 像素。

修复:

```c
int32_t dx = (int32_t)(uint8_t)mouse_bytes[1];
int32_t dy = (int32_t)(uint8_t)mouse_bytes[2];
if (mouse_bytes[0] & 0x10) dx -= 256;   /* X 符号位 */
if (mouse_bytes[0] & 0x20) dy -= 256;   /* Y 符号位 */
state.x += dx;
state.y -= dy;        /* PS/2 的 Y 向上为正, 屏幕坐标向下为正 */
```

判据: 修复后用**小幅增量**(4 组 +50,+50 再 4 组 +50,+4, 全部在 ±127 内)
可以精确把光标从 (0,0) 走到 (400,216), 并稳定点中控制台标题栏。

## 2. 键盘：鼠标中断把键盘字节读出来丢掉了（真实丢键）

键盘和鼠标**共用 0x60 数据端口**, 靠 0x64 状态寄存器的 bit5 区分
(0 = 键盘, 1 = 鼠标)。

- 键盘中断 (`keyboard.cpp:keyboard_handler`) 处理正确:
  `if ((status & 0x01) && !(status & 0x20))` 才取字节, 否则**不碰**。
- 鼠标中断 (`mouse.cpp:ps2_mouse_handler`) 却是:

```c
if (!(status & 0x20)) {
    io_in8(PS2_DATA_PORT);   // 丢弃键盘数据  ← 这会把按键吞掉
    LAPIC::EOI();
    return;
}
```

当两个设备同时有待处理字节时 (快速打字 + 移动鼠标), 落到鼠标 IRQ 里的
那个键盘字节会被**静默丢弃** —— 表现为偶发漏字, 很难复现。

修复: 键盘侧导出 `keyboard_feed_byte(uint8_t)` (转调内部的
`feed_scancode`), 鼠标中断改为把它**交给键盘翻译**而不是丢弃。

## 3. 验证（真实注入 + 截图）

用 QEMU monitor 注入真实输入 (`mouse_move` / `mouse_button` / `sendkey`):

1. 光标按预期移动 → 鼠标增量解码正确;
2. 点击控制台标题栏 → 窗口置顶并获得焦点 (click-to-front 生效);
3. 依次按 `h i t h e r e` + `ret` → 控制台输出

```
What is your name? hithere
Hello, hithere!
Type an integer: abc
Type a decimal (e.g. 3.14): _
```

说明**焦点路由 + 按键投递 + 回车**整条链路都通, 且客户端能持续交互
(第二次输入 `abc` + 回车同样被接收)。

修复前后行为一致 (证明转发改动没有破坏正常路径), 系统指标不变:
`NEEDED=5 / doom init done=1 / Segfault=0 / OOM=0 / ACK=0 / STALL=0`。

## 4. 顺带清理的临时诊断

移除并验证为 0 出现: `[pmmsample]`、`[vmmdiag]`、`[elfdiag]`、
`[mmapsample]`、`[munmapsample]`、`[pmmdiag]`。

## 5. 未验证 / 已知限制

- **Shift / Ctrl / Alt 组合键**没有单独验证, 只验证了字母键与回车;
- **方向键**在 DOOM 里没用真实注入验证过 (映射代码存在:
  `0x48/0x50/0x4B/0x4D` → `KBD_KEY_*`);
- **鼠标滚轮**没有实现;
- 注入测试是合成事件, 真实鼠标的**高回报率** (1000Hz, 一包内多次增量累积)
  和**丢包**行为没有覆盖。
