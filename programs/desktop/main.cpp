#include <syscall.h>

int main(){
    sys_write(1,(void*)"Hello, World!\n",14);
    return 0;
}