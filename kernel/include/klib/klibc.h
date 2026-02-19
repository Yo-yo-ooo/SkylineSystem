#pragma once
#ifndef _KILBC_H_
#define _KILBC_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <klib/klib.h>
#else
extern void _memcpy(void* src, void* dest, uint64_t size);
extern void _memset(void* dest, uint8_t value, uint64_t size);
extern void _memmove(void* dest,void* src, uint64_t size);
extern int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  count);

extern void* kmalloc(uint64_t size);
extern void kfree(void* ptr);
extern void* krealloc(void* ptr, uint64_t size);
extern void *kcalloc(size_t numitems, size_t size);
#endif

#endif