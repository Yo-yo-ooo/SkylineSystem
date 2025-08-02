#pragma once
#ifndef x86MEM_ENABLE_H
#define x86MEM_ENABLE_H

#include <stdint.h>
#include <stddef.h>
#include <klib/kio.h>

#ifdef __x86_64__


int32_t xstate_enable(void);
int32_t avx_enable(void);
int32_t avx512_enable(void);



#else
#error "You include x86_64 ARCH file,but your ARCH not x86_64"
#endif


#endif