#include <string.h>
#include <stdlib.h>

static inline void swp(char* a, char* b, size_t size) {
    if (__builtin_constant_p(size)) {
        if (size == 4) {
            uint32_t va, vb;
            memcpy(&va, a, 4); memcpy(&vb, b, 4);
            memcpy(a, &vb, 4); memcpy(b, &va, 4);
            return;
        }
        else if (size == 8) {
            uint64_t va, vb;
            memcpy(&va, a, 8); memcpy(&vb, b, 8);
            memcpy(a, &vb, 8); memcpy(b, &va, 8);
            return;
        }
    }

    if (size == 4) {
        uint32_t va, vb;
        memcpy(&va, a, 4); memcpy(&vb, b, 4);
        memcpy(a, &vb, 4); memcpy(b, &va, 4);
    }
    else if (size == 8) {
        uint64_t va, vb;
        memcpy(&va, a, 8); memcpy(&vb, b, 8);
        memcpy(a, &vb, 8); memcpy(b, &va, 8);
    }
    else {
        size_t i = 0;
        for (; i + sizeof(uintmax_t) <= size; i += sizeof(uintmax_t)) {
            uintmax_t va, vb;
            memcpy(&va, a + i, sizeof(uintmax_t));
            memcpy(&vb, b + i, sizeof(uintmax_t));
            memcpy(a + i, &vb, sizeof(uintmax_t));
            memcpy(b + i, &va, sizeof(uintmax_t));
        }
        for (; i < size; i++) {
            char tmp = a[i]; a[i] = b[i]; b[i] = tmp;
        }
    }
}

// SWAR accelerate -> bound search/binaray search
void* swar_lower_bound(const void* key, const void* base, size_t nmemb, size_t size,
    int (*compar)(const void*, const void*)) {
    const char* p = (const char*)base;
    size_t i = 0;
    size_t n = nmemb;

    while (n > 0) {
        size_t half = n / 2;
        size_t mid = i + half;
        int cmp = compar(key, p + mid * size);

        size_t mask = (size_t)0 - (size_t)(cmp > 0);
        size_t n_right = n - half - 1;

        i = (i & ~mask) | ((mid + 1) & mask);
        n = (half & ~mask) | (n_right & mask);
    }
    return (void*)(p + i * size);
}

void* swar_bsearch(const void* key, const void* base, size_t nmemb, size_t size,
    int (*compar)(const void*, const void*)) {
    const char* p = (const char*)base;
    const char* found = (const char*)swar_lower_bound(key, base, nmemb, size, compar);

    if (found < p + nmemb * size && compar(key, found) == 0) {
        return (void*)found;
    }
    return NULL;
}

// Qsort OPT: 3-rd way sort + 轴心暂存优化
static void insertion_sort(char* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    if (nmemb <= 1) return;

    char* tmp = (char*)malloc(size);
    if (!tmp) {
        for (size_t i = 1; i < nmemb; i++) {
            for (size_t j = i; j > 0; j--) {
                char* cur = base + j * size;
                char* prev = base + (j - 1) * size;
                if (compar(prev, cur) > 0) swp(prev, cur, size);
                else break;
            }
        }
        return;
    }

    char* end = base + nmemb * size;
    for (char* i = base + size; i < end; i += size) {
        memcpy(tmp, i, size);
        char* j = i;
        while (j > base) {
            char* prev = j - size;
            if (compar(prev, tmp) > 0) {
                memcpy(j, prev, size);
                j -= size;
            }
            else {
                break;
            }
        }
        if (j != i) {
            memcpy(j, tmp, size);
        }
    }
    free(tmp);
}

static void sift_down(char* base, size_t start, size_t end, size_t size, int (*compar)(const void*, const void*)) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        if (child + 1 <= end && compar(base + child * size, base + (child + 1) * size) < 0) {
            child++;
        }
        if (compar(base + root * size, base + child * size) < 0) {
            swp(base + root * size, base + child * size, size);
            root = child;
        }
        else {
            return;
        }
    }
}

static void heapsort_impl(char* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    if (nmemb < 2) return;
    size_t end = nmemb - 1;
    for (size_t start = (end - 1) / 2; start > 0; start--) {
        sift_down(base, start, end, size, compar);
    }
    sift_down(base, 0, end, size, compar);
    while (end > 0) {
        swp(base, base + end * size, size);
        end--;
        sift_down(base, 0, end, size, compar);
    }
}

static void introsort(char* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*), int depth_limit) {
    while (nmemb > 16) {
        if (depth_limit == 0) {
            heapsort_impl(base, nmemb, size, compar);
            return;
        }
        depth_limit--;

        // Median-of-3 选轴
        char* lo = base;
        char* mid = base + (nmemb / 2) * size;
        char* hi = base + (nmemb - 1) * size;
        if (compar(lo, mid) > 0) swp(lo, mid, size);
        if (compar(lo, hi) > 0) swp(lo, hi, size);
        if (compar(mid, hi) > 0) swp(mid, hi, size);

        // 轴心暂存微优化：小对象拷贝到栈缓冲区，避免后续无意义的自身交换
        // 大对象回退到交换至 lo 位置，规避栈溢出风险
        char pivot_buf[256];
        int use_buf = (size <= 256);
        if (use_buf) {
            memcpy(pivot_buf, mid, size);
        }
        else {
            swp(lo, mid, size);
        }
        const void* pivot_val = use_buf ? (const void*)pivot_buf : (const void*)lo;

        // Dijkstra 三路划分
        char* lt = lo;
        char* gt = hi;
        char* i = lo;

        while (i <= gt) {
            int cmp = compar(i, pivot_val);
            if (cmp < 0) {
                swp(lt, i, size);
                lt += size;
                i += size;
            }
            else if (cmp > 0) {
                swp(i, gt, size);
                gt -= size;
            }
            else {
                i += size;
            }
        }

        size_t left_nmemb = (lt - base) / size;
        size_t right_nmemb = (base + nmemb * size - (gt + size)) / size;

        if (left_nmemb < right_nmemb) {
            introsort(base, left_nmemb, size, compar, depth_limit);
            base = gt + size;
            nmemb = right_nmemb;
        }
        else {
            introsort(gt + size, right_nmemb, size, compar, depth_limit);
            nmemb = left_nmemb;
        }
    }

    if (nmemb > 1) {
        insertion_sort(base, nmemb, size, compar);
    }
}

void swar_qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    if (nmemb < 2 || size == 0) return;

    int depth_limit = 2;
    for (size_t n = nmemb; n > 1; n >>= 1) depth_limit++;

    introsort((char*)base, nmemb, size, compar, depth_limit);
}


void* bsearch(const void* key, void* base, size_t nmemb, size_t size,
int (*compar)(const void* , const void* )){
    return swar_bsearch(key,base,nmemb,size,compar);
}
void qsort(void* base, size_t nmemb, size_t size,
int (*compar)(const void* , const void* )){
    swar_qsort(base,nmemb,size,compar);
}