#pragma once
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <libfg.h>

uint64_t syscall6(uint64_t num,uint64_t a, \
    uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f);
uint64_t syscall5(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c,uint64_t d,uint64_t e);
uint64_t syscall4(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c,uint64_t d);
uint64_t syscall3(uint64_t num,uint64_t a, \
uint64_t b,uint64_t c);
uint64_t syscall2(uint64_t num,uint64_t a, \
uint64_t b);
uint64_t syscall1(uint64_t num,uint64_t a);
uint64_t syscall0(uint64_t num);



#endif