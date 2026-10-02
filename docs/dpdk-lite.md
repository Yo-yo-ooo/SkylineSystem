# DPDK-lite 网络数据面设计（e1000e 82574L + lwIP）

> 目标：把 lwIP 的数据面改造成"类似 DPDK"的高性能形态——轮询、每核私有
> 数据结构、批量处理、校验卸载。**不是 DPDK 复刻**（无用户态驱动框架/
> 无 hugepage/无 RSS 库），而是把 DPDK 的核心理念落到本内核架构上。

## DPDK 理念 → 本内核映射

| DPDK 概念 | 本内核现状 | 差距/计划 |
|---|---|---|
| **PMD 轮询驱动** | ✅ `E1000::PollRX()` 轮询收包（lwIP 线程每 5ms 拉取） | 中断模式下还需修 INTx 路由（MSI-X 计划） |
| **每核私有数据结构（无锁）** | ⚠️ RX FIFO 单生产者单消费者（IRQ→lwIP 线程）；TX 由 lwIP 线程独占 | 每核 pbuf 池 + 每核 TX 队列（计划） |
| **rte_mempool（预分配对象池）** | ⚠️ lwIP PBUF_POOL 32×2048（全局） | 改为 per-CPU pbuf 池（见下） |
| **批量收发（burst API）** | ❌ 单帧处理 | PollRX 批量出队 → 批量入栈（计划） |
| **零拷贝** | ❌ IRQ→FIFO 拷贝 + FIFO→pbuf 拷贝（2 次） | 接收缓冲直接作为 pbuf 池条目（计划，见下） |
| **校验和卸载** | ❌ 软件校验（发侧全开、收侧验证关闭） | e1000e TX context descriptor 卸载（需 82574 数据手册核对寄存器布局） |
| **多队列 + MSI-X 分核** | ❌ 单 RX 队列（82574 支持 2+2） | MRQC/RXDCTL1 + MSI-X 表（计划，同样需寄存器核对） |
| **rte_ring（无锁环形队列）** | ⚠️ 简单环形 FIFO（spinlock-free 单生产者） | 已具雏形；多生产者需 CAS 版本 |

## 每核 pbuf 池设计（下一步）

```
per_cpu:
  pbuf_pool[MAX_CPU][PBUF_POOL_PER_CPU]     // 每核 64×2048B = 128KB/核
  pbuf_free_head[MAX_CPU]                    // 每核空闲链 (单消费者 = 本核)
```
- 分配：本核池取；空 → 全局池（自旋锁）。
- 释放：**释放回本核池**（DPDK 同款：对象留在产生它的核，跨核归还走慢路径）。
- lwIP 改动点：`PBUF_POOL` 分配走自定义 `pbuf_alloc` 钩子（`LWIP_MEM_ALIGN` +
  自定义 memp 无法直接替换——用 `PBUF_POOL` 的自定义分配宏或改 `pbuf_alloc`
  的 POOL 分支）。

## 零拷贝接收（计划）

当前路径：NIC DMA → RX 缓冲页 → FIFO 拷贝 → pbuf 拷贝 → 协议栈（2 次拷贝）。
零拷贝方案：**RX 缓冲页即 pbuf 池页**——PollRX 直接把描述符缓冲挂成
`PBUF_REF`（引用外部内存的 pbuf），协议栈消费后由驱动回收描述符并重新
DMA。lwIP 需要 PBUF_REF + 自定义回收回调（`pbuf_free_custom`）。
风险：协议栈持有 pbuf 期间描述符必须停用（回压）；82574 单队列下
RX 停顿成本 = 描述符数 256 个 × 2048B = 512KB 缓冲深度，可接受。

## 校验和卸载（计划）

e1000e TX 路径：发送**上下文描述符**（cmd=0x04，含 IPCSS/IPCSO/TUCSS/TUCSO）
+ 数据描述符（cmd 含 IFCS），硬件计算 IP/TCP/UDP 校验。lwIP 侧：
`CHECKSUM_GEN_IP/UDP/TCP=0` + netif 标志 `NETIF_FLAG_..._CHECKSUM_OFFLOAD`。
阻塞项：82574 上下文描述符的精确字段布局需数据手册/内核源码对照，
QEMU e1000e 模拟支持（e1000e_core.c 的 tso/checksum 路径）待实测。

## 多队列 + MSI-X（计划）

82574L：2 RX + 2 TX 队列。RX 多队列 = MRQC（0x5818）选 RSS 或 VT 模式 +
RXDCTL[0/1] 使能 + queue1 环寄存器（RDBAL1 0x2900 组）+ MSI-X 表每队列一个
向量路由到不同核。之后每核一个 PollRX 线程各拉各的队列——真正的每核独立
数据面。阻塞项同校验卸载：寄存器布局核对 + QEMU 模拟行为实测。

## 性能基线与目标

- 当前（轮询+双拷贝，QEMU TCG）：ping 往返正常；吞吐未压测（下轮加 TX flood 基准）。
- 目标路径：零拷贝 + 批处理 + 卸载 → 目标"单核 100Mbps 线速方向"（TCG 下
  以相对提升为准，绝对数字只在 KVM/真实硬件上成立）。

## 零拷贝生命周期审计（round 20 定稿）

所有权链逐段分析（`pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &c->pc, data, len)`）：

1. **入栈**：`ethernet_input` 对 REF pbuf 做 `pbuf_remove_header(14)`——REF 无头部预留 →
   慢路径分配新 PBUF_RAM 头 + 链接原 REF（ref 各 1）→ 链完整。
2. **消费**：`ip_input` 等消费后释放链头 → 链遍历释放 REF → 触发 `custom_free_function`
   → `kfree(容器) + RecycleRx(idx)`。**单次释放正确**（双释放已在 round 17 修复）。
3. **归还**：RecycleRx 置 state=1 + `rx_arm_advance` 前向推进 RDT；借出的洞拦住推进 ✓。

**仍未解释的观察**（round 19 二分）：双释放修复后零拷贝路径依旧"首 stat 后静默"
（拷贝路径同配置 21 节拍/10min 稳定）。已排除：双释放、探测挂死、栈溢出、索引反推。
round 21 审计增量（源码逐行复核）：
- `pbuf_alloced_custom`：PBUF_RAW 的层偏移 = 0 → payload = data 精确对齐 ✓；
  `LWIP_PBUF_CUSTOM_DATA_INIT` 为空宏 → custom_free_function 预置不被覆盖 ✓。
- `ethernet_input` 成功路径 (L248) **不释放 pbuf**（分派目标 ip_input/etharp 释放）→
  round 17 的"仅失败释放"修复方向正确（双释放确系 round 11 双分支所致）。
- raw pcb 契约：`recv` 返回 1 = 已消费（ping_recv 自释放 + 返回 1 ✓）。
- 剩余嫌疑收窄为：DHCP 慢路径头 pbuf 链的释放计数（PBUF_RAM 头 + REF 链）、
  或与桌面进程加载时序相关的串口/线程交互（需运行时跟踪，暂缓）。

**决定**：生产默认 = 拷贝路径（已验证稳定）；零拷贝保留为实验态（`NET_ZEROCOPY=0`），
上述嫌疑列表即后续修复路线图。

## 已落地（可观测性 + 关键修复历程）

- 驱动层完整计数器：rx/tx 包数+字节、丢包、描述符错误、环满、IRQ 数。
- lwIP 线程每 10 秒输出速率（rx/s、tx/s）→ 长期趋势/泄漏/退化可直接观测。
- **RTT 百分位画像**：每 5 分钟输出 ping RTT 的 min/P50/P90/P99/max + 丢包数。
- **零拷贝（索引显式传递版）**：pbuf_custom 直挂 DMA 缓冲，check-out/RecycleRx
  状态机 + 借出洞处理。历史故障：① 按地址差反推索引（独立页分配不连续 → 全错）
  ② 内核线程栈 16KB 不足（lwIP 深路径 + DHCP 帧处理 → 栈溢出 → 线程静默死亡，
  曾被误判为环形竞态）——两者均已修复，网络线程用 `NewKernelThreadEx(..., 8)`。
- 停滞自诊断：30 秒无 RX 帧 → dump 环形状态机全景。
