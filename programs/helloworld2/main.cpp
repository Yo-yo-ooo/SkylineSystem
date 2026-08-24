#include <stdint.h>
#include <syscall.h>
#include <stdlib.h>
/* 指向链接脚本中 .prepad 段的起始地址 (0x400000) */
#define RW_PAGE_BASE 0x400000UL

int main() {
    // 将该地址强制转换为字符指针
    // 使用 volatile 防止编译器将内存读写优化掉
    volatile char *rw_page = (volatile char *)RW_PAGE_BASE;

    /* 1. 向这块 4KB 的可读写内存写入数据 */
    const char *msg = "Hello from 0x400000!\n";
    int len = 0;
    while (msg[len] != '\0') {
        rw_page[len] = msg[len];
        len++;
    }

    /* 2. 调用 syscall 直接输出这块内存的内容 (假设 24 是 write) */
    syscall(24, (long)rw_page, len, 0, 0, 0, 0);

    /* 3. 证明它是可修改的：直接修改这块内存中的部分字符 */
    rw_page[0] = 'h'; // 'H' -> 'h'
    rw_page[6] = 'F'; // 'f' -> 'F' (修改 from 中的 f)
    
    // 再次输出，验证内存确实被修改了
    syscall(24, (long)rw_page, len, 0, 0, 0, 0);

    //while(true);
    
    //exit(0);
    return 0;
}