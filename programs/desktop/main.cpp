#include <unisted.h>
#include <syscall.h>

extern "C" void phl(void);
extern "C" void exit(void);

//test exit in user prog

int main(){
    /* ssize_t x = write(1,"Hello, World!\n",13); */
    ssize_t ret;
    syscall3(
        __NR_write,
        (uint64_t)1,
        (uint64_t)"Hello, World!\n",
        (uint64_t)13,
        ret
    );
    phl();
    phl();
    while(true);
    return 0;
}