#include <nmmintrin.h> 
__m128i bitmask; 
char data[16]; 
int main(int argc, char **argv) { 
    bitmask = _mm_cmpgt_epi64(_mm_set1_epi64x(0), _mm_loadu_si128((void*)data)); 
    return 0; 
}