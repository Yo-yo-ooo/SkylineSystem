// tests/common/kernel_shim.h — 内核原语 → 宿主映射（声明）
#pragma once
#include <stdint.h>
#include <stddef.h>

// 内核符号, 定义见 kernel_shim.cpp（签名与 klib.h 声明一致）
extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern "C" void _memset(void *dest, uint8_t value, uint64_t size);
extern "C" void _memmove(void *dest, void *src, uint64_t size);
extern "C" int32_t _memcmp(const void *a, const void *b, size_t size);

// 宿主测试辅助: 全局操作计数 (harness 每轮自增, 供内核测试守卫打印定位)
extern uint64_t g_test_op;

void spinlock_lock(int32_t *l);
void spinlock_unlock(int32_t *l);

extern "C" void Panic(const char *message);
void hcf(void);

void bitmap_set(uint8_t *bmp, uint64_t bit);
void bitmap_clear(uint8_t *bmp, uint64_t bit);
bool bitmap_get(uint8_t *bmp, uint64_t bit);

// kprintf.h 将 kinfo/kerror/... 定义为宏, 底层实现是 printf_ (extern "C", int32_t);
// 宿主只需提供 printf_ 即可让全部内核日志宏工作。
extern "C" int32_t printf_(const char *fmt, ...);
// kprintf.h 把 snprintf 宏映射到 snprintf_ (extern "C", int32_t, 无 noexcept)
extern "C" int32_t snprintf_(char *buf, size_t size, const char *fmt, ...);
