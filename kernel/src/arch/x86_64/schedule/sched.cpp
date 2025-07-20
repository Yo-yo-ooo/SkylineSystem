#include <arch/x86_64/allin.h>
#include <elf/elf.h>


namespace Schedule{

    u64 sched_pid = 0;
    spinlock_t sched_lock = 0;
    list* sched_sleep_list = NULL;


    process* GetNextProc(cpu_info* c) {
        spinlock_lock(&c->sched_lock);
        struct process* proc;
        while (true) {
            proc = (struct process*)List::Iterate(c->proc_list, true);
            if (proc->threads->count > 0)
                break;
        }
        spinlock_unlock(&c->sched_lock);
        return proc;
    }

    thread* GetNextThread(process* proc) {
        spinlock_lock(&proc->lock);
        struct thread* t;
        while (true) {
            t = (struct thread*)List::Iterate(proc->threads, false);
            if (t == NULL) {
                get_cpu(proc->cpu)->scheduled_threads = true;
                break;
            }
            if (t->state == SCHED_RUNNING) {
            break;
            }
        }
        spinlock_unlock(&proc->lock);
        return t;
    }

    void Init(){
        if (!sched_sleep_list)
            sched_sleep_list = List::Create();
        cpu_info* cpu = this_cpu();
        cpu->idle_proc = Schedule::NewProc("Idle", SCHED_IDLE, cpu->lapic_id, false);
        Schedule::ProcAddThread(cpu->idle_proc, (void*)hcf, false);
        cpu->proc_idx = -1;
        cpu->thread = NULL;
        cpu->proc = NULL;
    }

    void Schedule(registers* r){
        LAPIC::StopTimer();

        cpu_info* c = this_cpu();

        if (c->thread) {
            thread* t = c->thread;
            t->ctx = *r;
            t->pm = c->pm;
            t->gs = read_kernel_gs();
            __asm__ volatile ("fxsave %0" : : "m"(t->fxsave));
        } else {
            c->proc = GetNextProc(c);
        }

        // Find a suitable thread
        thread* next_thread = GetNextThread(c->proc);
        while (c->scheduled_threads) {
            c->proc = GetNextProc(c);
            next_thread = GetNextThread(c->proc);
            c->scheduled_threads = false;
        }

        c->thread = next_thread;

        *r = c->thread->ctx;
        VMM::SwitchPM(c->thread->pm);
        write_kernel_gs((u64)c->thread);

        __asm__ volatile ("fxrstor %0" : : "m"(c->thread->fxsave));

        LAPIC::EOI();
        LAPIC::Oneshot(0x80, 5);
    }

    process* NewProc(char* name, u8 type, u64 cpu, bool child) {
        cpu_info* c = get_cpu(cpu);
        if (!c) return NULL;

        process* proc = (process*)kmalloc(sizeof(process));
        if (!proc) {
            return NULL;
        }
        _memset(proc, 0, sizeof(proc));

        proc->cpu = cpu;

        proc->pm = (child ? NULL : vmm_new_pm());

        proc->type = type;

        //proc->current_dir = vfs_root;
        //proc->fds[0] = fd_open(kb_node, FS_READ, 0);
        //proc->fds[1] = fd_open(tty_node, FS_WRITE, 1);
        //proc->fds[2] = fd_open(tty_node, FS_WRITE, 2);
        //kinfo("New proc step 1 done.\n");

        proc->threads = List::Create();

        proc->name = (char*)kmalloc((u64)strlen(name));
        
        __memcpy(proc->name, name, (size_t)strlen(name));
        //kinfo("New proc step 2 done.\n");

        proc->tidx = -1;
        proc->scheduled = false;

        proc->idx = c->proc_list->count;
        proc->pid = sched_pid++; // TODO: hash this

        

        spinlock_lock(&c->sched_lock);
        List::Add(c->proc_list, proc);
        spinlock_unlock(&c->sched_lock);

        kinfo("New proc %lu created.\n", proc->pid);
        return proc;
    }

    thread* ProcAddThread(process* proc, void* entry, bool fork) {
        thread* t = (thread*)kmalloc(sizeof(thread));
        if (!t) return NULL;
        _memset(t, 0, sizeof(thread));

        t->pm = proc->pm;
        t->heap_area = Heap::Create(proc->pm);

        char* kstack = (char*)kmalloc(SCHED_STACK_SIZE);
        char* stack = NULL;

        if (proc->type == SCHED_USER && !fork) {
            pagemap* pm = this_cpu()->pm;
            vmm_switch_pm(t->pm);
            stack = (char*)Heap::Alloc(t->heap_area, SCHED_STACK_SIZE);
            _memset(stack, 0, SCHED_STACK_SIZE);
            vmm_switch_pm(pm);
        } else if (!fork) {
            stack = (char*)kmalloc(SCHED_STACK_SIZE);
            _memset(stack, 0, SCHED_STACK_SIZE);
        }

        t->stack_base = (u64)(stack + SCHED_STACK_SIZE);
        t->kernel_stack = (u64)(kstack + SCHED_STACK_SIZE);

        t->stack_bottom = (u64)stack;
        t->kstack_bottom = (u64)kstack;

        __asm__ volatile ("fxsave %0" : : "m"(t->fxsave));

        t->gs = 0;

        t->ctx.rip = (u64)entry;
        t->ctx.rsp = (u64)t->stack_base;
        t->ctx.cs  = (proc->type == SCHED_USER ? 0x43 : 0x28);
        t->ctx.ss  = (proc->type == SCHED_USER ? 0x3B : 0x30);
        t->ctx.rflags = 0x202;

        t->sleeping_time = 0;

        t->state = (fork ? SCHED_BLOCKED : SCHED_RUNNING);
        t->idx = proc->threads->count;
        t->parent = proc;

        spinlock_lock(&proc->lock);
        List::Add(proc->threads, t);
        spinlock_unlock(&proc->lock);

        return t;
    }

    thread* ProcAddELFThread(process* proc, char* path){
        return nullptr;
    }
}