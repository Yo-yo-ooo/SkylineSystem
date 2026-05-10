#ifndef __ATOMIC_ARCH_H
#define __ATOMIC_ARCH_H

#define __INTERNAL_ATOMIC_EXTEND

#include <arch/x86_64/atomic/__a_and_64.h> 
#include <arch/x86_64/atomic/__a_and.h>
#include <arch/x86_64/atomic/__a_barrier.h>
#include <arch/x86_64/atomic/__a_cas_p.h>
#include <arch/x86_64/atomic/__a_cas.h>
#include <arch/x86_64/atomic/__a_clz_64.h>
#include <arch/x86_64/atomic/__a_crash.h>
#include <arch/x86_64/atomic/__a_ctz_64.h>
#include <arch/x86_64/atomic/__a_dec.h>
#include <arch/x86_64/atomic/__a_fetch_add.h>
#include <arch/x86_64/atomic/__a_inc.h>
#include <arch/x86_64/atomic/__a_or_64.h>
#include <arch/x86_64/atomic/__a_or.h>
#include <arch/x86_64/atomic/__a_spin.h>
#include <arch/x86_64/atomic/__a_store.h>
#include <arch/x86_64/atomic/__a_swap.h>

#undef __INTERNAL_ATOMIC_EXTEND

#endif
