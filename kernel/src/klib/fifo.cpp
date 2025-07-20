#include <klib/klib.h>
#include <mem/heap.h>
#include <klib/fifo.h>

;
namespace FIFO
{
    fifo *Create(u64 cap, u64 item_size)
    {
        fifo *f = (fifo *)kmalloc(sizeof(fifo));
        f->cap = cap;
        f->data = (void **)kmalloc(cap * item_size);
        _memset(f->data, 0, cap * item_size);
        f->item_size = item_size;
        f->idx = 0;
        f->count = 0;
        return f;
    }

    void Push(fifo *f, void *val)
    {
        spinlock_lock(&f->lock);
        if (f->count == f->cap)
        {
            spinlock_unlock(&f->lock);
            return;
        }
        __memcpy(f->data + (f->count * f->item_size), val, f->item_size);
        f->count++;
        spinlock_unlock(&f->lock);
    }

    void Pop(fifo *f, void *buffer)
    {
        spinlock_lock(&f->lock);
        if (f->count == 0)
        {
            spinlock_unlock(&f->lock);
            _memset(buffer, 0, f->item_size);
            return;
        }
        __memcpy(buffer, f->data + (f->idx * f->item_size), f->item_size);
        f->idx++;
        if (f->idx == f->count)
        {
            f->idx = 0;
            f->count = 0;
            _memset(f->data, 0, f->item_size * f->cap);
        }
        spinlock_unlock(&f->lock);
    }

    void Get(fifo *f, void *buffer)
    {
        __memcpy(buffer, (f->data + (f->idx * f->item_size)), f->item_size);
    }
}