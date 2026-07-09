#include <syscalln.h>
#include <syscall.h>
#include <mouse/ps2.h>

#define PS2_MOUSE_IDX   7

uint64_t mouse_addr = 0;

void MouseInit(){
    mouse_addr = syscall(SYSCALL_DEV_MMAP,PS2_MOUSE_IDX,0,0,0,0,0);
}