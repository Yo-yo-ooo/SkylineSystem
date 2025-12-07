#include <syscall.h>

extern "C" void phl(void);
extern "C" void exit(void);

//test exit in user prog

int main(){
    phl();
    exit();
    phl();
    while(true);
    return 0;
}