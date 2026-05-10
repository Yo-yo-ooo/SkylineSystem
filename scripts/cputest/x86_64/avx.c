#include <immintrin.h> 
void floats_add(float *dest, float *a, float *b, unsigned size) { 
    for (; size >= 8; size -= 8, dest += 8, a += 8, b += 8) { 
        _mm256_storeu_ps(dest, _mm256_add_ps(_mm256_loadu_ps(a), _mm256_loadu_ps(b))); 
    } 
} 
int main(int argc, char **argv) { 
    floats_add((float*)0, (float*)0, (float*)0, 0); 
    return 0; 
}