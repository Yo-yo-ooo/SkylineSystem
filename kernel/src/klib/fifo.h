#pragma once

#ifndef _FIFO_H
#define _FIFO_H

#include <stddef.h>
#include <stdint.h>

#include "../klib/klib.h"

typedef struct {
    atomic_lock lock;
    u64 cap;
    void** data;
    u64 item_size;
    u64 idx;
    u64 count;
} fifo;

namespace FIFO
{
    fifo* Create(u64 cap, u64 item_size);
    void Push(fifo* fifo, void* val);
    void Pop(fifo* fifo, void* buffer);
    void Get(fifo* fifo, void* buffer);
} // namespace FIFO


#endif