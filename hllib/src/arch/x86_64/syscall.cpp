#include <syscall.h>

uint64_t sys_read(uint32_t fd_idx, void *buf, size_t count){
    uint64_t n;
    asm volatile (
		"syscall\t\n"
		: "=a" (n)
		: "a" (1), "S" (buf), "D" (fd_idx), "d" (count));
    return n;
}
uint64_t sys_write(uint32_t fd_idx, void *buf, size_t count){
    uint64_t res = hal_task_syscall_api3(2, fd_idx, (uint64_t)buf, count);
    return res;
}
int64_t sys_lseek(uint32_t fd_idx, int64_t offset, int32_t whence){
    return 0;
}
uint64_t sys_open(const char *path, int flags, unsigned int mode){
    return 0;
}