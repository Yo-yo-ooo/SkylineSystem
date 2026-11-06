// tests/kmod/host_platform.h — 宿主平台层的测试侧辅助接口
// （kmod_platform.h 的实现在 host_platform.cpp；本头只暴露测试用的记账钩子）
#pragma once
#include <stdint.h>

#include <kmod/kmod_component.h>

// 当前仍存活的 kmod_alloc 块数与字节数（用于断言 KmodElfFreeImage 无泄漏；
// ASAN/LSAN 是第二道保险，这里提供比泄漏报告更早、更精确的断言点）。
uint64_t kmod_host_live_alloc_count(void);
uint64_t kmod_host_live_alloc_bytes(void);

// 重置峰值/计数（不释放内存；测试开始前调用，保证断言从 0 起算）
void kmod_host_alloc_reset_stats(void);

// 进程内 kmod_alloc 历史总次数（含已释放），负向用例可用来确认"没有偷偷分配"
uint64_t kmod_host_total_alloc_count(void);

// ----------------------------------------------------------------------------
// 内建组件描述表（链接脚本 __kbuiltin_desc_start/__kbuiltin_desc_end 的宿主替身）
// host_platform.cpp 在自定义段里提供恰好 1 个 kmod_builtin_raw_t 槽位，
// 默认全零（非法描述符，用于"坏描述符"负向用例）。测试可写入一个合成描述符
// 来验证 KmodBuiltinInit/BootLoad 正向路径；raw == nullptr 表示清空。
// ----------------------------------------------------------------------------
void kmod_host_builtin_set_raw(const kmod_builtin_raw_t *raw);

// 当前宿主描述表槽位数（恒为 1，便于测试断言 KmodBuiltinCount 的上界）
uint32_t kmod_host_builtin_slots(void);
