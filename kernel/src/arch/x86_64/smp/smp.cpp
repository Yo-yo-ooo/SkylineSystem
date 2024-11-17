#include "smp.h"
#include "../lapic/lapic.h"

u64 smp_cpu_started = 0;
cpu_info* smp_cpu_list[128];

cpu_info* get_cpu(u64 lapic_id) {
    return smp_cpu_list[lapic_id];
}

cpu_info* this_cpu() {
    return smp_cpu_list[LAPIC::GetID()];
}