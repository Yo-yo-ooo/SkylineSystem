#pragma once
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <libfg.h>


uint64_t sys_read(uint32_t fd_idx, void *buf, size_t count);
uint64_t sys_write(uint32_t fd_idx, void *buf, size_t count);
int64_t sys_lseek(uint32_t fd_idx, int64_t offset, int32_t whence);
uint64_t sys_open(const char *path, int flags, unsigned int mode);



#endif