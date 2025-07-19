#include <arch/x86_64/allin.h>
#include <limine.h>

extern struct limine_smp_response* smp_response;

u64 smp_cpu_started = 0;
cpu_info* smp_cpu_list[128];

cpu_info* get_cpu(u64 lapic_id) {
    return smp_cpu_list[lapic_id];
}

cpu_info* this_cpu() {
    return smp_cpu_list[LAPIC::GetID()];
}

atomic_lock smp_lock;

void smp_init_cpu(struct limine_smp_info* smp_info) {
    lock(&smp_lock);

    gdt_init();
    idt_reinit();
    vmm_switch_pm_nocpu(vmm_kernel_pm);

    void* stack = HIGHER_HALF(PMM::Alloc(3)) + (3 * PAGE_SIZE);
    tss_list[smp_info->lapic_id].rsp[0] = (u64)stack;
    
    cpu_info* c = (cpu_info*)kmalloc(sizeof(cpu_info));
    _memset(c, 0, sizeof(cpu_info));
    c->lapic_id = smp_info->lapic_id;
    c->pm = vmm_kernel_pm;

    c->proc_list = List::Create();

    smp_cpu_list[smp_info->lapic_id] = c;

    vmm_switch_pm(vmm_kernel_pm);

    LAPIC::Init();
    LAPIC::CalibrateTimer();

    sse_enable();
    fpu_init();

    
    user_init();
    Schedule::Init();

    kinfo("   smp_init_cpu(): CPU %ld started.\n", smp_info->lapic_id);
    smp_cpu_started++;

    unlock(&smp_lock);

    LAPIC::IPI(smp_info->lapic_id, 0x80);

    while (true) {hcf();}
}

bool smp_started = false;

void smp_init() {
    kinfo("    smp_init(): bsp_lapic_id: %d.\n", bsp_lapic_id);

    cpu_info* c = (cpu_info*)kmalloc(sizeof(cpu_info));
    _memset(c, 0, sizeof(cpu_info));
    c->pm = vmm_kernel_pm;
    c->proc_list = List::Create();

    smp_cpu_list[0] = c;
    vmm_switch_pm(vmm_kernel_pm);

    for (u64 i = 0; i < smp_cpu_count; i++)
        if (smp_response->cpus[i]->lapic_id != bsp_lapic_id)
        smp_response->cpus[i]->goto_address = smp_init_cpu;
    
    while (smp_cpu_started < smp_cpu_count - 1)
        __asm__ volatile ("nop");

    
}