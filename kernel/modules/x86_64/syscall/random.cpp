//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>
#include <klib/rnd.h>
#include <klib/kio.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <arch/x86_64/cpu/smap.h>
#include <kmod/kmod_seats.h>   /* Tier1 座位: 随机数提供者策略可插拔 */

#define GRND_NONBLOCK	0x0001
#define GRND_RANDOM	0x0002
#define GRND_INSECURE	0x0004

uint64_t sys_getrandom(uint64_t buf, uint64_t size, uint64_t flags,
    GENERATE_IGN3())
{
    IGNV_3();
	//struct iov_iter iter;
	//int ret; /* 修复: 未使用, 已删除 */

	if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
		return -EINVAL;

	/*
	 * Requesting insecure and blocking randomness at the same time makes
	 * no sense.
	 */
	if ((flags & (GRND_INSECURE | GRND_RANDOM)) == (GRND_INSECURE | GRND_RANDOM))
		return -EINVAL;

    /* 机制固定（syscall 入口/语义/错误码不变），策略可插拔：
       随机数来源是一个 Tier1 座位 (KMOD_SEAT_RANDOM)。默认提供者是 Tier0
       内建实现；持有 CAP_SEAT_PROVIDER 的模块可在治理事务里热替换它，
       切换瞬间由 epoch/看门狗保护，失败自动回退（见 docs/kernel-modules.md）。
       注：座位提供者若返回固定常量属于"演示/诊断"用途，治理层用能力
       与审计记录约束，而不是假装硬件隔离。 */
    /* 用户缓冲必须经 ua 校验/翻译：SmapGuard 只允许内核访问用户页，
       不校验地址是否属于用户空间。直接写 buf 等于"任意内核地址写
       size 字节"，因此按块填内核临时缓冲再 CopyToUser。 */
    if (size == 0)
        return 0;
    /* 单次调用上限：避免超大 size 把内核拖进长时间循环（DoS）。 */
    if (size > (16ull * 1024ull * 1024ull))
        return -EINVAL;

    pagemap_t *pm = Schedule::this_thread()->pagemap;
    uint8_t tmp[256];
    uint64_t done = 0;
    while (done < size) {
        uint64_t chunk = size - done;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        KmodSeatRandomFill(tmp, chunk);
        if (!VMM::UserAccess::CopyToUser(pm, buf + done, tmp, chunk))
            return done ? (uint64_t)done : (uint64_t)(int64_t)-EFAULT;
        done += chunk;
    }
    return done;
}
