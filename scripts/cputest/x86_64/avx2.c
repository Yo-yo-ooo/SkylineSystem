#include <immintrin.h>
void ints_add(int *dest, int *a, int *b, unsigned size) {
    for (; size >= 8; size -= 8, dest += 8, a += 8, b += 8) {
        _mm256_storeu_si256((__m256i*)dest, _mm256_add_epi32(_mm256_loadu_si256((__m256i*)a), _mm256_loadu_si256((__m256i*)b)));
    }
}
int main(int argc, char **argv) {
    ints_add((int*)0, (int*)0, (int*)0, 0);
    return 0;
}