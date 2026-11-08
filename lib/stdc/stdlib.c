#include <stdlib.h>

#ifdef __x86_64__
#include <base/arch/x86_64/syscall.h>
#endif

void _Exit(){
#ifdef __x86_64__
    sys_exit(0);
#endif
    __builtin_unreachable();
}

/* ISO C: abort —— 异常终止, 不返回。
   用户态没有 SIGABRT 投递机制, 直接 sys_exit(128+SIGABRT)=134,
   与 POSIX shell 的 128+signal 退出码约定一致。 */
void abort(void) {
#ifdef __x86_64__
    sys_exit(128 + 6);
#endif
    __builtin_unreachable();
}