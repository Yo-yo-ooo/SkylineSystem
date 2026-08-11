#include <limits.h>
#include <stdint.h>
#include <stdlib.h>


#pragma GCC push_options
#pragma GCC optimize ("O2")
// POSTIVITVE: mask = 0, (j ^ 0) - 0 = j
// NEG: mask = -1 (全1), (j ^ -1) - (-1) = ~j + 1 = -j
#define FAST_ABS(j) __extension__ ({ \
    typeof(j) _mask = (j) >> (sizeof(j) * CHAR_BIT - 1); \
    ((j) ^ _mask) - _mask; \
})

int abs(int j) {
    return FAST_ABS(j);
}

long labs(long j) {
    return FAST_ABS(j);
}

long long llabs(long long j) {
    return FAST_ABS(j);
}


div_t div(int numer, int denom) {
    div_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    return result;
}

#pragma GCC pop_options