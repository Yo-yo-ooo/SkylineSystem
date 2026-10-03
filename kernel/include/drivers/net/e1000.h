//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* drivers/net/e1000.h — Intel 82540EM/82574L (e1000/e1000e) 网卡驱动
 * 寄存器子集: 传统(legacy)收发环, 与 QEMU `-net nic` 模拟的 e1000 兼容;
 * MSI/MSI-X 与多队列(e1000e)留作后续扩展。 */
#pragma once
#include <stdint.h>
#include <arch/x86_64/dev/pci/pcidef.h>

namespace E1000 {

/* ---- MMIO 寄存器偏移 ---- */
enum Reg {
    CTRL   = 0x0000,   // 设备控制 (RST=bit26, SLU=bit6, FD=bit0)
    STATUS = 0x0008,   // 设备状态 (FD=bit0, LU=bit1, SPEED=bit6-7)
    EECD   = 0x0010,   // EEPROM 控制
    EERD   = 0x0014,   // EEPROM 读
    ICR    = 0x00C0,   // 中断原因
    ITR    = 0x00C4,   // 中断节流
    ICS    = 0x00C8,   // 中断置位(软件)
    IMS    = 0x00D0,   // 中断掩码置位
    IMC    = 0x00D8,   // 中断掩码清除
    RCTL   = 0x0100,   // 接收控制
    TCTL   = 0x0400,   // 发送控制
    TIPG   = 0x0410,   // 帧间间隔
    RDBAL  = 0x2800,   // 接收环基址低
    RDBAH  = 0x2804,   // 接收环基址高
    RDLEN  = 0x2808,   // 接收环长度
    RDH    = 0x2810,   // 接收头
    RDT    = 0x2818,   // 接收尾
    TDBAL  = 0x3800,   // 发送环基址低
    TDBAH  = 0x3804,   // 发送环基址高
    TDLEN  = 0x3808,   // 发送环长度
    TDH    = 0x3810,   // 发送头
    TDT    = 0x3818,   // 发送尾
    RAL    = 0x5400,   // 接收地址低
    RAH    = 0x5404,   // 接收地址高 (AV=bit31)
    MTA    = 0x5200,   // 组播表
};

/* CTRL */
#define E1000_CTRL_RST  (1u << 26)
#define E1000_CTRL_SLU  (1u << 6)
#define E1000_CTRL_FD   (1u << 0)

/* STATUS */
#define E1000_STATUS_LU (1u << 1)
#define E1000_STATUS_FD (1u << 0)

/* RCTL */
#define E1000_RCTL_EN     (1u << 1)
#define E1000_RCTL_SBP    (1u << 2)
#define E1000_RCTL_UPE    (1u << 3)
#define E1000_RCTL_MPE    (1u << 4)
#define E1000_RCTL_LPE    (1u << 5)
#define E1000_RCTL_BAM    (1u << 15)
#define E1000_RCTL_SECRC  (1u << 26)
#define E1000_RCTL_BSEX   (1u << 25)
#define E1000_RCTL_BSIZE_2048 (0u << 16)

/* TCTL */
#define E1000_TCTL_EN     (1u << 1)
#define E1000_TCTL_PSP    (1u << 3)
#define E1000_TCTL_CT_15  (0x0Fu << 4)
#define E1000_TCTL_COLD_FD (0x40u << 12)

/* ICR / IMS 位 */
#define E1000_ICR_TXDW   (1u << 0)
#define E1000_ICR_TXQE   (1u << 1)
#define E1000_ICR_LSC    (1u << 2)
#define E1000_ICR_RXDMT0 (1u << 4)
#define E1000_ICR_RXO    (1u << 6)
#define E1000_ICR_RXT0   (1u << 7)

/* 环参数 */
#define E1000_RX_RING_LEN 256
#define E1000_TX_RING_LEN 256
#define E1000_RX_BUF_SIZE 2048

/* 传统描述符 (16 字节, 16 对齐) */
struct rx_desc {
    uint64_t addr;      // 缓冲区物理地址
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;    // DD=bit0, EOP=bit1
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;      // 缓冲区物理地址
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;       // EOP=bit0, IFCS=bit1, RS=bit3
    uint8_t  status;    // DD=bit0
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static_assert(sizeof(rx_desc) == 16, "rx_desc must be 16 bytes");
static_assert(sizeof(tx_desc) == 16, "tx_desc must be 16 bytes");

/* ---- 驱动接口 (init.cpp 的 probe 与 lwIP 胶水调用) ---- */
void Init(PCI::PCIHeader0 *header);

/* 发送一个以太网帧 (数据必须保持到硬件确认 DD)。返回 0 成功, -1 环满。 */
int32_t Send(const uint8_t *data, uint16_t len);

/* 接收回调: 由轮询/中断处理程序在收到完整帧时调用 (lwIP 胶水注册)。
   idx = 描述符索引 (零拷贝归还用)。返回 0 = 缓冲已复制, 驱动立即
   重新武装; 返回非 0 = 消费方借走缓冲, 稍后调用 RecycleRx(idx)。 */
typedef int32_t (*InputFn)(uint8_t *data, uint16_t len, uint32_t idx, void *arg);
void SetInputCallback(InputFn fn, void *arg);

void GetMAC(uint8_t out[6]);
bool LinkUp();
void PollRX(void);         // 轮询收包 (lwIP 线程/IRQ 共用)
/* 零拷贝: 消费方调用 RecycleRx 归还缓冲 (按描述符索引重新武装) */
void RecycleRx(uint32_t idx);

/* 可观测性统计 (驱动层计数器) */
struct Stats {
    uint64_t rx_pkts;      // 收包数
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint32_t rx_dropped;   // 环满/上层拒绝
    uint32_t rx_err;       // 描述符错误
    uint32_t tx_full;      // 发送环满次数
    uint32_t irq_count;
    uint32_t rx_burst_avg; // round 97: 平均每轮 PollRX 的帧数 (DPDK rx_burst 口径)
};
Stats GetStats(void);

uint32_t DiagIRQCount();   // 诊断: 中断次数
uint32_t DiagRXPkts();     // 诊断: 收包数
uint32_t DiagTXTDH();      // 诊断: 硬件发送头 (完成进度)
uint32_t DiagRCTL();
uint32_t DiagTCTL();
uint32_t DiagRDH();
uint32_t DiagRDT();
uint32_t DiagSTATUS();
uint8_t  DiagRxDesc0Status();
uint8_t  DiagRxDesc0Errors();
uint8_t  DiagBuf0Byte0();
uint64_t DiagDesc0Addr();
/* 停滞诊断: 环形状态机全景 (零拷贝竞态定位用) */
uint32_t DiagRxTail();
uint32_t DiagRxArmed();
uint8_t  DiagDescState(uint32_t i);

} // namespace E1000

/* 网络栈整体上线 (lwIP 移植胶水: e1000 + netif + DHCP 线程)。
   需在 PCI 枚举 (e1000 探测) 与调度器就绪之后调用。 */
void NetStackInit(void);
/* Deferred bring-up: spawned as a kernel thread by the bootstrap after sti +
   scheduler kick; runs NetStackInit in a live thread context, never pre-sti. */
void NetStackInitDeferred(void);
