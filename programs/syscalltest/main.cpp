#include "syscall.h"

int main(){
    //Write Test
    const char* msg = "Hello, SkylineSystem Syscall Test!\n";
    syscall(1, 1, (long)msg, 34, 0, 0, 0); //Syscall Write
    //Do Exit
    syscall(60, 0, 0, 0, 0, 0, 0); //Syscall Exit
    syscall(1, 1, (long)"Should not be printed", 22, 0, 0, 0); //Syscall Write
    while (true);
    return 0;
}