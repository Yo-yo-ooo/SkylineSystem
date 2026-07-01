#include <stdint.h>
#include <math.h>

/* Convert IEEE single precison numbers into decimal ASCII strings, while
   satisfying the following two properties:
   1) Calling strtof or '(float) strtod' on the result must produce the
   original float, independent of the rounding mode used by strtof/strtod.
   2) Minimize the number of produced decimal digits. E.g. the float 0.7f
   should convert to "0.7", not "0.69999999".

   To solve this we use a dedicated single precision version of
   Florian Loitsch's Grisu2 algorithm. See
   http://florian.loitsch.com/publications/dtoa-pldi2010.pdf?attredirects=0

   The code below is derived from Loitsch's C code, which
   implements the same algorithm for IEEE double precision. See
   http://florian.loitsch.com/publications/bench.tar.gz?attredirects=0
*/

#define DIY_SIGNIFICAND_SIZE 64
#define SP_SIGNIFICAND_MASK 0x7fffff
#define SP_HIDDEN_BIT 0x800000 /* 2^23 */
 
typedef union _f32
{
    float f;
    unsigned int i;
} _f32;

#if defined(__x86_64__) || defined(__amd64__)
static uint64_t 
multiply(uint64_t x, uint32_t y)
{
    uint64_t y0 = ((uint64_t)y << 32), ac, tmp;
    __asm__ __volatile__("mulq %3" : "=a" (tmp), "=d" (ac) :"%0" (x), "rm" (y0));
    //tmp += 0x80000000; /* Round.  */
    return ac + (tmp >> 63);
}
#else
static uint64_t 
multiply(uint64_t x, uint32_t y)
{
    uint64_t xlo = (x & 0xffffffff);
    uint64_t xhi = (x >> 32);
    return ((xhi * y) + ((xlo * y) >> 31));
}
#endif

static int
k_comp(int n)
{
    float ds = n*0.30103f;
    int k = (int)ds;
    return n < 0 ? k - 1 : k;
}

/* Cached powers of ten from 10**-37..10**40.
   Produced using GNU MPFR's mpfr_pow_si.  */

/* Significands.  */
static uint64_t powers_ten[78] = {
    0x881cea14545c7575, 0xaa242499697392d3, 0xd4ad2dbfc3d07788, 
    0x84ec3c97da624ab5, 0xa6274bbdd0fadd62, 0xcfb11ead453994ba, 
    0x81ceb32c4b43fcf5, 0xa2425ff75e14fc32, 0xcad2f7f5359a3b3e, 
    0xfd87b5f28300ca0e, 0x9e74d1b791e07e48, 0xc612062576589ddb, 
    0xf79687aed3eec551, 0x9abe14cd44753b53, 0xc16d9a0095928a27, 
    0xf1c90080baf72cb1, 0x971da05074da7bef, 0xbce5086492111aeb, 
    0xec1e4a7db69561a5, 0x9392ee8e921d5d07, 0xb877aa3236a4b449, 
    0xe69594bec44de15b, 0x901d7cf73ab0acd9, 0xb424dc35095cd80f, 
    0xe12e13424bb40e13, 0x8cbccc096f5088cc, 0xafebff0bcb24aaff, 
    0xdbe6fecebdedd5bf, 0x89705f4136b4a597, 0xabcc77118461cefd, 
    0xd6bf94d5e57a42bc, 0x8637bd05af6c69b6, 0xa7c5ac471b478423, 
    0xd1b71758e219652c, 0x83126e978d4fdf3b, 0xa3d70a3d70a3d70a, 
    0xcccccccccccccccd, 0x8000000000000000, 0xa000000000000000, 
    0xc800000000000000, 0xfa00000000000000, 0x9c40000000000000, 
    0xc350000000000000, 0xf424000000000000, 0x9896800000000000, 
    0xbebc200000000000, 0xee6b280000000000, 0x9502f90000000000, 
    0xba43b74000000000, 0xe8d4a51000000000, 0x9184e72a00000000, 
    0xb5e620f480000000, 0xe35fa931a0000000, 0x8e1bc9bf04000000, 
    0xb1a2bc2ec5000000, 0xde0b6b3a76400000, 0x8ac7230489e80000, 
    0xad78ebc5ac620000, 0xd8d726b7177a8000, 0x878678326eac9000, 
    0xa968163f0a57b400, 0xd3c21bcecceda100, 0x84595161401484a0, 
    0xa56fa5b99019a5c8, 0xcecb8f27f4200f3a, 0x813f3978f8940984, 
    0xa18f07d736b90be5, 0xc9f2c9cd04674edf, 0xfc6f7c4045812296, 
    0x9dc5ada82b70b59e, 0xc5371912364ce305, 0xf684df56c3e01bc7, 
    0x9a130b963a6c115c, 0xc097ce7bc90715b3, 0xf0bdc21abb48db20, 
    0x96769950b50d88f4, 0xbc143fa4e250eb31, 0xeb194f8e1ae525fd
};

/* Exponents.  */
static int8_t powers_ten_e[78] = {
    -127, -124, -121, -117, -114, -111, -107, -104, -101,  -98,  -94,  -91,  -88,  -84,  -81,  -78, 
     -74,  -71,  -68,  -64,  -61,  -58,  -54,  -51,  -48,  -44,  -41,  -38,  -34,  -31,  -28,  -24, 
     -21,  -18,  -14,  -11,   -8,   -4,   -1,    2,    5,    9,   12,   15,   19,   22,   25,   29, 
      32,   35,   39,   42,   45,   49,   52,   55,   59,   62,   65,   69,   72,   75,   79,   82, 
      85,   89,   92,   95,   98,  102,  105,  108,  112,  115,  118,  122,  125,  127,
};
 
/*
 * compute decimal integer m, exp such that:
 *  f = m*10^exp
 *  m is as short as possible without losing exactness
 */
unsigned int ftoa(char *s, float f, int *_K)
{
    uint32_t w_lower, w_upper;
    uint64_t D_upper, D_lower, delta, c_mk, one, p2;
    _f32 f2;
    int ve, mk = 0, kabs = 0;
    unsigned int len = 0;
    unsigned char digit, p1;

    if (f != f) {
        *(uint32_t*)s = 0x004E614E;
        goto nanzero;
    }

    /* Handle NaN/zero. This is split up like this because otherwise gcc generates shockingly awful assembly. Like, doubling total function size bad. */
    if (!f) {
        /* f is NaN, +0 or -0.  */
        *(uint32_t*)s = 0x00302E30;
nanzero:
        return 3;
    }
    
    f2.f = fabsf(f);
    ve = (f2.i >> 23) - 127 - 1;
    f2.i = ((f2.i & SP_SIGNIFICAND_MASK) | SP_HIDDEN_BIT);
    w_upper = (f2.i << 2) + 2;
    w_lower = (f2.i << 2) - 1;
    if (f2.i != SP_HIDDEN_BIT) {
        w_lower--;
    }
    w_upper <<= (DIY_SIGNIFICAND_SIZE - 58);
    w_lower <<= (DIY_SIGNIFICAND_SIZE - 58);

    mk = k_comp(ve - 1);
    ve = ve + powers_ten_e[37 - mk] - DIY_SIGNIFICAND_SIZE + 7;
    one = ((uint64_t) 1 << -ve) - 1;

    c_mk = powers_ten[37 - mk];
    D_upper = multiply(c_mk, w_upper);
    D_lower = multiply(c_mk, w_lower);

    D_upper--;
    D_lower++;

    delta = (D_upper - D_lower);
    p1 = D_upper >> -ve;
    p2 = D_upper & one;

    digit = p1 / 10;
    if (digit) {
        s[len++] = 0x30 + digit;
        s[len++] = '.';
        mk++;
    }
    p1 %= 10;
    s[len++] = 0x30 + p1;
    if (!digit)
        s[len++] = '.';
    do {
        p2 *= 10;
        s[len++] = 0x30 + (p2 >> -ve);
        p2 &= one;
        delta *= 10;
    } while (p2 > delta);

    s[len++] = 'e';
    if(mk < 0) {
        s[len++] = '-';
        kabs = -mk;
    } else {
        s[len++] = '+';
        kabs = mk;
    }
    s[len++] = (kabs / 10)+0x30;
    s[len++] = (kabs % 10)+0x30;
    s[len] = 0;

    if(_K) {
        *_K = mk;
    }
    return len;
}

unsigned int ftoahex(char *s, float f, int *K)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunterminated-string-initialization"
    static const char hex[16] = "0123456789abcdef";
#pragma GCC diagnostic pop
    uint32_t k = 6, uval = 0, tmp;
    int mk = 0;
    _f32 f2;

    /* Handle NaN/zero.  */
    if (f != f) {
        *(uint32_t*)s = 0x004E614E;
        goto nanzero;
    }
    if (!f) {
        /* f is NaN, +0 or -0.  */
        *(uint32_t*)s = 0x00302E30;
nanzero:
        return 3;
    }
    
    f2.f = fabsf(f);
    mk = ((int)(f2.i >> 23) - 127);
    if(K) {
        *K = mk;
    }
    f2.i &= 0x007fffff;
    f2.i <<= 1;
    uval = f2.i;
    s[0] = '0';
    s[1] = 'x';
    s[2] = '1';
    s[3] = '.';
    while(k--) {
        tmp = (uval & 0x0f);
        s[4+k] = hex[tmp]; uval >>= 4;
    }
    s[10] = 'p';
    if(mk < 0) {
        s[11] = '-';
        mk = -mk;
    } else {
        s[11] = '+';
    }
    s[12] = (mk / 10)+0x30;
    s[13] = (mk % 10)+0x30;
    s[14] = 0;
    return 14;
}