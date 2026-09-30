// tests/common/errno.h — 宿主 shim:
// 1) include_next 拿 kernel/include/errno.h 的 E* 错误常量 (与 Linux 一致);
// 2) 补上 freestanding 版缺失的 errno 宏 (glibc 语义)。
#ifndef _TEST_ERRNO_SHIM_H_
#define _TEST_ERRNO_SHIM_H_

#include_next <errno.h>

#ifdef __cplusplus
extern "C" {
#endif
extern int *__errno_location(void);
#ifdef __cplusplus
}
#endif

#ifndef errno
#define errno (*__errno_location())
#endif

#endif
