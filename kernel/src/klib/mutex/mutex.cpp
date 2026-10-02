//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <klib/mutex/mutex.h>
#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/smp/smp.h>
#endif

#ifdef __x86_64__
/* P0-1 修复: 唤醒阻塞线程必须重新入队 + 触发抢占。
   原实现只改 state=RUNNING —— 线程已不在任何运行队列 (Acquire 的
   BLOCKED 让 switch 路径不重插), 被唤醒后永久丢失 → 并发下挂死。 */
static void wake_thread(thread_t *t)
{
    if (!t) return;
    uint32_t c = __atomic_load_n(&t->cpu_num, __ATOMIC_ACQUIRE);
    if (c >= MAX_CPU) return;
    cpu_t *cpu = smp_cpu_list[c];
    if (!cpu) return;
    uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_BLOCKED) {
        t->state = THREAD_RUNNING;
        Schedule::Internal::InsertToQueue(cpu, t);
    }
    spin_unlock_irqrestore(&cpu->sched_lock, rflags);
    Schedule::TriggerPreempt(t);
}
#endif


mutex_t *MutexCreate(void)
{
    mutex_t *mutex = (mutex_t*)kmalloc(sizeof(mutex_t));
    if (!mutex)
        return NULL;

    mutex->owner = NULL;
    mutex->lock = 0;
    mutex->queue = Queue::Create();

    // 队列创建失败，兜底释放内存
    if (!mutex->queue) {
        kfree(mutex);
        return NULL;
    }

    return mutex;
}

void MutexDestroy(mutex_t *mutex)
{
#ifdef __x86_64__
    if (!mutex)
        return;

    // 保险：释放锁并唤醒所有等待线程，避免死锁
    __sync_lock_release(&mutex->lock);
    mutex->owner = NULL;

    thread_t *thread;
    while ((thread = (thread_t*)Queue::Dequeue(mutex->queue)) != NULL) {
        wake_thread(thread);
    }

    // 销毁队列 + 释放自身内存
    Queue::Destroy(mutex->queue);
    kfree(mutex);
#endif
}

void MutexAcquire(mutex_t *mutex)
{
    ASSERT(mutex != NULL);

#ifdef __x86_64__
    thread_t *curr = Schedule::this_thread();

    // 循环抢锁，失败则阻塞等待，唤醒后重试
    while (__sync_lock_test_and_set(&mutex->lock, 1)) {
        // 入队前设置阻塞状态，保证调度原子性
        curr->state = THREAD_BLOCKED;
        Queue::Append(mutex->queue, curr);
        Schedule::Yield();
        // 唤醒后回到循环开头，重新抢锁
    }

    mutex->owner = curr;
#endif
}

void MutexRelease(mutex_t *mutex)
{
    ASSERT(mutex != NULL);

#ifdef __x86_64__
    ASSERT(mutex->owner == Schedule::this_thread());

    mutex->owner = NULL;
    __sync_lock_release(&mutex->lock);

    // 唤醒队首第一个等待线程 (重新入队 + 抢占, P0-1)
    thread_t *wait_thread = (thread_t*)Queue::Dequeue(mutex->queue);
    wake_thread(wait_thread);
#endif
}