#pragma once
#ifndef KERNEL_QUEUE_H
#define KERNEL_QUEUE_H

#include <stdint.h>
#include <stddef.h>

typedef struct queue_item_t {
    void *data;
    struct queue_item_t *next;
} queue_item_t;

typedef struct {
    queue_item_t *head;
    queue_item_t *tail;
} queue_t;

#ifdef __cplusplus
namespace Queue{
    queue_t *Create();

    queue_item_t *Append(queue_t *queue, void *data);
    void *Dequeue(queue_t *queue);
}
#endif

#endif