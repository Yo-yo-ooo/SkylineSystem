#include <syscalln.h>
#include <syscall.h>
#include <mouse/ps2.h>
#include <graphic/devtype.h>

#define PS2_MOUSE_IDX   DEV_TYPE_PS2_MOUSE   /* P5-97: 权威在 devtype.h */

uint64_t mouse_addr = 0;

void MouseInit(){
    mouse_addr = syscall(SYSCALL_DEV_MMAP,PS2_MOUSE_IDX,0,0,0,0,0);
}