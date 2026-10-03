//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* e1000_lwip.cpp — lwIP 移植胶水 (NO_SYS=1 轮询模式)
 * 结构:
 *   e1000 IRQ 回调 → 帧拷贝 → 无锁环形 FIFO (IRQ 里不跑 lwIP)
 *   lwIP 内核线程 → 出队 → pbuf → ethernet_input → 协议栈
 *   TX: 协议栈 → low_level_output → 静态弹跳缓冲 → E1000::Send
 * 多核/DPDK-lite (每核 pbuf 池、零拷贝、多队列) 见后续轮。 */
#include <drivers/net/e1000.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/smp/smp.h>
#include <klib/kio.h>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <netif/ethernet.h>
#include <lwip/dhcp.h>
#include <lwip/pbuf.h>
#include <lwip/timeouts.h>
#include <lwip/ip4.h>
#include <lwip/icmp.h>
#include <lwip/raw.h>
#include <lwip/inet_chksum.h>

/* sys_now 的 C 链接定义在 lwip_port.c; 这里只提供时间源 */
extern "C" uint64_t net_time_ms(void) { return PIT::TimeSinceBootMS(); }

/* NET_FLOOD_AUTO: diagnostic multi-core TX flood. Default OFF. When enabled,
   NetStackInit also spawns one infinite flood_thread per CPU 1..3 as a data-
   plane stress test. It must never run on the normal boot path: the flood
   starts inside the boot window, saturates cores and contends global locks,
   which caused the intermittent failure to reach the desktop. */
#define NET_FLOOD_AUTO 0

static struct netif g_netif;
static bool g_started = false;
static struct raw_pcb *g_ping_pcb = nullptr;
static uint32_t g_ping_seq = 0;
static uint32_t g_ping_replies = 0;
static uint64_t g_ping_send_time = 0;   // 最近一次 ping 发送时刻 (RTT 起点)

/* 性能画像: ping RTT 直方图 (环, 每 5 分钟算 P50/P90/P99) */
#define RTT_HIST_SLOTS 512
static uint64_t g_rtt_hist[RTT_HIST_SLOTS];
static uint32_t g_rtt_cnt = 0;
static uint32_t g_rtt_lost = 0;         // 发送后 10s 无回复 = 丢包

static void rtt_report(void) {
    if (g_rtt_cnt < 8) return;
    uint64_t sorted[RTT_HIST_SLOTS];
    _memcpy(g_rtt_hist, sorted, sizeof(sorted));   // 转置序: (src, dest, n)
    /* 简单插入排序 (小数组, 5 分钟一次) */
    for (uint32_t i = 1; i < g_rtt_cnt && i < RTT_HIST_SLOTS; i++) {
        uint64_t key = sorted[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    uint32_t n = g_rtt_cnt < RTT_HIST_SLOTS ? g_rtt_cnt : RTT_HIST_SLOTS;
    uint64_t p50 = sorted[n * 50 / 100];
    uint64_t p90 = sorted[n * 90 / 100];
    uint64_t p99 = sorted[n * 99 / 100];
    kinfoln("[lwip][rtt] n=%u min=%llu P50=%llu P90=%llu P99=%llu max=%llu ms drop=%u\n",
            n, (unsigned long long)sorted[0],
            (unsigned long long)p50, (unsigned long long)p90, (unsigned long long)p99,
            (unsigned long long)sorted[n - 1], g_rtt_lost);
    g_rtt_cnt = 0;
    g_rtt_lost = 0;
}

/* ICMP echo 回复回调: 收到即证明 ARP+IP+ICMP 全链路 */
static uint8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip4_addr_t *addr) {
    (void)arg; (void)pcb;
    g_ping_replies++;
    if (g_ping_send_time) {
        uint64_t rtt = PIT::TimeSinceBootMS() - g_ping_send_time;
        if (g_rtt_cnt < RTT_HIST_SLOTS)
            g_rtt_hist[g_rtt_cnt] = rtt;
        g_rtt_cnt++;
        g_ping_send_time = 0;
    }
    if (g_ping_replies <= 3)
        kinfoln("[lwip][ping] reply from %s (len=%u, replies=%u)\n",
                ip4addr_ntoa(addr), (unsigned)p->tot_len, g_ping_replies);
    pbuf_free(p);
    return 1;
}

static void ping_send_once(void) {
    if (!g_ping_pcb) return;
    struct pbuf *p = pbuf_alloc(PBUF_IP, 32, PBUF_RAM);
    if (!p) return;
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)p->payload;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->chksum = 0;
    icmp->id = 0x534B;                      // 'SK'
    icmp->seqno = lwip_htons((u16_t)++g_ping_seq);
    _memset((uint8_t *)p->payload + sizeof(struct icmp_echo_hdr), 0x5A, p->len - sizeof(struct icmp_echo_hdr));
    icmp->chksum = inet_chksum(p->payload, p->len);
    ip4_addr_t gw;
    ip4addr_aton("10.0.2.2", &gw);
    raw_sendto(g_ping_pcb, p, &gw);
    pbuf_free(p);
}

/* ---- 多核 TX 洪泛 (DPDK-lite 原始数据面路径: 绕过协议栈直发) ----
 * 每核一个内核线程, 连续 Send 罐头广播 ICMP 帧;
 * 验证: E1000::Send 多核并发 (自旋锁) + SLUB per-CPU 池 + NIC 环深度。
 * 帧内容为静态构建的广播帧, 无需 ARP/协议栈参与。 */
static uint8_t g_flood_frame[64];
static void flood_thread(void) {
    /* 广播 ICMP echo (eth dst=ff*6, src=本机 MAC, IPv4 0.0.0.0→255.255.255.255) */
    while (true) {
        for (int i = 0; i < 100; i++) {
            if (E1000::Send(g_flood_frame, sizeof(g_flood_frame)) != 0) {
                /* 环满退避: 此前紧旋烧满时间片, 拖慢同核调度
                   (实测 stat 节拍 10s→40s) */
                PIT::Sleep(1);
            }
        }
        PIT::Sleep(100);
    }
}

static void build_flood_frame(void) {
    _memset(g_flood_frame, 0, sizeof(g_flood_frame));
    for (int i = 0; i < 6; i++) g_flood_frame[i] = 0xFF;          // 广播
    uint8_t mac[6];
    E1000::GetMAC(mac);
    _memcpy(mac, g_flood_frame + 6, 6);                            // src MAC
    g_flood_frame[12] = 0x08; g_flood_frame[13] = 0x00;           // IPv4
    g_flood_frame[14] = 0x45;                                      // v4 IHL5
    g_flood_frame[15] = 0x00;                                      // TOS
    g_flood_frame[16] = 0x00; g_flood_frame[17] = 28;              // total len 28
    g_flood_frame[22] = 64;                                        // TTL
    g_flood_frame[23] = 0x01;                                      // ICMP
    /* src IP 0.0.0.0 (20-23), dst 255.255.255.255 (24-27) */
    g_flood_frame[24] = g_flood_frame[25] = g_flood_frame[26] = g_flood_frame[27] = 0xFF;
    g_flood_frame[34] = 0x08;                                      // ICMP echo
    /* 校验和故意不填: 数据面压力测试不依赖对端接受 */
    for (int i = 40; i < 64; i++) g_flood_frame[i] = (uint8_t)i;
}

/* ---- RX FIFO: 生产者(轮询/IRQ) / lwIP 线程消费者 ---- */
/* NET_ZEROCOPY=1: pbuf 直挂 DMA 缓冲。
   历史: 双释放 (round 17) 与探测忙等 (round 18) 修复后, 长稳仍出现
   "首 stat 后静默" (round 19 二分中); 拷贝路径为生产默认。 */
#define NET_ZEROCOPY 0
#define RX_FIFO_SLOTS 64

#if NET_ZEROCOPY
static struct pbuf *g_rx_pbufs[RX_FIFO_SLOTS];
#else
struct rx_frame { uint8_t *data; uint16_t len; };
static rx_frame g_rx_fifo[RX_FIFO_SLOTS];
#endif
static volatile uint32_t g_rx_head = 0;   // 生产者写
static uint32_t g_rx_tail = 0;            // 消费者读
static uint32_t g_rx_dropped = 0;
/* 故障注入: 前 N 次 RX 构造强制失败 (OOM 路径实验) */
static uint32_t g_rx_fail_remaining = 5;

#if NET_ZEROCOPY
struct rx_pbuf_custom {
    struct pbuf_custom pc;   // 必须为首成员 (lwIP 由容器指针反推)
    uint32_t idx;            // 描述符索引 (归还用)
};

static void rx_pbuf_free_fn(struct pbuf *p) {
    rx_pbuf_custom *c = (rx_pbuf_custom *)((uint8_t *)p - offsetof(rx_pbuf_custom, pc));
    uint32_t idx = c->idx;
    kfree(c);
    E1000::RecycleRx(idx);
}
#endif

/* 回调 (轮询线程或 IRQ 上下文) */
static int32_t rx_input(uint8_t *data, uint16_t len, uint32_t idx, void *arg) {
    (void)arg;
    if (g_rx_fail_remaining) {
        g_rx_fail_remaining--;
        g_rx_dropped++;
        return 0;                           // 注入失败: 不借走
    }
#if NET_ZEROCOPY
    rx_pbuf_custom *c = (rx_pbuf_custom *)kmalloc(sizeof(rx_pbuf_custom));
    if (!c) { g_rx_dropped++; return 0; }
    c->idx = idx;
    c->pc.custom_free_function = rx_pbuf_free_fn;
    struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &c->pc, data, len);
    if (!p) { kfree(c); g_rx_dropped++; return 0; }
    uint32_t next = (g_rx_head + 1) % RX_FIFO_SLOTS;
    if (next == g_rx_tail) { pbuf_free(p); g_rx_dropped++; return 0; }
    g_rx_pbufs[g_rx_head] = p;
    g_rx_head = next;
    return 1;                               // 借走
#else
    uint8_t *copy = (uint8_t *)kmalloc(len);
    if (!copy) { g_rx_dropped++; return 0; }
    _memcpy(data, copy, len);   /* 注意 _memcpy 转置序: (src, dest, n) */
    uint32_t next = (g_rx_head + 1) % RX_FIFO_SLOTS;
    if (next == g_rx_tail) { kfree(copy); g_rx_dropped++; return 0; }
    g_rx_fifo[g_rx_head] = {copy, len};
    g_rx_head = next;
    return 0;                               // 已拷贝: 驱动立即重新武装
#endif
}

#if NET_ZEROCOPY
static struct pbuf *rx_fifo_pop_pbuf(void) {
    if (g_rx_tail == g_rx_head) return nullptr;
    struct pbuf *p = g_rx_pbufs[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % RX_FIFO_SLOTS;
    return p;
}
#else
static bool rx_fifo_pop(rx_frame *out) {
    if (g_rx_tail == g_rx_head) return false;
    *out = g_rx_fifo[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % RX_FIFO_SLOTS;
    return true;
}
#endif

/* ---- TX ---- */
static uint8_t g_tx_bounce[2048];
static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (p->tot_len > 1514) return ERR_MEM;
    pbuf_copy_partial(p, g_tx_bounce, p->tot_len, 0);

    /* 环满重试 (TX 由 lwIP 线程独占, 无并发) */
    for (int i = 0; i < 1000; i++) {
        if (E1000::Send(g_tx_bounce, (uint16_t)p->tot_len) == 0)
            return ERR_OK;
        PIT::Sleep(1);
    }
    return ERR_MEM;
}

static err_t ethernetif_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    uint8_t mac[6];
    E1000::GetMAC(mac);
    for (int i = 0; i < 6; i++) netif->hwaddr[i] = mac[i];
    netif->hwaddr_len = 6;
    return ERR_OK;
}

static void lwip_thread(void) {
    uint64_t last_coarse = 0, last_fine = 0;
    bool logged_ip = false;
    kinfoln("[lwip] thread started, waiting for DHCP...\n");

    for (;;) {
        /* 轮询收包 */
        E1000::PollRX();

#if NET_ZEROCOPY
        struct pbuf *p;
        while ((p = rx_fifo_pop_pbuf()) != nullptr) {
            /* 所有权: ethernet_input 成功时内部已 pbuf_free (触发自定义释放
               → RecycleRx); 只有失败才由我们释放。此前双分支都释放 → 双重
               释放 → "pbuf_free: p->ref > 0" 断言 + pbuf 池损坏 = 间歇性
               崩溃/停滞的真根源 (曾误判为环形竞态/栈溢出)。 */
            if (g_netif.input(p, &g_netif) != ERR_OK)
                pbuf_free(p);
        }
#else
        rx_frame f;
        while (rx_fifo_pop(&f)) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, f.len, PBUF_RAM);
            if (p) {
                _memcpy(f.data, p->payload, f.len);   /* 转置序: (src, dest) */
                if (g_netif.input(p, &g_netif) != ERR_OK)
                    pbuf_free(p);
            }
            kfree(f.data);
        }
#endif

        uint64_t now = PIT::TimeSinceBootMS();
        if (now - last_coarse >= 1000) {
            sys_check_timeouts();
            dhcp_coarse_tmr();
            last_coarse = now;
        }
        if (now - last_fine >= 500) {
            dhcp_fine_tmr();
            last_fine = now;
        }

        if (!logged_ip && dhcp_supplied_address(&g_netif)) {
            /* ip4addr_ntoa 是静态缓冲: 一次打印只能安全调用一次 */
            kinfoln("[lwip] DHCP success: %s\n", ip4addr_ntoa(netif_ip4_addr(&g_netif)));
            kinfoln("[lwip] netmask=%s\n", ip4addr_ntoa(netif_ip4_netmask(&g_netif)));
            kinfoln("[lwip] gateway=%s\n", ip4addr_ntoa(netif_ip4_gw(&g_netif)));
            logged_ip = true;
        }

        /* ping 网关: 每 3 秒一次 */
        static uint64_t last_ping = 0;
        if (now - last_ping >= 3000) {
            if (g_ping_send_time && now - g_ping_send_time > 10000) {
                g_rtt_lost++;               // 上轮 ping 无回复
                g_ping_send_time = 0;
            }
            g_ping_send_time = now;
            ping_send_once();
            last_ping = now;
        }

        /* 性能画像: 每 5 分钟输出 RTT 百分位 */
        static uint64_t last_rtt = 0;
        if (now - last_rtt >= 300000) {
            rtt_report();
            last_rtt = now;
        }

        if (g_rx_dropped && (now & 0x3FF) == 0) {
            kinfoln("[lwip] RX dropped frames cumulative %u\n", g_rx_dropped);
            g_rx_dropped = 0;
        }

        /* 可观测性: 每 10 秒一次统计 + 速率 (观测长期趋势/泄漏/退化) */
        static uint64_t last_diag = 0;
        static uint64_t prev_rx = 0, prev_tx = 0;
        if (now - last_diag >= 10000) {
            E1000::Stats s = E1000::GetStats();
            uint64_t rxp_s = (s.rx_pkts - prev_rx) * 1000 / (now - last_diag);
            uint64_t txp_s = (s.tx_pkts - prev_tx) * 1000 / (now - last_diag);
            kinfoln("[lwip][stat] rx=%llu(%lluB) tx=%llu(%lluB) rate rx=%llu/s tx=%llu/s drop=%u err=%u txfull=%u irq=%u ping=%u\n",
                    (unsigned long long)s.rx_pkts, (unsigned long long)s.rx_bytes,
                    (unsigned long long)s.tx_pkts, (unsigned long long)s.tx_bytes,
                    (unsigned long long)rxp_s, (unsigned long long)txp_s,
                    s.rx_dropped, s.rx_err, s.tx_full, s.irq_count, g_ping_replies);
            prev_rx = s.rx_pkts; prev_tx = s.tx_pkts;
            last_diag = now;
        }

        /* 停滞自诊断: 30 秒无新 RX 帧 → dump 环形状态机全景 */
        static uint64_t last_rx = 0;
        static uint64_t last_rx_pkts = 0;
        {
            E1000::Stats s = E1000::GetStats();
            if (s.rx_pkts != last_rx_pkts) { last_rx = now; last_rx_pkts = s.rx_pkts; }
            if (now - last_rx > 30000 && (now - last_rx) % 15000 < 100) {
                kinfoln("[lwip][stall] rdh=%u rdt=%u tail=%u armed=%u fifo=%u\n",
                        E1000::DiagRDH(), E1000::DiagRDT(),
                        E1000::DiagRxTail(), E1000::DiagRxArmed(),
                        (g_rx_head - g_rx_tail) % RX_FIFO_SLOTS);
                char line[100]; int o = 0;
                for (uint32_t i = 0; i < 16; i++)
                    o += snprintf(line + o, sizeof(line) - o, "%u", (unsigned)E1000::DiagDescState(i));
                kinfoln("[lwip][stall] state[0..15]=%s\n", line);
            }
        }
        PIT::Sleep(5);
    }
}

void NetStackInit(void) {
    if (g_started) return;

    lwip_init();
    /* 静态配置 (slirp 默认网络 10.0.2.0/24, 网关 10.0.2.2);
       DHCP 已实调 dhcp_start (P3-83 修正: 原注释"暂缓"过时 ——
       ACD 检查关闭后 slirp OFFER 正常租赁, 静态 IP 为回退) */
    ip4_addr_t ip, mask, gw;
    ip4addr_aton("10.0.2.15", &ip);
    ip4addr_aton("255.255.255.0", &mask);
    ip4addr_aton("10.0.2.2", &gw);
    if (netif_add(&g_netif, &ip, &mask, &gw, NULL, ethernetif_init, ethernet_input) == NULL) {
        kinfoln("[lwip] netif_add failed\n");
        return;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    g_ping_pcb = raw_new(IP_PROTO_ICMP);
    if (g_ping_pcb) raw_recv(g_ping_pcb, ping_recv, NULL);
    build_flood_frame();

    E1000::SetInputCallback(rx_input, NULL);

    /* 诊断期: 同时启动 DHCP (静态 IP 保持, 租约到达会覆盖) */
    dhcp_start(&g_netif);

    proc_t *proc = Schedule::NewProcess(false);
    if (proc) {
        /* 网络线程深调用链 (lwIP 输入路径 + DHCP), 用 8 页=32KB 栈
           (默认 4 页在 DHCP 帧处理 + 调试打印下栈溢出, 实测线程静默死亡) */
        thread_t *nw = Schedule::NewKernelThreadEx(proc, 0, 12, (void *)lwip_thread, 8);
        /* B2 (round 5): 钉扎在 CPU0 —— RX FIFO 依赖线程与 IRQ 同核
           (P1-51 前提), 负载均衡不得迁核 */
        if (nw) nw->pinned = true;
        /* 多核 TX 洪泛: 仅诊断构建 (NET_FLOOD_AUTO) 才在 CPU1..3 起无限
           数据面压力线程; 正常启动绝不运行 (启动窗口争锁/占核, 曾致间歇
           性进不了桌面)。 */
#if NET_FLOOD_AUTO
        for (uint32_t c = 1; c < 4 && c <= (uint32_t)smp_last_cpu; c++)
            Schedule::NewKernelThreadEx(proc, c, 12, (void *)flood_thread, 8);
        g_started = true;
        kinfoln("[lwip] netif e0 up: 10.0.2.15/24 gw 10.0.2.2, ping + multi-core TX flood\n");
#else
        g_started = true;
        kinfoln("[lwip] netif e0 up: 10.0.2.15/24 gw 10.0.2.2, ping\n");
#endif
    } else {
        kinfoln("[lwip] cannot create process\n");
    }
}

/* Deferred bring-up entry. The bootstrap spawns this as a normal kernel thread
   AFTER it executes sti and kicks scheduling; NetStackInit then runs in a live
   thread context instead of the pre-sti bootstrap (which cannot block and
   races the APs/desktop). The short settle lets the desktop finish its early
   startup before the lwIP thread is created. */
void NetStackInitDeferred(void) {
    PIT::Sleep(300);
    NetStackInit();
    while (true) PIT::Sleep(1000);
}
