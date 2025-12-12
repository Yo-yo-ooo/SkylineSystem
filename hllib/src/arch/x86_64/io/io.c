#include <arch/x86_64/unisted.h>
#include <arch/x86_64/syscall.h>

ssize_t write(int fd,const void *buf, size_t count){
    ssize_t ret;
    syscall3(
        __NR_write,
        (uint64_t)fd,
        (uint64_t)buf,
        (uint64_t)count,
        ret
    );
    return ret;
}