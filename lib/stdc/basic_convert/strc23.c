#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#define BIG_INT_LEN 48
#define MAX_DEC_CHUNKS 40
#define MAX_PRECISION 400
#define BUF_LEN 820
#define POW10_8 100000000ULL
#define POW10_9 1000000000ULL

// 乘法逆元常量定义
#define MAGIC_DIV_10000 0xD1B71759ULL
#define SHIFT_DIV_10000 45
#define MAGIC_DIV_100 0x51EB851FULL
#define SHIFT_DIV_100 37
#define MAGIC_DIV_10_PACKED 0xCDULL
#define SHIFT_DIV_10_PACKED 11


//SWAR :全并行乘法逆元无除法转换
static inline uint64_t extract_8_digits_swar(uint32_t val) {
    uint32_t h4 = (uint32_t)((uint64_t)val * MAGIC_DIV_10000 >> SHIFT_DIV_10000);
    uint32_t l4 = val - h4 * 10000;

    uint32_t hh = (uint32_t)((uint64_t)h4 * MAGIC_DIV_100 >> SHIFT_DIV_100);
    uint32_t hl = h4 - hh * 100;
    uint64_t packed_h = ((uint64_t)hh << 16) | hl;
    uint64_t mul_h = packed_h * MAGIC_DIV_10_PACKED;
    uint64_t q_h = (mul_h >> SHIFT_DIV_10_PACKED) & 0x000F000FULL;
    uint64_t r_h = (packed_h & 0x00FF00FFULL) - (q_h * 10);
    uint32_t ascii_h = (uint32_t)(((q_h << 8) | r_h) + 0x30303030ULL);

    uint32_t lh = (uint32_t)((uint64_t)l4 * MAGIC_DIV_100 >> SHIFT_DIV_100);
    uint32_t ll = l4 - lh * 100;
    uint64_t packed_l = ((uint64_t)lh << 16) | ll;
    uint64_t mul_l = packed_l * MAGIC_DIV_10_PACKED;
    uint64_t q_l = (mul_l >> SHIFT_DIV_10_PACKED) & 0x000F000FULL;
    uint64_t r_l = (packed_l & 0x00FF00FFULL) - (q_l * 10);
    uint32_t ascii_l = (uint32_t)(((q_l << 8) | r_l) + 0x30303030ULL);

    return ((uint64_t)ascii_h << 32) | ascii_l;
}

static int swar_utoa(char* p, uint64_t val) {
    if (val == 0) { p[0] = '0'; return 1; }
    char temp[24];
    int len = 0, block_idx = 0;
    uint64_t blocks[3];

    while (val >= POW10_8) {
        uint32_t chunk;
#if defined(__SIZEOF_INT128__)
        uint64_t q = (uint64_t)((__uint128_t)val * 0xABCC77118461CEFDULL >> 87);
        chunk = (uint32_t)(val - q * POW10_8);
        val = q;
#else
        chunk = (uint32_t)(val % POW10_8);
        val /= POW10_8;
#endif
        blocks[block_idx++] = extract_8_digits_swar(chunk);
    }
    blocks[block_idx++] = extract_8_digits_swar((uint32_t)val);

    uint64_t highest = blocks[block_idx - 1];
    int start = 0;
    while (start < 8 && ((highest >> (56 - start * 8)) & 0xFF) == '0') start++;

    for (int i = start; i < 8; ++i) temp[len++] = (char)((highest >> (56 - i * 8)) & 0xFF);
    for (int b = block_idx - 2; b >= 0; b--) {
        uint64_t blk = blocks[b];
        for (int i = 0; i < 8; ++i) temp[len++] = (char)((blk >> (56 - i * 8)) & 0xFF);
    }
    memcpy(p, temp, len);
    return len;
}

/* ==========================================
 * 纯大整数操作封装 (小端存储)
 * ========================================== */
static inline bool big_int_shl(uint32_t* big, int* big_len, int exp) {
    int word_shift = exp / 32;
    int bit_shift = exp % 32;
    if (word_shift > 0) {
        if (word_shift + *big_len > BIG_INT_LEN) return false;
        for (int i = *big_len - 1; i >= 0; i--) big[i + word_shift] = big[i];
        for (int i = 0; i < word_shift; i++) big[i] = 0;
        *big_len += word_shift;
    }
    if (bit_shift > 0) {
        uint32_t carry = 0;
        for (int i = word_shift; i < *big_len; i++) {
            uint32_t cur = big[i];
            big[i] = (cur << bit_shift) | carry;
            carry = cur >> (32 - bit_shift);
        }
        if (carry && *big_len < BIG_INT_LEN) {
            big[(*big_len)++] = carry;
        }
        else if (carry) {
            return false;
        }
    }
    return true;
}

static inline void big_int_mul_small(uint32_t* big, int* big_len, uint64_t mul_val) {
    uint64_t carry = 0;
    for (int j = 0; j < *big_len; j++) {
        uint64_t val = (uint64_t)big[j] * mul_val + carry;
        big[j] = (uint32_t)val;
        carry = val >> 32;
    }
    while (carry && *big_len < BIG_INT_LEN) {
        big[(*big_len)++] = (uint32_t)carry;
        carry >>= 32;
    }
}

static inline void big_int_shr(uint32_t* big, int* big_len, int shift) {
    int word_shift = shift / 32;
    int bit_shift = shift % 32;
    if (word_shift >= *big_len) {
        for (int i = 0; i < *big_len; i++) big[i] = 0;
        *big_len = 0;
    }
    else {
        for (int i = 0; i < *big_len; i++) {
            if (i + word_shift < *big_len) {
                if (bit_shift == 0) {
                    big[i] = big[i + word_shift];
                }
                else {
                    uint64_t val = big[i + word_shift] >> bit_shift;
                    if (i + word_shift + 1 < *big_len) {
                        val |= (uint64_t)big[i + word_shift + 1] << (32 - bit_shift);
                    }
                    big[i] = (uint32_t)val;
                }
            }
            else {
                big[i] = 0;
            }
        }
        while (*big_len > 0 && big[*big_len - 1] == 0) (*big_len)--;
    }
}

static inline void big_int_add_small(uint32_t* big, int* big_len, uint32_t val) {
    uint64_t carry = val;
    for (int i = 0; i < *big_len; i++) {
        uint64_t v = (uint64_t)big[i] + carry;
        big[i] = (uint32_t)v;
        carry = v >> 32;
    }
    while (carry && *big_len < BIG_INT_LEN) {
        big[(*big_len)++] = (uint32_t)carry;
        carry >>= 32;
    }
}

static inline uint32_t big_int_div_pow10(uint32_t* big, int* big_len, uint64_t divisor, uint64_t magic, int shift) {
    uint64_t rem = 0;
    for (int i = *big_len - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | big[i];
#if defined(__SIZEOF_INT128__)
        uint64_t q = (uint64_t)((__uint128_t)cur * magic >> shift);
        big[i] = (uint32_t)q;
        rem = cur - q * divisor;
#else
        big[i] = (uint32_t)(cur / divisor);
        rem = cur % divisor;
#endif
    }
    while (*big_len > 0 && big[*big_len - 1] == 0) (*big_len)--;
    return (uint32_t)rem;
}

static int big_utoa(char* p, uint64_t mant, int exp) {
    uint32_t big[BIG_INT_LEN] = { 0 };
    big[0] = (uint32_t)(mant & 0xFFFFFFFFULL);
    big[1] = (uint32_t)(mant >> 32);
    int big_len = 2;

    if (!big_int_shl(big, &big_len, exp)) return 0;

    uint32_t chunks[MAX_DEC_CHUNKS];
    int c_idx = 0;
    while (big_len > 0) {
        chunks[c_idx++] = big_int_div_pow10(big, &big_len, POW10_9, 0x44B82FA09B5A52DULL, 93);
    }

    char temp[400];
    int len = swar_utoa(temp, chunks[c_idx - 1]);

    for (int i = c_idx - 2; i >= 0; i--) {
        uint32_t c = chunks[i];
        uint32_t high = c / POW10_8;
        uint32_t low8 = c - high * POW10_8;
        temp[len++] = '0' + high;
        uint64_t packed = extract_8_digits_swar(low8);
        for (int j = 7; j >= 0; j--) {
            temp[len++] = (char)((packed >> (j * 8)) & 0xFF);
        }
    }
    memcpy(p, temp, len);
    return len;
}

static bool format_frac(char* buf, uint64_t rem, int shift, int precision, bool truncate) {
    if (rem == 0) {
        for (int i = 0; i < precision; i++) buf[i] = '0';
        return false;
    }

    uint32_t big[BIG_INT_LEN] = { 0 };
    big[0] = (uint32_t)(rem & 0xFFFFFFFF);
    big[1] = (uint32_t)(rem >> 32);
    int big_len = 2;

    int full_blocks = precision / 8;
    int rem_prec = precision % 8;
    for (int i = 0; i < full_blocks; i++) big_int_mul_small(big, &big_len, POW10_8);
    for (int i = 0; i < rem_prec; i++) big_int_mul_small(big, &big_len, 10);

    bool round_up = false;
    if (!truncate && shift > 0) {
        int word_idx = (shift - 1) / 32;
        int bit_idx = (shift - 1) % 32;
        if (word_idx < BIG_INT_LEN) {
            bool half_bit = (big[word_idx] >> bit_idx) & 1;
            bool has_rem = false;
            if (bit_idx > 0 && (big[word_idx] & ((1ULL << bit_idx) - 1))) has_rem = true;
            if (!has_rem) {
                for (int i = word_idx - 1; i >= 0; i--) {
                    if (big[i]) { has_rem = true; break; }
                }
            }
            if (half_bit && has_rem) round_up = true;
            else if (half_bit && !has_rem) {
                int q_word_idx = shift / 32;
                int q_bit_idx = shift % 32;
                if (q_word_idx < big_len && (big[q_word_idx] >> q_bit_idx) & 1) round_up = true;
            }
        }
    }

    big_int_shr(big, &big_len, shift);
    if (round_up) big_int_add_small(big, &big_len, 1);

    int blocks_needed = (precision + 1 + 7) / 8;
    if (blocks_needed == 0) blocks_needed = 1;
    int total_digits = blocks_needed * 8;
    char temp[420];
    int pos = total_digits - 8;

    for (int b = 0; b < blocks_needed; b++) {
        if (big_len == 0) {
            if (pos >= 0) {
                memset(temp, '0', pos + 8);
            }
            pos -= 8;
            continue;
        }
        uint32_t rem_div = big_int_div_pow10(big, &big_len, POW10_8, 0xABCC77118461CEFDULL, 87);
        uint64_t packed = extract_8_digits_swar(rem_div);
        for (int i = 7; i >= 0; i--) {
            temp[pos + (7 - i)] = (char)((packed >> (i * 8)) & 0xFF);
        }
        pos -= 8;
    }

    int carry_pos = total_digits - 1 - precision;
    bool has_overflow = (temp[carry_pos] != '0');

    if (has_overflow) {
        for (int i = 0; i < precision; i++) buf[i] = '0';
        return true;
    }
    else {
        int start_idx = total_digits - precision;
        for (int i = 0; i < precision; i++) buf[i] = temp[start_idx + i];
        return false;
    }
}

/* ==========================================
 * 格式化核心 (零 snprintf 依赖)
 * ========================================== */

static int format_f_core(char* buf, int exp, uint64_t mant, int precision, bool flag_hash, bool truncate) {
    int len = 0;
    if (exp >= 0) {
        if (exp < 12) {
            uint64_t int_part = mant << exp;
            len += swar_utoa(buf, int_part);
        }
        else {
            len += big_utoa(buf, mant, exp);
            if (len == 0) { buf[0] = '0'; len = 1; }
        }
        if (precision > 0) {
            buf[len++] = '.';
            for (int i = 0; i < precision; i++) buf[len++] = '0';
        }
        else if (flag_hash) {
            buf[len++] = '.';
        }
    }
    else {
        int shift = -exp;
        uint64_t int_part = (shift >= 64) ? 0 : (mant >> shift);
        uint64_t rem = (shift >= 64) ? mant : (mant & ((1ULL << shift) - 1));

        if (precision == 0) {
            if (!truncate && shift <= 64) {
                uint64_t half = 1ULL << (shift - 1);
                if (rem > half || (rem == half && (int_part & 1))) int_part++;
            }
            len += swar_utoa(buf, int_part);
            if (flag_hash) buf[len++] = '.';
        }
        else {
            char int_buf[64];
            int int_len = swar_utoa(int_buf, int_part);

            char frac_buf[420];
            bool has_overflow = format_frac(frac_buf, rem, shift, precision, truncate);
            if (has_overflow && !truncate) {
                int_part++;
                int_len = swar_utoa(int_buf, int_part);
            }

            memcpy(buf, int_buf, int_len);
            buf[int_len] = '.';
            memcpy(buf + int_len + 1, frac_buf, precision);
            len = int_len + 1 + precision;
        }
    }
    return len;
}

static int format_e_core(char* buf, int exp, uint64_t mant, int precision, bool flag_hash, char e_char) {
    char temp[BUF_LEN];
    int prec_f = precision + 5;
    if (prec_f < 17) prec_f = 17;

    if (exp < 0) {
        int est_zeros = (int)(((int64_t)(-exp) * 30103) / 100000);
        if (est_zeros + prec_f > prec_f) {
            prec_f = est_zeros + prec_f;
        }
    }
    if (prec_f > MAX_PRECISION) prec_f = MAX_PRECISION;

    int len = format_f_core(temp, exp, mant, prec_f, false, true);

    int dot_pos = -1;
    for (int j = 0; j < len; j++) {
        if (temp[j] == '.') { dot_pos = j; break; }
    }
    if (dot_pos == -1) dot_pos = len;

    int first_non_zero = -1;
    for (int j = 0; j < len; j++) {
        if (temp[j] >= '1' && temp[j] <= '9') {
            first_non_zero = j;
            break;
        }
    }

    int E = 0;
    if (first_non_zero == -1) {
        E = 0;
        first_non_zero = 0;
    }
    else {
        if (first_non_zero < dot_pos) {
            E = dot_pos - first_non_zero - 1;
        }
        else {
            E = dot_pos - first_non_zero;
        }
    }

    int buf_idx = 0;
    if (first_non_zero == -1) {
        buf[buf_idx++] = '0';
    }
    else {
        buf[buf_idx++] = temp[first_non_zero++];
    }

    if (precision > 0) {
        buf[buf_idx++] = '.';
        int p_count = 0;
        while (p_count < precision && first_non_zero < len) {
            if (temp[first_non_zero] != '.') {
                buf[buf_idx++] = temp[first_non_zero];
                p_count++;
            }
            first_non_zero++;
        }
        while (p_count < precision) {
            buf[buf_idx++] = '0';
            p_count++;
        }

        // 修复：严格按照 C 标准四舍六入五成双判定进位
        char next_digit = (first_non_zero < len) ? temp[first_non_zero] : '0';
        bool round_up = false;
        if (next_digit > '5') {
            round_up = true;
        }
        else if (next_digit == '5') {
            bool has_non_zero_after = false;
            for (int k = first_non_zero + 1; k < len; k++) {
                if (temp[k] >= '1' && temp[k] <= '9') {
                    has_non_zero_after = true;
                    break;
                }
            }
            if (has_non_zero_after) {
                round_up = true;
            }
            else {
                int last_digit = buf[buf_idx - 1] - '0';
                if (last_digit % 2 != 0) {
                    round_up = true;
                }
            }
        }

        if (round_up) {
            int k = buf_idx - 1;
            bool carry = false;
            while (k >= 0) {
                if (buf[k] == '.') { k--; continue; }
                if (buf[k] == '9') {
                    buf[k] = '0';
                    k--;
                }
                else {
                    buf[k]++;
                    carry = true;
                    break;
                }
            }
            // 修复：进位溢出时正确移位小数点
            if (!carry) {
                buf[0] = '1';
                if (precision > 0) {
                    buf[1] = '.';
                    for (int i = 2; i <= precision + 1; i++) buf[i] = '0';
                    buf_idx = precision + 2;
                }
                else {
                    buf_idx = 1;
                }
                E++;
            }
        }
    }
    else if (flag_hash) {
        buf[buf_idx++] = '.';
    }

    buf[buf_idx++] = e_char;
    if (E >= 0) buf[buf_idx++] = '+';
    else { buf[buf_idx++] = '-'; E = -E; }

    if (E >= 100) {
        buf[buf_idx++] = '0' + E / 100;
        buf[buf_idx++] = '0' + (E / 10) % 10;
        buf[buf_idx++] = '0' + E % 10;
    }
    else {
        buf[buf_idx++] = '0' + E / 10;
        buf[buf_idx++] = '0' + E % 10;
    }
    return buf_idx;
}

static int format_g_core(char* buf, int exp, uint64_t mant, int precision, bool flag_hash, char e_char) {
    if (precision == 0) precision = 1;

    char temp[BUF_LEN];
    int prec_f = precision + 5;
    if (prec_f < 17) prec_f = 17;

    if (exp < 0) {
        int est_zeros = (int)(((int64_t)(-exp) * 30103) / 100000);
        if (est_zeros + prec_f > prec_f) {
            prec_f = est_zeros + prec_f;
        }
    }
    if (prec_f > MAX_PRECISION) prec_f = MAX_PRECISION;
    int len = format_f_core(temp, exp, mant, prec_f, false, true);

    int dot_pos = -1;
    for (int j = 0; j < len; j++) {
        if (temp[j] == '.') { dot_pos = j; break; }
    }
    if (dot_pos == -1) dot_pos = len;

    int first_non_zero = -1;
    for (int j = 0; j < len; j++) {
        if (temp[j] >= '1' && temp[j] <= '9') {
            first_non_zero = j;
            break;
        }
    }

    int E = 0;
    if (first_non_zero == -1) {
        E = 0;
        first_non_zero = 0;
    }
    else {
        if (first_non_zero < dot_pos) {
            E = dot_pos - first_non_zero - 1;
        }
        else {
            E = dot_pos - first_non_zero;
        }
    }

    if (E < -4 || E >= precision) {
        int e_len = format_e_core(buf, exp, mant, precision - 1, flag_hash, e_char);
        if (!flag_hash) {
            int e_pos = 0;
            while (e_pos < e_len && buf[e_pos] != e_char) e_pos++;
            int end = e_pos - 1;
            if (buf[end] == '.') end--;
            while (end > 0 && buf[end] == '0') end--;
            if (buf[end] == '.') end--;
            int new_len = end + 1;
            for (int k = e_pos; k < e_len; k++) buf[new_len++] = buf[k];
            return new_len;
        }
        return e_len;
    }
    else {
        int p = precision - 1 - E;
        if (p < 0) p = 0;
        int f_len = format_f_core(buf, exp, mant, p, flag_hash, false);
        if (!flag_hash) {
            int end = f_len - 1;
            while (end > 0 && buf[end] == '0') end--;
            if (buf[end] == '.') end--;
            return end + 1;
        }
        return f_len;
    }
}

static int format_a_core(char* buf, int exp, uint64_t mant, int precision, bool flag_hash, char x_char, char p_char, int mant_bits) {
    int len = 0;
    buf[len++] = '0';
    buf[len++] = x_char;

    if (mant == 0) {
        buf[len++] = '0';
        if (precision > 0) {
            buf[len++] = '.';
            for (int i = 0; i < precision; i++) buf[len++] = '0';
        }
        else if (flag_hash) {
            buf[len++] = '.';
        }
        buf[len++] = p_char;
        buf[len++] = '+';
        buf[len++] = '0';
        return len;
    }

    buf[len++] = '1';
    uint64_t m = (mant_bits == 52) ? (mant & 0x000FFFFFFFFFFFFFULL) : (mant & 0x7FFFFFFFFFFFFFFFULL);
    int hex_digits = mant_bits / 4;

    if (precision > 0) {
        buf[len++] = '.';
        for (int i = 0; i < precision; i++) {
            if (i < hex_digits) {
                int shift = (mant_bits - 4) - i * 4;
                int digit = (m >> shift) & 0xF;
                buf[len++] = digit < 10 ? '0' + digit : (x_char == 'X' ? 'A' - 10 + digit : 'a' - 10 + digit);
            }
            else {
                buf[len++] = '0';
            }
        }
    }
    else if (flag_hash) {
        buf[len++] = '.';
    }

    buf[len++] = p_char;
    if (exp >= 0) buf[len++] = '+';
    else { buf[len++] = '-'; exp = -exp; }

    char exp_buf[12];
    int exp_len = 0;
    if (exp == 0) {
        exp_buf[exp_len++] = '0';
    }
    else {
        int tmp_exp = exp;
        while (tmp_exp > 0) {
            exp_buf[exp_len++] = '0' + (tmp_exp % 10);
            tmp_exp /= 10;
        }
    }
    for (int i = exp_len - 1; i >= 0; i--) {
        buf[len++] = exp_buf[i];
    }
    return len;
}

/* ==========================================
 * C23 标准 API 实现
 * ========================================== */

static int strfrom_internal(char* __restrict s, size_t n, const char* __restrict format, int sign, int exp, uint64_t mant, int mant_bits, bool is_nan, bool is_inf) {
    const char* fmt = format ? format : "%f";
    const char* p = fmt;
    while (*p && *p != '%') p++;

    if (*p == '\0') {
        int len = p - fmt;
        if (n > 0) {
            size_t copy_len = (size_t)len < n - 1 ? (size_t)len : n - 1;
            memcpy(s, fmt, copy_len);
            s[copy_len] = '\0';
        }
        return len;
    }

    const char* prefix = fmt;
    int prefix_len = p - fmt;

    bool flag_plus = false, flag_space = false, flag_minus = false, flag_zero = false, flag_hash = false;
    int width = 0;
    int precision = -1;
    char specifier = 'f';

    p++;
    while (*p == '+' || *p == ' ' || *p == '-' || *p == '0' || *p == '#') {
        if (*p == '+') flag_plus = true;
        else if (*p == ' ') flag_space = true;
        else if (*p == '-') flag_minus = true;
        else if (*p == '0') flag_zero = true;
        else if (*p == '#') flag_hash = true;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        width = width * 10 + (*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        precision = 0;
        while (*p >= '0' && *p <= '9') {
            precision = precision * 10 + (*p - '0');
            p++;
        }
    }
    while (*p == 'L' || *p == 'l') p++;
    specifier = *p;
    if (specifier) p++;

    const char* suffix = p;
    int suffix_len = 0;
    while (suffix[suffix_len]) suffix_len++;

    if (precision < 0) {
        if (specifier == 'f' || specifier == 'F' || specifier == 'e' || specifier == 'E' || specifier == 'g' || specifier == 'G') precision = 6;
        else if (specifier == 'a' || specifier == 'A') precision = -1;
    }
    if (precision > MAX_PRECISION) precision = MAX_PRECISION;

    char num_buf[BUF_LEN];
    int num_len = 0;

    if (is_nan || is_inf) flag_zero = false;
    if (flag_minus) flag_zero = false;

    if (sign) num_buf[num_len++] = '-';
    else if (flag_plus) num_buf[num_len++] = '+';
    else if (flag_space) num_buf[num_len++] = ' ';

    if (is_nan) {
        memcpy(num_buf + num_len, (specifier == 'F' || specifier == 'E' || specifier == 'G' || specifier == 'A') ? "NAN" : "nan", 3);
        num_len += 3;
    }
    else if (is_inf) {
        memcpy(num_buf + num_len, (specifier == 'F' || specifier == 'E' || specifier == 'G' || specifier == 'A') ? "INF" : "inf", 3);
        num_len += 3;
    }
    else {
        char* num_start = num_buf + num_len;
        int core_len = 0;

        if (specifier == 'f' || specifier == 'F') {
            if (precision < 0) precision = 6;
            core_len = format_f_core(num_start, exp, mant, precision, flag_hash, false);
        }
        else if (specifier == 'e' || specifier == 'E') {
            if (precision < 0) precision = 6;
            core_len = format_e_core(num_start, exp, mant, precision, flag_hash, specifier == 'E' ? 'E' : 'e');
        }
        else if (specifier == 'g' || specifier == 'G') {
            if (precision < 0) precision = 6;
            core_len = format_g_core(num_start, exp, mant, precision, flag_hash, specifier == 'G' ? 'E' : 'e');
        }
        else if (specifier == 'a' || specifier == 'A') {
            bool default_prec = (precision < 0);
            int p_a = default_prec ? mant_bits / 4 : precision;
            core_len = format_a_core(num_start, exp + mant_bits, mant, p_a, flag_hash, specifier == 'A' ? 'X' : 'x', specifier == 'A' ? 'P' : 'p', mant_bits);
            if (!default_prec && !flag_hash) {
                int p_pos = 0;
                while (p_pos < core_len && num_start[p_pos] != 'p' && num_start[p_pos] != 'P') p_pos++;
                int end = p_pos - 1;
                if (num_start[end] == '.') end--;
                while (end > 0 && num_start[end] == '0') end--;
                if (num_start[end] == '.') end--;
                int new_len = end + 1;
                for (int k = p_pos; k < core_len; k++) num_start[new_len++] = num_start[k];
                core_len = new_len;
            }
        }
        else {
            if (precision < 0) precision = 6;
            core_len = format_f_core(num_start, exp, mant, precision, flag_hash, false);
        }
        num_len += core_len;
    }

    int total_len = prefix_len + num_len + suffix_len;
    if (width > num_len) {
        total_len = prefix_len + width + suffix_len;
    }

    if (n > 0) {
        size_t idx = 0;
        for (int i = 0; i < prefix_len && idx < n - 1; i++) s[idx++] = prefix[i];

        int pad_len = width - num_len;
        if (pad_len > 0) {
            if (flag_minus) {
                for (int i = 0; i < num_len && idx < n - 1; i++) s[idx++] = num_buf[i];
                for (int i = 0; i < pad_len && idx < n - 1; i++) s[idx++] = ' ';
            }
            else if (flag_zero) {
                int sign_len = 0;
                if (num_len > 0 && (num_buf[0] == '-' || num_buf[0] == '+' || num_buf[0] == ' ')) {
                    sign_len = 1;
                    if (idx < n - 1) s[idx++] = num_buf[0];
                }
                for (int i = 0; i < pad_len && idx < n - 1; i++) s[idx++] = '0';
                for (int i = sign_len; i < num_len && idx < n - 1; i++) s[idx++] = num_buf[i];
            }
            else {
                for (int i = 0; i < pad_len && idx < n - 1; i++) s[idx++] = ' ';
                for (int i = 0; i < num_len && idx < n - 1; i++) s[idx++] = num_buf[i];
            }
        }
        else {
            for (int i = 0; i < num_len && idx < n - 1; i++) s[idx++] = num_buf[i];
        }

        for (int i = 0; i < suffix_len && idx < n - 1; i++) s[idx++] = suffix[i];
        s[idx] = '\0';
    }

    return total_len;
}

int strfromd(char* __restrict s, size_t n, const char* __restrict format, double fp) {
    uint64_t bits;
    memcpy(&bits, &fp, 8);

    int sign = (int)(bits >> 63);
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0x000FFFFFFFFFFFFFULL;

    bool is_nan = false, is_inf = false;
    if (exp == 0x7FF) {
        if (mant == 0) is_inf = true;
        else is_nan = true;
    }
    else {
        if (exp == 0) {
            exp = 1 - 1023 - 52;
        }
        else {
            mant |= 0x0010000000000000ULL;
            exp = exp - 1023 - 52;
        }
    }
    return strfrom_internal(s, n, format, sign, exp, mant, 52, is_nan, is_inf);
}

int strfromf(char* __restrict s, size_t n, const char* __restrict format, float fp) {
    return strfromd(s, n, format, (double)fp);
}

int strfroml(char* __restrict s, size_t n, const char* __restrict format, long double fp) {
    if (sizeof(long double) == sizeof(double)) {
        return strfromd(s, n, format, (double)fp);
    }
    else {
        uint8_t b[16] = { 0 };
        memcpy(b, &fp, sizeof(long double) < 16 ? sizeof(long double) : 16);
        uint64_t mant;
        memcpy(&mant, b, 8);
        uint16_t exp_sign;
        memcpy(&exp_sign, b + 8, 2);

        int sign = (exp_sign >> 15) & 1;
        int exp_raw = exp_sign & 0x7FFF;

        bool is_nan = false, is_inf = false;
        int exp = 0;

        if (exp_raw == 0x7FFF) {
            if ((mant & 0x7FFFFFFFFFFFFFFFULL) == 0) is_inf = true;
            else is_nan = true;
        }
        else {
            if (exp_raw == 0) {
                exp = 1 - 16383 - 63;
            }
            else {
                exp = exp_raw - 16383 - 63;
            }
        }
        return strfrom_internal(s, n, format, sign, exp, mant, 63, is_nan, is_inf);
    }
}