//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
//#include <arch/x86_64/smp/smp.h>
#include <klib/klib.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/kio.h>
#include <errno.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscalln.h>
#include <klib/stallwatch.h>

static uint64_t 
SYSCALL_NULL(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,syscall_frame_t*){return NULL;}

/* kmod 治理哨兵（Tier0 固定入口 + Tier1 可注册实现）：
 * syscall 编号/参数/返回/错误码/语义固定；syscall_lists[] 只决定"实现"。
 * 治理槽上的条目统一指向哨兵，由 KmodSyscallEntry 按帧内编号查治理表，
 * 并负责模块 epoch（热替换/卸载期间排空在飞调用）与状态检查。 */
extern "C" void *KmodSyscallSentinel(void);
extern "C" uint64_t KmodSyscallEntry(void *frame);

__attribute__((aligned(64)))
uint64_t (*syscall_lists[512])(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,syscall_frame_t*) = \
    {SYSCALL_NULL};

void dump_REG(syscall_frame_t *frame){
    kinfoln("START DUMP REG");
    kinfoln("RAX:0x%X RBX:0x%X RCX:0x%X RDX:0x%X",frame->rax,frame->rbx,frame->rcx,frame->rdx);
    kinfoln("RSI:0x%X RDI:0x%X RBP:0x%X RSP:0x%X",frame->rsi,frame->rdi,frame->rbp,frame->rsp);
    kinfoln("R8:0x%X R9:0x%X R10:0x%X R11:0x%X",frame->r8,frame->r9,frame->r10,frame->r11);
    kinfoln("END DUMP REG");
}

extern "C" void syscall_handler(syscall_frame_t *frame) {
    /* 停滞看门狗: 系统调用也是"本核还在动"的证据, 而且卡在自旋锁里时
       (syscall_entry 先 cli, 中断不来) 只有这里能刷新现场 —— 缺了这一步,
       看门狗看到的是上一次中断的老快照, 会把现场读成垃圾。 */
    stall_note_entry(frame->rip, frame->rsp, frame->rflags, 0, 1);
    stall_set_kernel_busy(1);

    if (frame->rax >= 512 || syscall_lists[frame->rax] == nullptr) {
        stall_set_kernel_busy(0);
        frame->rax = -ENOSYS; 
        return;
    }

    auto func = syscall_lists[frame->rax];

    //kinfoln("Attempting to call index %ld at addr 0x%p", frame->rax, func);
    /* kmod 治理槽：入口/编号/参数/返回/错误码固定，只有"实现"可注册/替换。
       哨兵统一入口负责治理表查找、模块 epoch 与 -ENOSYS/-EAGAIN 语义。 */
    if (__builtin_expect(func == (decltype(func))KmodSyscallSentinel(), 0)) {
        frame->rax = KmodSyscallEntry(frame);
        stall_set_kernel_busy(0);
        return;
    }
    /* 返回值只写进**帧槽** —— user.asm 的收尾是从帧槽读回 %rax 交给用户态
       (不再依赖"函数返回时 %rax 还是返回值"; %rax 是 caller-saved, 本函数
       返回前的任何一步都会踩掉它, 那正是"内核返回 0x320、用户态收到 0/1"
       这个 bug 的根源, 详见 docs/debug-hang-2026-10-11.md 第 11 节)。 */
    frame->rax = func(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9, frame);
    stall_set_kernel_busy(0);

    //kinfoln("Syscall returned %ld", frame->rax);
}

#include <klib/algorithm/queue.h>
extern "C" void syscall_entry();

// RFLAGS 关键标志位
#define RFLAGS_TF   (1 << 8)  // Trap Flag (单步调试)
#define RFLAGS_IF   (1 << 9)  // Interrupt Flag (中断响应)
#define RFLAGS_DF   (1 << 10) // Direction Flag (方向标志)
#define RFLAGS_IOPL (3 << 12) // I/O Privilege Level
#define RFLAGS_NT   (1 << 14) // Nested Task
#define RFLAGS_AC   (1 << 18) // Alignment Check

/*
syscall_lists[x] = sys_xxxx //! <---- it means some feature in this syscall doesn't complete!
syscall_lists[x] = sys_xxxx //? <---- it means i don't know how to impl this syscall
*/
/* time.cpp: wall-clock seconds derived from the RTC; exposed to userspace as
   syscall 17 (SYSCALL_TIME). Calling it with a null buffer just returns the
   current civil-time second count (see sys_time in syscall/time.cpp). */
uint64_t sys_time(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,syscall_frame_t*);

void syscall_init() {
    /* 固定槽 0..63 的实现不再在 Tier0 硬编码：由内建组件 kernel.syscall.*
     * （kernel/src/arch/x86_64/components/syscall_*.cpp）在
     * KmodBootComponents() 阶段通过导出表 kind=KMOD_SYM_SYSCALL 注册；
     * 槽位哨兵由 KmodSyscallInit() 安装（0..63 + 128..511），dispatch 在
     * syscall_handler 里经哨兵进入治理表（编号/参数/返回/错误码/语义不变）。
     * 本函数只保留 CPU 侧 syscall 入口装配（EFER/STAR/LSTAR/FMASK）。
     * 29(SYSCALL_KMOD_INFO)/30(SYSCALL_KMOD_PROBE) 的实现仍在 Tier0
     * syscall_registry.cpp 内部分派。 */

    __builtin_prefetch((const void*)syscall_lists,0,0);
    

    uint64_t efer = rdmsr(IA32_EFER);
    efer |= (1 << 0);
    wrmsr(IA32_EFER, efer);
    uint64_t star = 0;
    star |= ((uint64_t)0x08 << 32);
    star |= ((uint64_t)0x13 << 48);
    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    uint64_t fmask = RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_IOPL | RFLAGS_NT | RFLAGS_AC;
    wrmsr(IA32_FMASK, fmask);
    kinfo("User INIT(syscall_init): enabled.\n");
}