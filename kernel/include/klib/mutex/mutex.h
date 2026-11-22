// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only

#pragma once
#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <stdint.h>
#include <stddef.h>
#include <klib/algorithm/queue.h>

typedef struct {
    void *owner;
    queue_t *queue;
    int32_t lock;
} mutex_t;


mutex_t *MutexCreate();
void MutexAcquire(mutex_t *mutex);
void MutexRelease(mutex_t *mutex);
void MutexDestroy(mutex_t *mutex);

#endif