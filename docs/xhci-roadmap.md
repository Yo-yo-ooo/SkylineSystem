# xHCI 枚举深度路线图（C12）

> 审计 C12：xHCI 枚举深度（hub/HID 描述符/EP0 mps）为功能路线图，
> 连文档都未写。本文件建档（round 13）。

## 现状（已实现）

- 控制器初始化、MSI-X、命令环、事件环
- EP0 控制传输（同步路径，P1-32 的 IRQ 上下文阻塞问题已修）
- 端口复位/使能、设备地址分配（PORTSC W1C 已修 P1-24）
- 单设备枚举（QEMU xHCI 已验证）

## 缺口（按优先级）

| # | 缺口 | 影响 | 备注 |
|---|---|---|---|
| 1 | **hub 遍历**（外部 hub 下游端口未扫描） | 真机多级 USB 拓扑无法使用 | 需要 hub class 请求 + 下游端口枚举 |
| 2 | **HID Report Descriptor 读取** | hid.cpp 的报告 ID 剥离只能靠启发式（B7 已按 bInterfaceProtocol 收窄） | 读取 descriptor 后可精确解析 |
| 3 | **EP0 mps 协商**（max packet size 未按设备描述符调整） | 低/全速设备 EP0 用 8/64 而非 512 时传输错误 | Get Device Descriptor 后按 bMaxPacketSize0 更新 |
| 4 | 同步/异步 TRB 链式 SG（B5 异步路径） | 跨页 DMA 错误 | 异步走 bounce 或链式 TRB |
| 5 | 批量传输重试/流控、等时传输 | 音频/大容量设备 | 功能扩展 |

## 实施前提

- 真机 xHCI 控制器（QEMU 仅覆盖单设备路径）
- USB 2.0/3.x 规范中的 hub class 与 descriptor 章节

## 验证计划

真机可用后：多级 hub + 键盘/鼠标混合拓扑的枚举与热插拔回归。
