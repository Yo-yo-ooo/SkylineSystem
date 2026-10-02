#include <stdio.h>
#include <syscall.h>

int main(){
    const char *msg = "Hello, World!";
    syscall(SYS_DBGSOUT, (long)msg, 16, 0, 0, 0, 0);

    //while(true);
    syscall(SYS_EXIT, 0, 0, 0, 0, 0, 0); // Exit
    /* P5-96: 删除退出后的死行 (syscall 9 不返回, 原第 10 行永不执行) */
}