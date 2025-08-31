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

namespace Mutex{
    mutex_t *Create();
    void Acquire(mutex_t *mutex);
    void Release(mutex_t *mutex);
}
#endif