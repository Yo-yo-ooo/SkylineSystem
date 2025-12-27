#pragma once
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <libfg.h>

long syscall(long number,long arg1,long arg2, \
long arg3,long arg4,long arg5,long arg6);

#endif