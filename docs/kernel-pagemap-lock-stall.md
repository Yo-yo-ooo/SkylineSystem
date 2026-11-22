<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# kernel_pagemap vma_lock stall

> 现象：桌面进程已加载、控制台 PID 已拉起，随后 CPU 0 停在系统调用里，
> 串口连续出现 STALL WATCHDOG，桌面永远不出现完整画面。
>
> 用户现场：
>
> ~~~text
> [OK] LOADED PID:9
> [INFO] PROC->ID 9
> [INFO] OK! REMOVE SYSINFO MAPPING!
> [KERR] STALL WATCHDOG: CPU 0 has not run for 3143 ms (reported by CPU 1)
> [KERR]   last entry : rip=0000700000006B80 ... vec=0 (syscall)
> [KERR]   interrupts : ENABLED
> [KERR]   waiting on : lock=FFFF800000084010 (acquired at FFFFFFFF80034DA7)
> ~~~

## 1. 地址解码

- 0000700000006B80 是 libsky.so 的 sys_load 用户入口；
- FFFFFFFF80034DA7 用 addr2line 解到 VMM::Alloc(pagemap_t*, unsigned long, bool)
  （vmm.cpp:1070），即 spinlock_lock(&pm->vma_lock)；
- 锁地址 FFFF800000084010 对应物理 0x84010。启动日志新增：

  ~~~text
  VMM: kernel_pagemap=FFFF800000084000 phys=0x84000
  ~~~

  因此被等的是 kernel_pagemap->vma_lock（pagemap_t 偏移 0x10），
  不是某个用户进程的 pagemap。

## 2. 根因

sys_load() 在装载新客户端时会执行：

~~~c
thread->fx_area = VMM::Alloc(kernel_pagemap, ...);
kernel_stack   = VMM::Alloc(kernel_pagemap, 4, false);
~~~

VMM::Alloc 的 kernel_pagemap 路径持有 kernel_pagemap->vma_lock（以及
pt_lock）完成 VMA 预留、PMM 分配和页表映射。原来的临界区使用裸
spinlock_lock：

- 不关中断：timer IRQ 可以在持锁线程处于 VMM::Alloc 内部时打断它；
- 不增加 preempt_count：时钟 tick 的调度器可以把这个持锁线程换出；
- kernel_pagemap 是全局的：换入的线程/中断处理路径只要也走
  VMM::Alloc(kernel_pagemap, ...)（内核栈、fx_area、kmalloc 扩容等），
  就会在同一 CPU 上尝试拿同一把锁。

两种等价的自锁形态：

1. IRQ 重入：持锁线程被 timer 打断，IRQ 处理路径再次 VMM::Alloc
   kernel_pagemap，同一 CPU 自旋在同一把锁上；
2. 抢占：持锁线程被换出，另一个线程在同一 CPU 上抢锁；持锁者虽然
   runnable，但等待者在自旋，调度收敛很慢，表现为数秒到十几秒 STALL。

这解释了为什么锁地址总是 kernel_pagemap：客户端装载的 kernel stack /
fx_area 分配正好命中这把全局锁。

## 3. 修复

在 kernel/src/arch/x86_64/vmm/vmm.cpp 增加 KpmIrqGuard：只有
pm == kernel_pagemap 时关中断，函数结束恢复原 IF。把它放在：

- VMM::Alloc
- VMM::EAlloc
- VMM::Free

的入口。这样 kernel_pagemap 的 VMA/页表临界区不会被 timer IRQ 重入，
也不会被调度器抢占；用户 pagemap 的临界区保持原样，避免 ELF 大段映射
长时间关中断。

~~~c
struct KpmIrqGuard {
    bool active;
    bool prev;
    explicit KpmIrqGuard(pagemap_t *pm)
        : active(pm == kernel_pagemap), prev(false) {
        if (active) prev = Interrupt::SaveMask();
    }
    ~KpmIrqGuard() { if (active) Interrupt::Restore(prev); }
};
~~~

启动日志保留一行：

~~~text
VMM: kernel_pagemap=FFFF800000084000 phys=0x84000
~~~

以后出现同类锁地址可以直接判断是 kernel 还是用户 pagemap。

## 4. 验证

修复后（QEMU TCG，2G，4 vCPU）：

~~~text
kmod_smoke.ps1:        KMOD SMOKE: PASS
  KMOD_SELFTEST PASS 13/13
  desktop PROCESS: 6
client_churn_gate.py --cycles 5:
  C1..C6 全 PASS（spawn=5/5, exit=5/5, 0 STALL/#PF/OOM）
dosvm_trip_gate.py:    PASS
  [dosvm] trip OK: cpu=1 gen=1 trips=1
~~~

额外 5 次 35 秒启动（4 vCPU，-snapshot）：desktop ready=True,
clients=3, STALL WATCHDOG=0；2 vCPU 3 次启动同样 0 stall。

## 5. 边界

- 这是内核自旋锁的抢占/IRQ 语义问题，不是 SAF 模块加载本身；SAF 只是
  让模块更多、启动期堆/映射活动更密集，从而更容易撞上该窗口。
- 修复只覆盖 kernel_pagemap 的三条 VMM 路径；用户 pagemap 的 VMA 操作仍
  使用原有 spinlock 语义（同地址空间内竞争，不会跨进程触发全局锁）。
- 如果后续出现用户 pagemap 的同类等待，应按同样方法检查该临界区是否
  被 timer IRQ/调度抢占，而不是继续扩大 kernel_pagemap 的关中断范围。
