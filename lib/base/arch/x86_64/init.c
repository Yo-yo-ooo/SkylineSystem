#include <stdint.h>
#include <stddef.h>
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
#include <x86mem.h>
#endif
extern void *(*memcpyC)(void *str1, const void *str2, size_t n);
extern void *(*memsetC)(void *str, int c, size_t n);
extern void *(*memmoveC)(void *str1, const void *str2, size_t n);
extern int (*memcmpC)(const void *str1, const void *str2, size_t n);

extern void *memset_fscpuf(void *dst, const int32_t val, size_t n);
extern void *memcpy_fscpuf(void *dst, const void *src, size_t n);
extern void *memmove_fscpuf(void *dst, const void *src, size_t n);
extern int32_t memcmp_fscpuf(const void *left, const void *right, size_t len);


// 获取 XCR0 扩展控制寄存器值的辅助函数 (使用 xgetbv 指令)
static inline uint64_t xgetbv(uint32_t index) {
    uint32_t eax, edx;
    asm volatile (
        "xgetbv"
        : "=a"(eax), "=d"(edx)
        : "c"(index)
    );
    return ((uint64_t)edx << 32) | eax;
}

// 通用 CPUID 包装函数
static inline void cpuid(uint32_t code, uint32_t subcode, 
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    asm volatile (
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(code), "c"(subcode)
    );
}

/**
 * 检查 AVX、AVX2 和 AVX512F 支持情况
 * @param out_avx   输出: 1 支持, 0 不支持
 * @param out_avx2  输出: 1 支持, 0 不支持
 * @param out_avx512 输出: 1 支持, 0 不支持
 */
void check_avx_extensions(int *out_avx, int *out_avx2, int *out_avx512) {
    *out_avx = 0;
    *out_avx2 = 0;
    *out_avx512 = 0;

    uint32_t eax, ebx, ecx, edx;

    // 1. 发起 CPUID(EAX=1) 检查基础硬件与 OSXSAVE 标志
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    uint32_t osxsave_mask = (1 << 27);
    uint32_t avx_hardware_mask = (1 << 28);

    // 如果硬件甚至不支持 AVX，或者 OS 没开启 XSAVE，直接退出
    if ((ecx & osxsave_mask) == 0 || (ecx & avx_hardware_mask) == 0) {
        return; 
    }

    // 2. 读取 XCR0，检查操作系统层面的寄存器生命周期支持
    uint64_t xcr0 = xgetbv(0);

    // AVX 最基础的要求：XCR0 中的 SSE 状态(Bit 1) 和 AVX 状态(Bit 2) 必须被使能
    if ((xcr0 & 0x6) != 0x6) {
        return; // 操作系统未启用 YMM 寄存器上下文切换
    }
    *out_avx = 1; // 满足硬件且 OS 已开启，支持 AVX

    // 3. 发起 CPUID(EAX=7, ECX=0) 检查更高级的扩展特征
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);

    // 检查 AVX2：EBX 的 Bit 5
    if (ebx & (1 << 5)) {
        *out_avx2 = 1;
    }

    // 4. 检查 AVX512 基础集 (AVX512F)：EBX 的 Bit 16
    // 注意：AVX512 额外要求 XCR0 开启了 opmask(Bit 5), ZMM_Hi256(Bit 6), Hi16_ZMM(Bit 7)
    // 也就是 0xE0 (0b11100000)
    if ((ebx & (1 << 16)) && ((xcr0 & 0xE0) == 0xE0)) {
        *out_avx512 = 1;
    }
}
int memcmpCAV0(const void *str1, const void *str2, size_t n){
    return AVX_memcmpV0(str1,str2,n,1);
}

int memcmpCAV1(const void *str1, const void *str2, size_t n){
    return AVX_memcmpV1(str1,str2,n,1);
}

int memcmpCAV2(const void *str1, const void *str2, size_t n){
    return AVX_memcmpV2(str1,str2,n,1);
}

int memcmpCAV3(const void *str1, const void *str2, size_t n){
    return AVX_memcmpV3(str1,str2,n,1);
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
void _init_runtime_and_global_variables(){
    unsigned int ecx_val = 0;

    // 使用 GCC 内联汇编直接操作寄存器
    asm volatile (
        "mov $1, %%eax\n\t"     // 将 EAX 置为 1，请求功能特征标志
        "cpuid\n\t"             // 执行 cpuid，结果会填充到 EAX, EBX, ECX, EDX
        "mov %%ecx, %0\n\t"     // 将我们需要的 ECX 寄存器的值拷贝给输出变量
        : "=r" (ecx_val)        // %0: 输出操作数，绑定到 ecx_val
        :                       // 无输入操作数
        : "eax", "ebx", "ecx", "edx" // 破坏描述列表（Clobber List）：告诉编译器这些寄存器被污染了
    );
    uint8_t sse42;
    // 检查 ECX 的第 20 位 (Bit 20)
    // 1 << 20 的十六进制值是 0x100000
    if (ecx_val & (1 << 20)) {
        sse42 = 1;
    }
    int avx,avx2,avx512;
    check_avx_extensions(&avx,&avx2,&avx512);
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
    if(avx512 && MEMOPS_SupportV3){
        memcmpC = memcmpCAV3;
        memcpyC = AVX_memcpyV3;
        memsetC = AVX_memsetV3;
        memmoveC = AVX_memmoveV3;
    }
    else if(avx2 && MEMOPS_SupportV2){
        memcmpC = memcmpCAV2;
        memcpyC = AVX_memcpyV2;
        memsetC = AVX_memsetV2;
        memmoveC = AVX_memmoveV2;
    }
    else if(avx && MEMOPS_SupportV1){
        memcmpC = memcmpCAV1;
        memcpyC = AVX_memcpyV1;
        memsetC = AVX_memsetV1;
        memmoveC = AVX_memmoveV1;
    }else if(sse42){
        memcmpC = memcmpCAV0;
        memcpyC = AVX_memcpyV0;
        memsetC = AVX_memsetV0;
        memmoveC = AVX_memmoveV0;
    }else{
        memcmpC = memcmp_fscpuf;
        memcpyC = memcpy_fscpuf;
        memmoveC = memmove_fscpuf;
        memsetC = memset_fscpuf;
    }
#else
    memcmpC = memcmp_fscpuf;
    memcpyC = memcpy_fscpuf;
    memmoveC = memmove_fscpuf;
    memsetC = memset_fscpuf;
#endif
}
#pragma GCC diagnostic pop // 恢复原有的严格检查