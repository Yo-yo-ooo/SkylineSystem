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


mutex_t *MutexCreate(){
    mutex_t *mutex = (mutex_t*)kmalloc(sizeof(mutex_t));
    mutex->owner = NULL;
    mutex->queue = Queue::Create();
    mutex->lock = 0;
    return mutex;
}

void MutexAcquire(mutex_t *mutex) {
#ifdef __x86_64__
    thread_t *thread = Schedule::this_thread();
    if (__sync_lock_test_and_set(&mutex->lock, 1)) {
        // Enqueue this thread to wait to be the owner.
        thread->state = THREAD_BLOCKED;
        Queue::Append(mutex->queue, thread);
        Schedule::Yield();
    }
    mutex->owner = Schedule::this_thread();
#endif
}

void MutexRelease(mutex_t *mutex) {
#ifdef __x86_64__
    ASSERT(mutex->owner == Schedule::this_thread());
    mutex->owner = NULL;
    __sync_lock_release(&mutex->lock);
    // Wake up next thread on the owner queue.
    thread_t *thread = Queue::Dequeue(mutex->queue);
    if (!thread)
        return;
    thread->state = THREAD_RUNNING;
#endif
}
