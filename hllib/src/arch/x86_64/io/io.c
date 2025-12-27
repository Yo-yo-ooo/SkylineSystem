#include <arch/x86_64/io.h>
#include <arch/x86_64/syscall.h>

ssize_t write(int fd,const void *buf, size_t count){
    ssize_t ret = syscall(__NR_write,
        fd,
        (long)buf,
        count,0,0,0
    );
    return ret;
}