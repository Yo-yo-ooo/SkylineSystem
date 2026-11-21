// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * dosvm_contract.h —— 单核可逆退出 IA-32e（"CPU 下线跑 16 位代码再回来"）
 * 的跨层共享契约。Tier0 / Tier1 内建组件 / Tier2 .ko / 用户态客户端都按
 * 这一份常量与签名实现，避免四层各自漂移。
 *
 * 硬件可行性依据（x86_64 SDM Vol.3A）：
 *   - CR0/CR3/CR4/EFER/GDTR/IDTR/GS.base/FS.base/LSTAR/STAR/FMASK/PAT 都是
 *     **每逻辑处理器**寄存器。清 CR0.PG 只让本核退出 IA-32e，其它核继续用
 *     同一份 kernel_pagemap 跑 long mode，不受影响。
 *   - long mode 下硬件 VM86 不可用（EFLAGS.VM 仅在 32 位保护模式有效），
 *     因此必须真的 long -> compat32 -> protected16 -> real，再原路回来；
 *     本子系统不提供、也不假装提供"long mode 里的 v8086"。
 *   - 内核是 higher-half：关 PG 后 0xFFFF8... 立即不可访问，故整个切换
 *     过程必须在 identity-mapped 低 1 MiB 的 trampoline 里、用低内存栈，
 *     且切换期间不得调用任何内核 C++ 代码（this_cpu() 依赖 GS.base）。
 *
 * 分层：
 *   Tier0  g_cpu_offline_mask + 全路径 IPI/Steal/Preempt 过滤（lapic/smp/vmm）
 *   Tier1  kernel/modules/x86_64/dosvm/  真正的 long->real->long
 *   Tier2  kernel/modules/loadable/dosvm.cpp      动态 syscall 门面
 *   L3     programs/dos/main.cpp                  80x25 文本屏窗口客户端
 * ==========================================================================*/
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 80x25 DOS 文本屏快照（syscall 210 DOSVM_SYSCALL_READ）--------------
 * header(16B) + cells(4000B)。cells 为 (char, attr) 交替，行优先，
 * attr 是标准 VGA 属性字节（低 4 位前景 / 高 4 位背景）。
 * 定稿 (a')：4016 B 一次 CopyToUser，不用复合返回值。
 *
 * header 布局（2 x uint64，小端）：
 *   u64[0] = generation | (flags << 32)     低 32 位代次，高 32 位标志
 *   u64[1] = (trips << 32) | last_cpu       低 32 位上次 CPU，高 32 位往返数
 * generation 每次实模式程序成功返回并刷新影屏时 +1。 */
#define DOSVM_COLS          80u
#define DOSVM_ROWS          25u
#define DOSVM_CELLS         (DOSVM_COLS * DOSVM_ROWS * 2u)   /* 4000 */
#define DOSVM_HDR           16u                                /* 2 x uint64 */
#define DOSVM_BUFSZ         (DOSVM_HDR + DOSVM_CELLS)         /* 4016 */
#define DOSVM_FLAG_VALID    (1ull << 0)
#define DOSVM_FLAG_RUNNING  (1ull << 1)   /* 目标核正处于非 x64 窗口 */
#define DOSVM_FLAG_REAL_OK  (1ull << 2)   /* 上一次确实进过 real mode */
/* 16 位 DOS 环境（.COM 装载）状态，由 state 区投影到 header 的 flags。 */
#define DOSVM_FLAG_COM_MODE   (1ull << 3)  /* 装载区里有 .COM/.EXE，不是 banner */
#define DOSVM_FLAG_COM_EXITED (1ull << 4)  /* 上一次程序自己结束了 */
#define DOSVM_FLAG_WAITKEY    (1ull << 5)  /* 挂起等键，可交键后恢复 */
#define DOSVM_FLAG_WAIT_FS    (1ull << 6)  /* 挂起等宿主代执行文件请求 */

/* dosvm_enter 的正向返回码（0 之外）。用户态据此驱动"交键 / 代执行文件 /
 * 收工"三选一；负值仍是 errno。 */
#define DOSVM_RC_NEED_FS      1
#define DOSVM_RC_SUSPENDED    2

/* ---- 动态 syscall 槽（kernel.arch.dosvm 用 KCOMPONENT_EXPORT_SYSCALL 注册）---
 * Tier1/Tier2 合并后不再有 dosvm.ko；五个槽由同一个 resident DynModule 注册。
 * 注意：200u 已被 demo_syscall_v1/v2.ko 用作动态槽（虽非 BOOT，但 selftest
 * 会加载它们），为避免注册冲突改用 210..214。区间 [128,511] 合法。 */
#define DOSVM_SYSCALL_READ  210u   /* (user_buf, cap) -> 4016 / -EAGAIN / -EINVAL */
#define DOSVM_SYSCALL_ENTER 211u   /* (cpu_id, ...)   -> 0 / -errno；0 = 自动选 AP */
#define DOSVM_SYSCALL_STAGE 212u   /* (user_img, len, user_args, args_len) -> 0/-errno
                                    * 把 .COM 摆到 0x71100 并构造 PSP；len==0 卸载 */
#define DOSVM_SYSCALL_KEYS  213u   /* (user_keys, count) -> 0/-errno
                                    * 预置键盘队列，元素 u16 = (scan<<8)|ascii */
#define DOSVM_SYSCALL_MEM   214u   /* (off, user_buf, len, is_write) -> 0/-errno
                                    * 读写 0x70000 起的低内存窗口（唯一通道） */
/* 16 位 DOS 环境的挂起状态与控制流 -----------------------------------------
 * 实模式里 IF=0，没有异步事件，所以程序只能在自己调用 INT 21h/16h 的那一刻
 * 把整台机器交出去：上下文存进 state 区，trampoline 返回 64 位。dosvm_enter
 * 因此会带一个正向返回码回来，由用户态决定下一步：
 *   DOSVM_RC_NEED_FS    -> 用 SYSCALL_MEM 读请求块、执行真文件操作、写回结果
 *   DOSVM_RC_SUSPENDED  -> 用 SYSCALL_KEYS 交键，再 enter 一次即从原处继续
 *   DOSVM_RC_OK         -> 程序已结束（或 banner 演示跑完）
 * 这对 "支持 DOS 程序" 是硬约束而不是偷懒：VM86 在 IA-32e 下不可用，
 * 真实模式里没有任何抢占手段。 */

/* ---- 专用 IPI 向量：召唤目标 AP 执行 long->real->long 往返 --------------
 * 0xF0 位于 IDT 高位区（IRQ 只占 0x20..0x3F），不与 SCHED_VEC/TLB_FLUSH_VEC
 * 冲突。Tier1 用 idt_install_irq_cpu() 给每个非 BSP 核单独安装 handler；
 * LAPIC::IPI 的 offline 过滤必须对本向量放行（否则自己发的 IPI 也被吃掉）。 */
#define DOSVM_IPI_VEC       0xF0u

/* ---- trampoline 低物理内存布局（< 1 MiB）-------------------------------
 * vmm.cpp 已把低 4 GiB identity 映射（MM_READ|MM_WRITE，未置 MM_NX），
 * 故拷贝到物理 DOSVM_LOW_BASE 后，虚拟地址同值即可直接取指，无需新建页表。
 * 64 KiB 一整段由 Tier1 独占；real mode 段寻址用 seg = LOW_BASE >> 4。
 *
 * DOSVM_LOW_BASE 特意选 0x70000：它是 64 KiB 对齐的，低 16 位为 0，因此
 * 同一份 org=0x70000 的 flat binary 里，标签的低 16 位恰好等于实模式段内
 * 偏移，64/32/16 位三段代码都不需要任何重定位修正。
 *
 *   0x0000  trampoline blob (NASM -f bin, org 0x70000)，约 2.4 KiB。
 *           内含 64/32/16 位切换代码、16 位 DOS 环境（.COM 入口、INT
 *           21h/10h/16h/20h 服务、光标与滚屏）、临时 GDT 与 gdt_desc。
 *           blob 自带 GDT（位于 +0x940 附近），无需 C++ 另填。
 *   0x0C00  裸 IRET 字节（IVT 里未被接管的向量指向这里）+ 6 字节 IDTR
 *           映像；blob 变长后从 0x800 挪到这里，C++ 侧据此预填。
 *   0x1000  外部 .COM 映像加载区：PSP 在 0x71000，映像从 0x71100 开始，
 *           段基 = 0x7100（CS:IP = 7100:0100，正是 MS-DOS 的 .COM 语义）。
 *           加载器把映像长度卡在 0xC000 字节以内，避免写穿下面的 state。
 *           注意 .COM 的 64 KiB 段顶在 0x81000，越过了本块末尾 0x80000；
 *           PMM 因此把预留区扩到 0x84000（见 pmm.cpp），最坏是程序自己的
 *           栈盖掉影屏，而不会是写坏已发放出去的物理页。
 *   0x2000  保留：未来外部 GDT（当前 GDT 由 blob 自带）
 *   0x3000  real-mode IVT（0x400 B，物理 0x73000）。C++ 只预填 IDTR 与
 *           裸 IRET；0x20/0x21/0x10/0x16 四个向量由 trampoline 在进入实
 *           模式后自己接上（16 位侧算段内偏移最可靠）。
 *   0x4000  16/32/64 位共用的低内存栈（向下生长，顶 = 0x4FF0）
 *   ...
 *   0xE000  CPU 状态保存区（由 64 位入口填写；布局见 trampoline.nasm 头注）
 *   0xF000  80x25 影屏 4016 B：header 16 B（C++ 填）+ cells 4000 B
 *           （16 位 DOS payload 直接写这里）
 *
 * 低 1 MiB 在 QEMU e820 里属于 USABLE、会被 PMM 发放，故 PMM::Init() 显式
 * mark_allocated(0x70, 16) 预留这 64 KiB，避免与内核/模块分配冲突。
 */
#define DOSVM_LOW_BASE      0x00070000ull
#define DOSVM_LOW_SIZE      0x00010000ull   /* 64 KiB */
#define DOSVM_LOW_SEG       (DOSVM_LOW_BASE >> 4)   /* 0x7000 */
#define DOSVM_TRAMP_OFF     0x00000000ull
#define DOSVM_COM_OFF       0x00001000ull
#define DOSVM_GDT_OFF       0x00002000ull
#define DOSVM_IVT_OFF       0x00003000ull
#define DOSVM_STACK_OFF     0x00004000ull
#define DOSVM_STACK_TOP     0x00004FF0ull
#define DOSVM_STATE_OFF     0x0000E000ull
#define DOSVM_SHADOW_OFF    0x0000F000ull

/* 状态区魔数：trampoline 回程成功后写入，64 位侧据此确认"真的回来过"。 */
#define DOSVM_STATE_MAGIC   0x444F53564D544150ull  /* "DOSVMTAP" */

/* 临时 GDT 选择子（trampoline 内部使用，与内核 GDT 无关）。 */
#define DOSVM_SEL_NULL      0x00u
#define DOSVM_SEL_CODE32    0x08u   /* L=0, D=1, base=0, limit=0xffffffff */
#define DOSVM_SEL_DATA32    0x10u   /* D=1, base=0, limit=0xffffffff     */
#define DOSVM_SEL_CODE16    0x18u   /* D=0, base=0, limit=0xffff         */
#define DOSVM_SEL_DATA16    0x20u   /* D=0, base=0, limit=0xffff         */
#define DOSVM_SEL_CODE64    0x28u   /* L=1, D=0                          */

/* ---- Tier0：单核 offline 掩码（smp.cpp 实现，dosvm 组件使用）------------ */
extern volatile uint64_t g_cpu_offline_mask;
int  dosvm_core_offline(uint32_t cpu_id);   /* 置位并排空，返回 0 / -EBUSY */
void dosvm_core_online(uint32_t cpu_id);    /* 清位 */
int  dosvm_core_is_offline(uint32_t cpu_id);
/* LAPIC 层按 lapic_id 查询（供 IPI 底层统一过滤；0xF0 专用向量放行）。 */
int  dosvm_lapic_offline(uint32_t lapic_id);

/* ---- resident DynModule 实现、由 kernel_exports.cpp 加入 .kout 导出表 ----
 * provider = "kernel"，contract = "kernel.dosvm.v1"，abi = 1。
 * 外部 .ko 可用 KMOD_IMPORT_FN(..., "kernel", "<name>", "kernel.dosvm.v1")
 * 导入这些 C 级函数；syscall 210..214 已由本模块自己注册，不再需要门面。 */
#define DOSVM_KERNEL_CONTRACT "kernel.dosvm.v1"

int      dosvm_enter(uint32_t cpu_id);              /* 发 IPI 触发一次往返 */
long     dosvm_read(void *user_buf, uint64_t cap);  /* 4016 / 负 errno */
uint64_t dosvm_generation(void);
uint64_t dosvm_last_cpu(void);                      /* 上次执行往返的 CPU id */
uint64_t dosvm_trips(void);                         /* 成功往返次数 */
/* .COM 装载与键盘注入。实模式里 IF=0，没有活键盘与文件回调，所以映像和
 * 按键都必须在 trip 之前从用户态搬进低内存。 */
int      dosvm_stage_com(void *user_img, uint64_t len,
                         void *user_args, uint64_t args_len);
int      dosvm_push_keys(void *user_keys, uint64_t count);
int      dosvm_mem_rw(uint64_t off, void *user_buf, uint64_t len,
                      uint32_t is_write);

#ifdef __cplusplus
}
#endif
