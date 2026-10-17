#include <stdlib.h>

/* 注意 (动态链接): 这里原来是 `static __thread ... = 1` —— 带**非零初值**的
 * 线程局部变量会在 .so 里生成 R_X86_64_TPOFF32 (local-exec) 重定位, 链接
 * 共享库时直接报错:
 *   "relocation R_X86_64_TPOFF32 against `next' can not be used when making
 *    a shared object"
 * 原因是那个初值必须由一个"链接期已知的 TLS 偏移"来初始化, 而 .so 的 TLS
 * 偏移是加载期才知道的。改成**零初始化 + 首次使用时赋种子**, 就只需要
 * 常规的 GOTTPOFF/initial-exec 访问, 静态库与共享库都能编。
 * (next 用 0 作"未播种"哨兵; 0 本身不是合法种子, C 标准也不要求 srand 的
 *  具体序列, 所以先把它落到 1 再推进一格即可。) */
static __thread unsigned long int next;

static inline unsigned long int next_state(void) {
    if (next == 0) next = 1;            /* 未播种: 与旧行为 (初值 1) 对齐 */
    next = next * 1103515245 + 12345;
    return next;
}

int rand(void) /* RAND_MAX assumed to be 32767 */{
    return (unsigned int)(next_state() / 65536) % 32768;
}

void srand(unsigned int seed){
    /* seed==0 会与"未播种"哨兵冲突, 直接映射成 1 */
    next = seed ? seed : 1;
}
