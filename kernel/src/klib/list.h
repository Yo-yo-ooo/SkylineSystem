#pragma once

#include "klib.h"
#include "types.h"
#include "../mem/heap.h"

#define SCHED_STACK_SIZE (4 * PAGE_SIZE)

typedef struct list_item {
    void* val;
    struct list_item* next;
    struct list_item* prev;
} list_item;

typedef struct list {
    atomic_lock lock;
    u64 count;
    list_item* head;
    list_item* iterator;
} list;

namespace List{
    list* Create();
    void Add(list* l, void* v);
    void Remove(list* l, list_item* item);
    void* Iterate(list* l, bool wrap);
    list_item* Find(list* l, void* v);
}