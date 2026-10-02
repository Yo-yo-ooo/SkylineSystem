// tests/fuzz/fuzz_shim.cpp — kprintf.cpp 宿主桩 (序列输出吞掉即可)
#include <cstdint>
#include <cstdio>

extern "C" {
void PrintFSERIAL(const char* s) { (void)s; }
/* 宿主 fuzz 单线程, 自旋锁桩为无操作 */
void spinlock_lock(uint64_t* l) { (void)l; }
void spinlock_unlock(uint64_t* l) { (void)l; }
}
/* kprintf.cpp 的 e9_putc 是 C++ 链接 */
void e9_putc(char c) { (void)c; }
namespace Serial { void Write(char c) { (void)c; } }
