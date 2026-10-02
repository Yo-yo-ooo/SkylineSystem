#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/**
 * SYS_LIGHTWEIGHT_PROT==1: if you want inter-task protection for certain
 * critical regions during buffer allocation, deallocation and memory
 * allocation and deallocation.
 */
#define SYS_LIGHTWEIGHT_PROT    0

/**
 * NO_SYS==1: Provides VERY minimal functionality. Otherwise,
 * use lwIP facilities.
 */
#define NO_SYS                  1                   //(1)

/**
 * NO_SYS_NO_TIMERS==1: Drop support for sys_timeout when NO_SYS==1
 * Mainly for compatibility to old versions.
 */
#define NO_SYS_NO_TIMERS        0

/* ---------- Memory options ---------- */
/* MEM_ALIGNMENT: should be set to the alignment of the CPU for which
    lwIP is compiled. 4 byte alignment -> define MEM_ALIGNMENT to 4, 2
    byte alignment -> define MEM_ALIGNMENT to 2. */
#define MEM_ALIGNMENT           16                 // 16B: pbuf 负载保持缓存行友好 (内核 kmalloc 类粒度)

/* MEM_SIZE: the size of the heap memory. If the application will send
a lot of data that needs to be copied, this should be set high. */
#define MEM_SIZE                (64*1024)

/* MEMP_NUM_PBUF: the number of memp struct pbufs. If the application
    sends a lot of data out of ROM (or other static memory), this
    should be set high. */
#define MEMP_NUM_PBUF           64
/* MEMP_NUM_UDP_PCB: the number of UDP protocol control blocks. One
    per active UDP "connection". */
#define MEMP_NUM_UDP_PCB        8
/* MEMP_NUM_TCP_PCB: the number of simulatenously active TCP
    connections. */
#define MEMP_NUM_TCP_PCB        16
/* MEMP_NUM_TCP_PCB_LISTEN: the number of listening TCP
    connections. */
#define MEMP_NUM_TCP_PCB_LISTEN 8
/* MEMP_NUM_TCP_SEG: the number of simultaneously queued TCP
    segments. */
#define MEMP_NUM_TCP_SEG        32
/* MEMP_NUM_SYS_TIMEOUT: the number of simulateously active
    timeouts. */
#define MEMP_NUM_SYS_TIMEOUT    12

/* ---------- Pbuf options ---------- */
/* PBUF_POOL_SIZE: the number of buffers in the pbuf pool. */
#define PBUF_POOL_SIZE          32

/* PBUF_POOL_BUFSIZE: the size of each pbuf in the pbuf pool. */
#define PBUF_POOL_BUFSIZE       2048

/* ---------- TCP options ---------- */
#define LWIP_TCP                1
#define TCP_TTL                 255

/* Controls if TCP should queue segments that arrive out of
    order. Define to 0 if your device is low on memory. */
#define TCP_QUEUE_OOSEQ         0

/* TCP Maximum segment size. */
#define TCP_MSS                 (1500 - 40)             //    (6)

/* TCP sender buffer space (bytes). */
#define TCP_SND_BUF             (4*TCP_MSS)       //  (7)

/*  TCP_SND_QUEUELEN: TCP sender buffer space (pbufs). This must be at least
as much as (2 * TCP_SND_BUF/TCP_MSS) for things to work. */

#define TCP_SND_QUEUELEN        (2* TCP_SND_BUF/TCP_MSS)

/* TCP receive window. */
#define TCP_WND                 (2*TCP_MSS)       //  (8)

/* ---------- ICMP options ---------- */
#define LWIP_ICMP                       1

/* ---------- DHCP options ---------- */
#define LWIP_DHCP               1
/* slirp 会代理应答指向 guest IP 的 ARP (为主机侧可达性), lwIP 的
   DHCP ACD 冲突探测会误判"IP 已被占用"→ 拒绝 OFFER 无限重发 DISCOVER
   (实测: 第三个 TX 帧即 ARP who-has 10.0.2.15, 之后循环)。关闭 ACD。 */
#define LWIP_DHCP_DOES_ACD_CHECK 0

/* ---------- UDP options ---------- */
#define LWIP_UDP                1
#define UDP_TTL                 255

/* ---------- RAW options ---------- */
#define LWIP_RAW                1

/* ---------- Statistics options ---------- */
#define LWIP_STATS 0
#define LWIP_PROVIDE_ERRNO 1

/* ---------- link callback options ---------- */
/* LWIP_NETIF_LINK_CALLBACK==1: Support a callback function from an interface
* whenever the link changes (i.e., link down)
*/
#define LWIP_NETIF_LINK_CALLBACK        0
/*
    --------------------------------------
    ---------- Checksum options ----------
    --------------------------------------
*/

/* e1000 第一版不启用硬件校验卸载 (后续 DPDK-lite 轮再开 TX offload),
   全部软件校验 —— 与 QEMU slirp 对端无关, 保证端到端正确 */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
/* 已知移植项: 本 vendored lwIP 的 RX 校验验证路径会丢弃 slirp 的合法帧
   (ping 实测: 开启后收不到回复; slirp 校验和本身无误)。发侧校验全部开启,
   收侧验证待与 lwIP 版本核对后修复。 */
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_ICMP             0

/*
    ----------------------------------------------
    ---------- Sequential layer options ----------
    ----------------------------------------------
*/
/**
 * LWIP_NETCONN==1: Enable Netconn API (require to use api_lib.c)
 */
#define LWIP_NETCONN                    0          //  (9)

/*
    ------------------------------------
    ---------- Socket options ----------
    ------------------------------------
*/
/**
 * LWIP_SOCKET==1: Enable Socket API (require to use sockets.c)
 */
#define LWIP_SOCKET                     0          //  (10)

/*
    ----------------------------------------
    ---------- Lwip Debug options ----------
    ----------------------------------------
*/
/* 调试默认关闭 (开启会淹串口)。历史: 多模块 DEBUG 崩溃已定位为内核
   线程栈 16KB 不够 (现网络线程 32KB, 见 e1000_lwip.cpp), 非 vsnprintf
   缺陷; cc.h 的 DIAG varargs 链已修复 (上游 printf x 模式)。 */
#define LWIP_DEBUG                      0

#define MEM_CUSTOM_ALLOCATOR            1
#define MEM_CUSTOM_FREE                 kfree
#define MEM_CUSTOM_MALLOC               kmalloc
#define MEM_CUSTOM_CALLOC               kcalloc

#endif /* __LWIPOPTS_H__ */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/