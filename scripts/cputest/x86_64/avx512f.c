#include <immintrin.h>
void floats_add(float *dest, float *a, float *b, unsigned size) {
    for (; size >= 16; size -= 16, dest += 16, a += 16, b += 16) {
        _mm512_storeu_ps(dest, _mm512_add_ps(_mm512_loadu_ps(a), _mm512_loadu_ps(b)));
    }
}
int main(int argc, char **argv) {
    floats_add((float*)0, (float*)0, (float*)0, 0);
    return 0;
}