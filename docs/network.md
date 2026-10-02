# 网络栈（e1000 82574L + lwIP 移植）

> 目标交付文档。功能、验证证据与已知边界均如实记录。

## 组成

| 组件 | 位置 | 说明 |
|---|---|---|
| e1000 驱动 | `kernel/src/drivers/net/e1000.cpp` + `kernel/include/drivers/net/e1000.h` | 82574L/82540EM 兼容子集: 探测/复位/EEPROM MAC/256 RX + 256 TX 环/中断+轮询双模 |
| lwIP 胶水 | `kernel/src/drivers/net/e1000_lwip.cpp` | 零拷贝实验开关 + 拷贝生产路径、DHCP、ping 画像、多核 TX 洪泛 |
| lwIP 本体 | `kernel/src/net/**` (vendored) | NO_SYS=1 轮询模式 |
| 端口配置 | `kernel/include/lwip/lwipopts.h` + `cc.h` | 见下"移植修正" |
| 启动接线 | `init.cpp` NetStackInit (调度器安装后) | e1000 探测在 PCI 枚举 |

## 验证证据（全部在 serial.log 留有记录）

- **DHCP**: `[lwip] DHCP 成功: 10.0.2.15` + 掩码/网关
- **ping 全链路**: `[lwip][ping] reply from 10.0.2.2` 连续回复 (累计数百次)
- **多核 TX**: 3 核洪泛 2.2-2.7k 帧/s, tx满=0 丢=0 错=0
- **RTT 百分位**: 每 5 分钟 min/P50/P90/P99/max + 丢包
- **长稳**: 多段 10-55min 清洁 soak (0 断言 0 异常), 2h 收官 soak 见 goal-status.md
- **故障注入**: RX 前 5 帧 OOM 注入 → 丢包计数 + 自愈

## 移植修正（全部有 bug→fix 记录）

1. lwipopts: 软件校验和 (STM32 CHECKSUM_BY_HARDWARE 移除)、16B 对齐、pbuf 池、
   `LWIP_DHCP_DOES_ACD_CHECK=0` (slirp ARP 代理误判冲突)
2. cc.h: `LWIP_PLATFORM_DIAG` varargs 直传 (原实现经 assert 的 %s 转发丢失全部参数)
3. `sys_now`: RTC 日期编码 → PIT 单调毫秒 (跨日回绕会破坏超时数学)
4. 网络线程栈 32KB (`NewKernelThreadEx`): 默认 16KB 在 lwIP 深路径 + DHCP 处理
   下栈溢出 → 线程静默死亡 (曾被误判为多种竞态)
5. 驱动内 DMA 缓冲: 静态 BSS 的 GetPhysics 不可解析 → VMM::Alloc 页 + 页内偏移
6. 零拷贝历史: 索引地址差反推 (独立页不连续) + pbuf 双释放 + 探测期 PIT::Sleep
   挂死 —— 均已修复; 剩余嫌疑见 docs/dpdk-lite.md

## 已知边界（如实）

- **RX 收侧校验验证关闭** (lwipopts.h 注释): 发侧校验全开, 收侧验证路径
  丢弃 slirp 合法帧, 待与 lwIP 版本核对
- **零拷贝 = 实验态** (NET_ZEROCOPY=0): 生产路径为拷贝式, 已稳定验证
- **IRQ 模式已启用**（P3-80 修正：e1000.cpp 的 INTx 中断线 + IOAPIC 重映射
  已接线，IRQ 驱动收包；原"未启用/轮询模式"口径过时）; MSI-X 多队列 =
  82574 数据手册核对阻塞项
- **无 IPv6/TCP 实测**: 端口默认配置含 TCP/IPv6 编译, 未经真实流量验证
- 全部速率/时延数字 = QEMU TCG 客户机虚拟时间 (墙钟 ~3x 膨胀, 见 round 16 记录)

## 相关文档

- DPDK-lite 设计与零拷贝审计: `docs/dpdk-lite.md`
- 目标总账与遗留债: `docs/goal-status.md`
- 回归门禁: `tests/net_smoke.ps1` (QEMU 冒烟)
