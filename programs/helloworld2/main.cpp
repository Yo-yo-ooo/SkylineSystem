#include <stdio.h>
#include <syscall.h>

/* // SkylineSystem Console GUI Magic
const char gui_console_magic[] __attribute__((section(".GUI_C"), used, aligned(4)))
    = "SSYSGUI";
 */
int main(){
    const char *msg = "HelloWorld!";
    syscall(24, (long)msg, 12, 0, 0, 0, 0);

    while(true);
    /* syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0); */
}