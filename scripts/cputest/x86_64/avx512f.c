#include <immintrin.h>
// 强制要求编译器生成一条真正的 512 位指令
void test() {
    __m512i v = _mm512_setzero_si512();
    asm volatile("" : : "x"(v) : "memory"); 
}
int main() { return 0; }