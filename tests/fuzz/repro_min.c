#include <cstdio>
#include <cstdint>
#include <cstdarg>
extern "C" int32_t vsnprintf_(char* buffer, size_t count,
                              const char* format, va_list va);
void e9_putc(char c) { (void)c; }
extern "C" void PrintFSERIAL(const char* s) { (void)s; }
extern "C" void spinlock_lock(uint64_t* l) { (void)l; }
extern "C" void spinlock_unlock(uint64_t* l) { (void)l; }
namespace Serial { void Write(char c) { (void)c; } }

int main() {
    char buf[2048];
    const char* fmt = "%llu%%-8d%.0s%lu\n%lu%%o%p";
    const uintptr_t s_ok = (uintptr_t)"fuzz";
    uint64_t a0 = 0x123456789abcdef0ULL, a1 = 0xdeadbeefcafebabeULL;
    /* 与 fuzz 相同的槽位序列 */
    int r = vsnprintf_(buf, sizeof(buf), fmt, (va_list)nullptr);
    (void)r; (void)a0; (void)a1; (void)s_ok;
    printf("done r=%d\n", r);
    return 0;
}
