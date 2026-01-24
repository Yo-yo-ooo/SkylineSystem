#include "syscall.h"

int main(){
    //Write Test
    const char* msg = "This Program NOT finished yet!\n";
    syscall(1, 1, (long)msg, 32, 0, 0, 0); //Syscall Write
    //Do Exit
    syscall(60, 0, 0, 0, 0, 0, 0); //Syscall Exit
    return 0;
}