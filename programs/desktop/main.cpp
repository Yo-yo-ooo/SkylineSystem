#include <unisted.h>

extern "C" void phl(void);
extern "C" void exit(void);

//test exit in user prog

int main(){
    write(1,"Hello, World!\n",13);
    exit();
    phl();
    while(true);
    return 0;
}