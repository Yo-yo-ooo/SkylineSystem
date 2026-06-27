//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <klib/algorithm/queue.h>
#include <klib/klib.h>

namespace Queue{
    queue_t *Create() {
        queue_t *queue = (queue_t*)kmalloc(sizeof(queue_t));
        queue->head = queue->tail = NULL;
        return queue;
    }
    queue_item_t *Append(queue_t *queue, void *data){
        queue_item_t *it = (queue_item_t*)kmalloc(sizeof(queue_item_t));
        it->data = data;
        it->next = NULL;

        if (!queue->head)
            queue->head = queue->tail = it;
        else {
            queue->tail->next = it;
            queue->tail = it;
        }
        return it;
    }
    void *Dequeue(queue_t *queue) {
        queue_item_t *it = queue->head;
        if (!it)
            return NULL;
        queue->head = it->next;
        if (queue->tail == it)
            queue->tail = NULL;
        void *data = it->data;
        kfree(it);
        return data;
    }

    void Destroy(queue_t *queue) {
        if (!queue)
            return;

        queue_item_t *curr = queue->head;
        while (curr) {
            queue_item_t *next = curr->next;
            kfree(curr);
            curr = next;
        }

        kfree(queue);
    }
}