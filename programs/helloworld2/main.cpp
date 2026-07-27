#include <stdio.h>
#include <syscall.h>

int main(){
    const char *msg = "HelloWorld!";
    syscall(24, (long)msg, 12, 0, 0, 0, 0);

    while(true);
    /* syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0); */
}