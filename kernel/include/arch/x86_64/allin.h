#pragma once

#include <klib/kprintf.h>
#include <klib/klib.h>
#include <klib/kio.h>
#include <conf.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/dev/pci/pci.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/MStack/MStackM.h>
#include <arch/x86_64/MStack/MStackS.h>
#include <arch/x86_64/rtc/rtc.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
