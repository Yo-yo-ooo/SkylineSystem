#include <stdio.h>
#include <syscall.h>

int main(){
    const char *msg = "Hello, World!";
    syscall(24, (long)msg, 13, 0, 0, 0, 0);

    
    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
}