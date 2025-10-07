#include <syscall.h>

extern "C" void phl(void);

int main(){
    phl();
    while(1);
    return 0;
}