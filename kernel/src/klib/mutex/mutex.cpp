#include <klib/mutex/mutex.h>
#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/schedule/sched.h>
#endif

namespace Mutex{
    mutex_t *Create(){
        mutex_t *mutex = (mutex_t*)kmalloc(sizeof(mutex_t));
        mutex->owner = NULL;
        mutex->queue = Queue::Create();
        mutex->lock = 0;
        return mutex;
    }

    void mutex_acquire(mutex_t *mutex) {
#ifdef __x86_64__
        thread_t *thread = Schedule::this_thread();
        if (__sync_lock_test_and_set(&mutex->lock, 1)) {
            // Enqueue this thread to wait to be the owner.
            thread->state = THREAD_BLOCKED;
            Queue::Append(mutex->queue, thread);
            Schedule::Yield();
        }
        mutex->owner = Schedule::this_thread();
#endif
    }

    void mutex_release(mutex_t *mutex) {
#ifdef __x86_64__
        ASSERT(mutex->owner == Schedule::this_thread());
        mutex->owner = NULL;
        __sync_lock_release(&mutex->lock);
        // Wake up next thread on the owner queue.
        thread_t *thread = Queue::Dequeue(mutex->queue);
        if (!thread)
            return;
        thread->state = THREAD_RUNNING;
#endif
    }
}