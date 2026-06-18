/*
* SPDX-License-Identifier: GPL-2.0-only
* File: mutex.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <klib/mutex/mutex.h>
#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/schedule/sched.h>
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
    if (!mutex)
        return;

    // 保险：释放锁并唤醒所有等待线程，避免死锁
    __sync_lock_release(&mutex->lock);
    mutex->owner = NULL;

    thread_t *thread;
    while ((thread = (thread_t*)Queue::Dequeue(mutex->queue)) != NULL) {
        thread->state = THREAD_RUNNING;
    }

    // 销毁队列 + 释放自身内存
    Queue::Destroy(mutex->queue);
    kfree(mutex);
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

    // 唤醒队首第一个等待线程
    thread_t *wait_thread = (thread_t*)Queue::Dequeue(mutex->queue);
    if (wait_thread) {
        wait_thread->state = THREAD_RUNNING;
    }
#endif
}