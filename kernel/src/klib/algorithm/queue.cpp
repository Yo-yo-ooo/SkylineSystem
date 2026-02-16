/*
* SPDX-License-Identifier: GPL-2.0-only
* File: queue.cpp
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
}