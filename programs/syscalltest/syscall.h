#pragma once
#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

long syscall(long number,long arg1,long arg2, \
long arg3,long arg4,long arg5,long arg6);

#ifdef __cplusplus
}
#endif


#endif