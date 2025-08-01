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