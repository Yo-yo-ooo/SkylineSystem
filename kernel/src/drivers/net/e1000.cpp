//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* e1000.cpp — Intel 82540EM/82574L 网卡驱动 (legacy 收发环)
 * 结构: PCI 探测 → MMIO 映射 → 复位 → EEPROM 读 MAC → 收发环 → 中断。
 * 中断走传统 INTx (BSP), 多队列/MSI-X 后续扩展。
 * P1-51 前提文档化: RX 状态机 (g_rx_tail/g_rx_armed/g_rx_desc_state)
 * 是全局状态, 依赖「收包线程与 IRQ 同核」的隐式前提 —— INTx 绑 BSP,
 * lwIP 收包线程 (PollRX) 也运行在 BSP, IrqSave 关中断保护读写;
 * 若未来把收包线程迁到其他核, 必须加锁或迁移状态到 per-CPU。 */
#include <drivers/net/e1000.h>
#include <mem/heap.h>
#include <arch/x86_64/dev/pci/pci.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <klib/kio.h>

extern pagemap_t *kernel_pagemap;
extern volatile uint64_t hhdm_offset;

namespace E1000 {

static volatile uint32_t *g_regs = nullptr;      // MMIO 基址 (HHDM)
static rx_desc *g_rx_ring = nullptr;
static tx_desc *g_tx_ring = nullptr;
static uint8_t *g_rx_bufs[E1000_RX_RING_LEN];   // 每缓冲独立一页 (GetPhysics 可靠)
static uint8_t *g_tx_dma = nullptr;              // 发送 DMA 缓冲 (VMM::Alloc 页,
                                                 // GetPhysics 只对分配器页有效,
                                                 // 静态 BSS 地址会解析失败 → 发出全零帧)
/* 零拷贝描述符状态: 0=硬件拥有, 1=DD已消费待重新武装, 2=被消费方借出 */
static uint8_t g_rx_desc_state[E1000_RX_RING_LEN];
static uint32_t g_rx_armed = E1000_RX_RING_LEN - 1;  // 已交给硬件的最后位置 (RDT)
static uint8_t  g_mac[6] = {0};
static InputFn  g_input = nullptr;
static void   *g_input_arg = nullptr;
static bool     g_inited = false;
static uint32_t g_rx_tail = 0;                   // 软件尾指针 (与 RDT 同步)
static uint32_t g_tx_tail = 0;
static volatile uint32_t g_irq_count = 0;        // 诊断: IRQ 次数
static volatile uint32_t g_rx_pkts = 0;          // 诊断: 收包数
/* 可观测性: 完整统计 (驱动层, 供 /proc 风格输出与 DPDK-lite 设计引用) */
static volatile uint64_t g_stat_rx_bytes = 0;
static volatile uint64_t g_stat_tx_pkts = 0;
static volatile uint64_t g_stat_tx_bytes = 0;
static volatile uint32_t g_stat_rx_dropped = 0;  // 环满/分配失败
static volatile uint32_t g_stat_rx_err = 0;      // 描述符错误
static volatile uint32_t g_stat_tx_full = 0;     // 发送环满次数
static volatile uint64_t g_stat_rx_burst_sum = 0;     // round 97: Σ 每轮轮询帧数
static volatile uint64_t g_stat_rx_burst_calls = 0;  // round 97: 轮询次数
static spinlock_t g_tx_lock = 0;                 // 多核 TX 串行化 (共享 bounce + TDT)

uint32_t DiagIRQCount() { return g_irq_count; }
uint32_t DiagRXPkts() { return g_rx_pkts; }
uint32_t DiagRxTail() { return g_rx_tail; }
uint32_t DiagRxArmed() { return g_rx_armed; }
uint8_t DiagDescState(uint32_t i) { return (g_inited && i < E1000_RX_RING_LEN) ? g_rx_desc_state[i] : 0xFF; }
static inline uint32_t dreg(uint32_t off) { return g_inited ? *(volatile uint32_t *)((uintptr_t)g_regs + off) : 0; }
uint32_t DiagTXTDH() { return dreg(TDH); }
Stats GetStats(void) {
    Stats s{};
    s.rx_pkts    = __atomic_load_n(&g_rx_pkts, __ATOMIC_RELAXED);
    s.rx_bytes   = __atomic_load_n(&g_stat_rx_bytes, __ATOMIC_RELAXED);
    s.tx_pkts    = __atomic_load_n(&g_stat_tx_pkts, __ATOMIC_RELAXED);
    s.tx_bytes   = __atomic_load_n(&g_stat_tx_bytes, __ATOMIC_RELAXED);
    s.rx_dropped = __atomic_load_n(&g_stat_rx_dropped, __ATOMIC_RELAXED);
    s.rx_err     = __atomic_load_n(&g_stat_rx_err, __ATOMIC_RELAXED);
    s.tx_full    = __atomic_load_n(&g_stat_tx_full, __ATOMIC_RELAXED);
    s.irq_count  = g_irq_count;
    s.rx_burst_avg = __atomic_load_n(&g_stat_rx_burst_calls, __ATOMIC_RELAXED)
        ? (uint32_t)(__atomic_load_n(&g_stat_rx_burst_sum, __ATOMIC_RELAXED)
                     / __atomic_load_n(&g_stat_rx_burst_calls, __ATOMIC_RELAXED))
        : 0;
    return s;
}
uint32_t DiagRCTL() { return dreg(RCTL); }
uint32_t DiagTCTL() { return dreg(TCTL); }
uint32_t DiagRDH() { return dreg(RDH); }
uint32_t DiagRDT() { return dreg(RDT); }
uint32_t DiagSTATUS() { return dreg(STATUS); }
uint8_t DiagRxDesc0Status() { return g_inited ? g_rx_ring[0].status : 0; }
uint8_t DiagRxDesc0Errors() { return g_inited ? g_rx_ring[0].errors : 0; }
uint8_t DiagBuf0Byte0() { return g_inited ? g_rx_bufs[0][0] : 0; }
uint64_t DiagDesc0Addr() { return g_inited ? g_rx_ring[0].addr : 0; }

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)((uintptr_t)g_regs + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)((uintptr_t)g_regs + off) = v; }

static uint16_t eeprom_read(uint8_t addr) {
    // EERD: bit0=START, bit4=DONE, bits16-31=DATA
    wr(EERD, (uint32_t)1 | ((uint32_t)addr << 2));
    /* 纯忙等: 早期启动 (调度器/PIT 定时器未就绪) 时 PIT::Sleep(1)
       间歇性不返回, 曾致探测挂死 (soak 实锤: 卡在 "probing e1000") */
    for (int i = 0; i < 100000; i++) {
        if (rd(EERD) & (1u << 4)) break;
    }
    /* P1-27: 超时显式失败 (不再静默返回垃圾值) */
    if (!(rd(EERD) & (1u << 4))) return 0xFFFF;
    return (uint16_t)(rd(EERD) >> 16);
}

static void irq_handler(registers *r) {
    (void)r;
    g_irq_count++;
    if (!g_regs) return;

    uint32_t icr = rd(ICR);
    if (!icr) { LAPIC::EOI(); return; }

    /* P1-108: 接线死计数器 —— RXO (接收器溢出) = 环满丢帧事件 */
    if (icr & E1000_ICR_RXO)
        __atomic_add_fetch(&g_stat_rx_dropped, 1, __ATOMIC_RELAXED);

    PollRX();
    LAPIC::EOI();
}

/* 归还: 从已武装游标向前连续推进 (借出的洞拦住) */
static void rx_arm_advance(void) {
    uint32_t i = (g_rx_armed + 1) % E1000_RX_RING_LEN;
    uint32_t n = 0;
    while (i != g_rx_tail && n < E1000_RX_RING_LEN) {
        if (g_rx_desc_state[i] != 1) break;   // 洞 (借出) 或已归还
        g_rx_desc_state[i] = 0;
        g_rx_armed = i;
        i = (i + 1) % E1000_RX_RING_LEN;
        n++;
    }
    wr(RDT, g_rx_armed);
}

/* 轮询收包 (IRQ 与 lwIP 线程共用; 关中断执行)
 * 零拷贝约定: 输入回调返回 0 = 缓冲已复制(立即重新武装);
 * 返回非 0 = 消费方借走缓冲, 稍后调用 RecycleRx 归还。 */
void PollRX(void) {
    if (!g_inited) return;
    IrqSave guard_irq;
    uint32_t guard = 0;
    uint32_t burst = 0;   /* round 97: 单次轮询的帧数 (DPDK rx_burst 口径) */
    while ((g_rx_ring[g_rx_tail].status & 1) && guard < E1000_RX_RING_LEN) {
        rx_desc *d = &g_rx_ring[g_rx_tail];
        uint16_t len = d->length;
        bool eop = (d->status & 2) != 0;
        if (len && eop) {
            uint8_t *data = g_rx_bufs[g_rx_tail];
            int32_t consumed = 0;
            if (g_input)
                consumed = g_input(data, len, g_rx_tail, g_input_arg);
            /* 统计口径: 每个已消费帧都计包数与字节 (借出/拷贝都是成功;
               失败由胶水内部计数) */
            __atomic_add_fetch(&g_stat_rx_bytes, len, __ATOMIC_RELAXED);
            g_rx_desc_state[g_rx_tail] = consumed ? 2 : 1;   // 借出 / 待武装
            g_rx_pkts++;
        } else {
            g_rx_desc_state[g_rx_tail] = 1;
        }
        if (d->errors) __atomic_add_fetch(&g_stat_rx_err, 1, __ATOMIC_RELAXED);
        d->status = 0;
        g_rx_tail = (g_rx_tail + 1) % E1000_RX_RING_LEN;
        guard++;
        burst++;
    }
    __atomic_add_fetch(&g_stat_rx_burst_sum, burst, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_stat_rx_burst_calls, 1, __ATOMIC_RELAXED);
    rx_arm_advance();
}

void RecycleRx(uint32_t idx) {
    if (!g_inited || idx >= E1000_RX_RING_LEN) return;
    IrqSave guard_irq;
    if (g_rx_desc_state[idx] == 2) g_rx_desc_state[idx] = 1;   // 借出 → 待武装
    rx_arm_advance();
}

void Init(PCI::PCIHeader0 *header) {
    if (g_inited) return;
    uint16_t device = header->Header.Device_ID;
    if (device != 0x100E && device != 0x10D3) {   // 82540EM / 82574L
        kinfoln("[e1000] 不支持的设备 ID 0x%X, 跳过\n", device);
        return;
    }

    PCI::enable_interrupt((uint64_t)header);
    PCI::enable_io_space((uint64_t)header);
    PCI::enable_mem_space((uint64_t)header);
    PCI::enable_bus_mastering((uint64_t)header);

    uint64_t phys = header->BAR0 & ~0xFu;
    if (!phys) { kinfoln("[e1000] BAR0 无效\n"); return; }
    for (uint64_t off = 0; off < 0x20000; off += 0x1000)
        VMM::Map((pagemap_t *)kernel_pagemap, phys + off, phys + off, VMM_FLAGS_MMIO);
    g_regs = (volatile uint32_t *)(phys + hhdm_offset);

    /* 复位: CTRL.RST 自清 (纯忙等, 同 eeprom_read 的原因)。
       P1-27: 超时显式失败 */
    wr(CTRL, rd(CTRL) | E1000_CTRL_RST);
    for (int i = 0; i < 100000; i++) {
        if (!(rd(CTRL) & E1000_CTRL_RST)) break;
    }
    if (rd(CTRL) & E1000_CTRL_RST) { kinfoln("[e1000] 复位超时\n"); return; }

    /* 关全部中断 → 配置环 → 再开 */
    wr(IMC, 0xFFFFFFFFu);

    /* MAC: EEPROM 0-2 字; 失败 (全零/超时全 FF) 回退 RAL */
    bool mac_valid = true;
    for (int i = 0; i < 3; i++) {
        uint16_t w = eeprom_read((uint8_t)i);
        if (w == 0xFFFF) mac_valid = false;   /* P1-27: 超时失败标记 */
        g_mac[i * 2]     = (uint8_t)(w & 0xFF);
        g_mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    if (!(g_mac[0] | g_mac[1] | g_mac[2] | g_mac[3] | g_mac[4] | g_mac[5]) || !mac_valid) {
        uint32_t lo = rd(RAL);
        for (int i = 0; i < 4; i++) g_mac[i] = (uint8_t)(lo >> (8 * i));
        uint32_t hi = rd(RAH);
        g_mac[4] = (uint8_t)(hi & 0xFF);
        g_mac[5] = (uint8_t)(hi >> 8);
    }

    /* 接收环: 描述符 1 页 + 每缓冲独立 1 页 (页对齐 → 物理地址无页内偏移问题) */
    g_rx_ring = (rx_desc *)VMM::Alloc((pagemap_t *)kernel_pagemap, 1, false);
    for (uint32_t i = 0; i < E1000_RX_RING_LEN; i++)
        g_rx_bufs[i] = (uint8_t *)VMM::Alloc((pagemap_t *)kernel_pagemap, 1, false);
    g_tx_ring = (tx_desc *)VMM::Alloc((pagemap_t *)kernel_pagemap, 1, false);
    g_tx_dma  = (uint8_t *)VMM::Alloc((pagemap_t *)kernel_pagemap, 1, false);
    bool ok = g_rx_ring && g_tx_ring && g_tx_dma;
    for (uint32_t i = 0; i < E1000_RX_RING_LEN && ok; i++)
        if (!g_rx_bufs[i]) ok = false;
    if (!ok) { kinfoln("[e1000] 环内存分配失败\n"); return; }

    uint64_t rx_phys = VMM::GetPhysics((pagemap_t *)kernel_pagemap, (uint64_t)g_rx_ring);
    uint64_t tx_phys = VMM::GetPhysics((pagemap_t *)kernel_pagemap, (uint64_t)g_tx_ring);
    for (uint32_t i = 0; i < E1000_RX_RING_LEN; i++) {
        /* GetPhysics 返回页基地址, 补页内偏移才是字节级物理地址 */
        g_rx_ring[i].addr   = VMM::GetPhysics((pagemap_t *)kernel_pagemap, (uint64_t)g_rx_bufs[i])
                            + ((uint64_t)g_rx_bufs[i] & 0xFFF);
        g_rx_ring[i].status = 0;
        g_rx_desc_state[i] = 0;
        _memset(g_rx_bufs[i], 0xAB, 64);   // 金丝雀: 验证 DMA 落点
    }
    for (uint32_t i = 0; i < E1000_TX_RING_LEN; i++)
        g_tx_ring[i].status = 1;   // 全部视为已完成 (可复用)

    wr(RDBAL, (uint32_t)rx_phys);
    wr(RDBAH, (uint32_t)(rx_phys >> 32));
    wr(RDLEN, E1000_RX_RING_LEN * 16);
    wr(RDH, 0);
    /* 硬件拥有 [RDH+1, RDT] 的描述符; RDT = N-1 交出全部 (RDH=RDT=0 表示
       硬件一个描述符都没有 → RX 静默丢弃, 实测 slirp 下收包为 0 的根因) */
    wr(RDT, E1000_RX_RING_LEN - 1);
    g_rx_tail = 0;

    wr(TDBAL, (uint32_t)tx_phys);
    wr(TDBAH, (uint32_t)(tx_phys >> 32));
    wr(TDLEN, E1000_TX_RING_LEN * 16);
    wr(TDH, 0); wr(TDT, 0);
    g_tx_tail = 0;

    /* 帧间间隔: IPGT=10, IPGR1=8, IPGR2=6 */
    wr(TIPG, 0x0060200A);

    /* RX: 混杂(广播必收) + 2048B 缓冲 + 剥 CRC。
       P1-26: 关闭 LPE —— 2KB 缓冲 + LPE 会接受巨帧越界写 */
    wr(RCTL, E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE |
             E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048);
    /* TX: 使能 + 填充短帧 + 全双工碰撞阈值 */
    wr(TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT_15 | E1000_TCTL_COLD_FD);

    /* 链路 (82540EM 无 PHY 控制, 直接开) */
    wr(CTRL, rd(CTRL) | E1000_CTRL_SLU | E1000_CTRL_FD);

    /* 中断: RX 满/溢出/低水位 + 链路状态 + TX 队列空 */
    wr(IMS, E1000_ICR_RXT0 | E1000_ICR_RXO | E1000_ICR_RXDMT0 | E1000_ICR_LSC | E1000_ICR_TXQE);

    /* 传统 INTx: PCI 中断线给出 ISA IRQ。
       P1-28: 魔法数回退显式告警 (回退向量可能冲突 —— 后续集中
       分配校验; 现按驱动加载顺序, e1000 在 xHCI/NVMe 之后探测,
       向量 43 (11+32) 为 QEMU slirp 的稳定配置) */
    uint8_t irq = header->InterruptLine;
    if (irq == 0 || irq == 0xFF) {
        irq = 11;   // 保守回退 (QEMU 常见 NIC 中断线)
        kinfoln("[e1000] IRQ 中断线缺失 (0x%02x), 回退 %u\n", header->InterruptLine, irq);
    }
    uint32_t vector = (uint32_t)irq + 32;
    idt_install_irq((uint8_t)vector, (void *)irq_handler);
    IOAPIC::RemapIRQ(smp_bsp_cpu, irq, vector, false);

    g_inited = true;
    kinfoln("[e1000] 就绪: MAC %02X:%02X:%02X:%02X:%02X:%02X, IRQ %u → vector %u\n",
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5], irq, vector);
    kinfoln("[e1000][dbg] ring_va=%p ring_phys=%llx buf0_va=%p buf0_phys=%llx buf1_phys=%llx\n",
            (void *)g_rx_ring, (unsigned long long)rx_phys,
            (void *)g_rx_bufs[0],
            (unsigned long long)VMM::GetPhysics((pagemap_t *)kernel_pagemap, (uint64_t)g_rx_bufs[0]),
            (unsigned long long)VMM::GetPhysics((pagemap_t *)kernel_pagemap, (uint64_t)g_rx_bufs[1]));
}

int32_t Send(const uint8_t *data, uint16_t len) {
    if (!g_inited || !data || len == 0 || len > 1514) return -1;

    /* 多核 TX: 共享 bounce 缓冲与 TDT 软件尾, 自旋锁串行化
       (中断上下文不碰 TX, 无需关中断) */
    spinlock_lock(&g_tx_lock);

    uint32_t next = (g_tx_tail + 1) % E1000_TX_RING_LEN;
    if (next == rd(TDH)) { __atomic_add_fetch(&g_stat_tx_full, 1, __ATOMIC_RELAXED); spinlock_unlock(&g_tx_lock); return -1; }   // 环满

    /* 拷贝进驱动自有 DMA 缓冲 (调用方的内存可能是任意虚拟地址,
       GetPhysics 无法解析)。注意 _memcpy 参数为转置序 (src, dest, n) */
    _memcpy((void *)data, g_tx_dma, len);

    tx_desc *d = &g_tx_ring[g_tx_tail];
    d->addr   = VMM::GetPhysics((pagemap_t *)kernel_pagemap, (uint64_t)g_tx_dma)
              + ((uint64_t)g_tx_dma & 0xFFF);
    d->length = len;
    d->cmd    = 0x01 | 0x02 | 0x08;   // EOP | IFCS | RS
    d->status = 0;
    g_tx_tail = next;
    wr(TDT, g_tx_tail);
    __atomic_add_fetch(&g_stat_tx_pkts, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_stat_tx_bytes, len, __ATOMIC_RELAXED);
    spinlock_unlock(&g_tx_lock);
    return 0;
}

void SetInputCallback(InputFn fn, void *arg) { g_input = fn; g_input_arg = arg; }
void GetMAC(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = g_mac[i]; }
bool LinkUp() { return g_inited && (rd(STATUS) & E1000_STATUS_LU); }

} // namespace E1000
