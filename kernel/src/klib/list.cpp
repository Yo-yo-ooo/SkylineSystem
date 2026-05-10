/*
* SPDX-License-Identifier: GPL-2.0-only
* File: list.cpp
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
#include <klib/list.h>


namespace List{

    list* Create() {
        list* l = (list*)kmalloc(sizeof(list));
        l->head = (list_item*)kmalloc(sizeof(list_item));
        l->head->next = l->head;
        l->head->prev = l->head;
        l->iterator = l->head;
        l->count = 0;
        spinlock_unlock(&l->lock);
        return l;
    }

    void Add(list* l, void* val) {
        spinlock_lock(&l->lock);
        list_item* item = (list_item*)kmalloc(sizeof(list_item));
        if (!item) {
            spinlock_unlock(&l->lock);
            return;
        }
        item->val = val;
        item->prev = l->head->prev;
        item->next = l->head;
        l->head->prev->next = item;
        l->head->prev = item;
        l->count++;
        spinlock_unlock(&l->lock);
    }

    void Remove(list* l, list_item* item) {
        spinlock_lock(&l->lock);
        item->next->prev = item->prev;
        item->prev->next = item->next;
        l->count--;
        spinlock_unlock(&l->lock);
    }

    void* Iterate(list* l, bool wrap) {
        if (l->count == 0) return NULL;
        spinlock_lock(&l->lock);
        l->iterator = l->iterator->next;
        if (l->iterator == l->head) {
            if (wrap) {
            l->iterator = l->head->next;
            } else {
            spinlock_unlock(&l->lock);
            return NULL;
            }
        }
        spinlock_unlock(&l->lock);
        return l->iterator->val;
    }

    list_item* Find(list* l, void* v) {
        if (l->count == 0) return NULL;
        spinlock_lock(&l->lock);
        for (list_item* item = l->head->next; item != l->head; item = item->next) {
            if (item->val == v) {
            spinlock_unlock(&l->lock);
            return item;
            }
        }
        spinlock_unlock(&l->lock);
        return NULL;
    }

}