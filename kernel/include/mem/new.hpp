#include <stddef.h>
#include <klib/klib.h>

inline void *operator new(size_t, void *ptr_) throw() {return ptr_;}
inline void* operator new[](unsigned long size) {
    void* ptr;
    if (ptr = kmalloc(size)) {
        return ptr;
    }
    kerror("BAD MALLOC %p", ptr);
    for (;;) {
#ifdef __x86_64__
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
#endif
    }
}

extern const char* _X__file__;
extern const char* _X__func__;
extern size_t _X__line__;
#define new (_X__func__=__PRETTY_FUNCTION__,_X__file__=__FILE__,_X__line__=__LINE__) && 0 ? NULL : new
