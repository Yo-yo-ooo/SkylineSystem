//SPDX‑FileCopyrightText: 2026 Yo‑yo‑ooo
//SPDX‑License‑Identifier: MIT
#include <string.h>
#include <stdlib.h>

// Calculate recursion depth limit for introsort
static inline int get_depth_limit(size_t nmemb) {
#ifdef _MSC_VER
    unsigned long count;
    _BitScanReverse64(&count, (unsigned __int64)nmemb);
    return (int)count + 3;
#else
    return 64 - __builtin_clzll(nmemb) + 2;
#endif
}

// Forward‑declaration for dispatch to specialized fast‑path
int cmp_int(const void* a, const void* b);

// Macro for generating monomorphized sort routines for concrete types
#define DEFINE_SORT_SPECIALIZED(NAME, TYPE) \
static void NAME##_insertion_sort(TYPE* base, size_t nmemb) { \
    for (size_t i = 1; i < nmemb; i++) { \
        TYPE tmp = base[i]; \
        size_t j = i; \
        while (j > 0 && base[j - 1] > tmp) { \
            base[j] = base[j - 1]; \
            j--; \
        } \
        if (j != i) base[j] = tmp; \
    } \
} \
static void NAME##_sift_down(TYPE* base, size_t start, size_t end) { \
    size_t root = start; \
    while (root * 2 + 1 <= end) { \
        size_t child = root * 2 + 1; \
        if (child + 1 <= end && base[child] < base[child + 1]) child++; \
        if (base[root] < base[child]) { \
            TYPE tmp = base[root]; base[root] = base[child]; base[child] = tmp; \
            root = child; \
        } else return; \
    } \
} \
static void NAME##_heapsort(TYPE* base, size_t nmemb) { \
    if (nmemb < 2) return; \
    size_t end = nmemb - 1; \
    size_t start = (end - 1) / 2; \
    do { NAME##_sift_down(base, start, end); } while (start-- > 0); \
    while (end > 0) { \
        TYPE tmp = base[0]; base[0] = base[end]; base[end] = tmp; \
        end--; NAME##_sift_down(base, 0, end); \
    } \
} \
static void NAME##_introsort(TYPE* base, size_t nmemb, int depth_limit) { \
    while (nmemb > 16) { \
        if (depth_limit == 0) { NAME##_heapsort(base, nmemb); return; } \
        depth_limit--; \
        TYPE* lo = base; \
        TYPE* mid = base + nmemb / 2; \
        TYPE* hi = base + nmemb - 1; \
        if (*lo > *mid) { TYPE t = *lo; *lo = *mid; *mid = t; } \
        if (*lo > *hi) { TYPE t = *lo; *lo = *hi; *hi = t; } \
        if (*mid > *hi) { TYPE t = *mid; *mid = *hi; *hi = t; } \
        TYPE pivot_val = *mid; \
        TYPE* lt = lo; \
        TYPE* gt = hi; \
        TYPE* i = lo; \
        while (i <= gt) { \
            if (*i < pivot_val) { \
                TYPE t = *lt; *lt = *i; *i = t; lt++; i++; \
            } else if (*i > pivot_val) { \
                TYPE t = *i; *i = *gt; *gt = t; gt--; \
            } else { i++; } \
        } \
        size_t left_nmemb = lt - base; \
        size_t right_nmemb = (base + nmemb) - (gt + 1); \
        if (left_nmemb < right_nmemb) { \
            NAME##_introsort(base, left_nmemb, depth_limit); \
            base = gt + 1; nmemb = right_nmemb; \
        } else { \
            NAME##_introsort(gt + 1, right_nmemb, depth_limit); \
            nmemb = left_nmemb; \
        } \
    } \
    if (nmemb > 1) NAME##_insertion_sort(base, nmemb); \
}

// Instantiate specialized implementations for 32‑bit and 64‑bit integers
DEFINE_SORT_SPECIALIZED(i32, int32_t)
DEFINE_SORT_SPECIALIZED(i64, int64_t)

// Generic element‑swap helper, operates on raw byte buffers
static inline void swp(char* a, char* b, size_t size) {
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

// Binary search implementation, overflow‑safe midpoint calculation
void* swar_bsearch(const void* key, const void* base, size_t nmemb, size_t size,
                   int (*compar)(const void*, const void*)) {
    const char* p = (const char*)base;
    size_t l = 0;
    size_t u = nmemb;

    while (l < u) {
        size_t idx = l + (u - l) / 2;
        const void* mid = p + idx * size;
        int cmp = compar(key, mid);

        if (cmp < 0) {
            u = idx;
        } else if (cmp > 0) {
            l = idx + 1;
        } else {
            return (void*)mid;
        }
    }
    return NULL;
}

// Insertion sort for generic elements, stack‑allocated buffer for small element sizes
static void insertion_sort(char* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    if (nmemb <= 1) return;

    char stack_buf[256];
    char* tmp = stack_buf;
    int use_heap = 0;

    if (size > 256) {
        tmp = (char*)malloc(size);
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
        use_heap = 1;
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
            else break;
        }
        if (j != i) memcpy(j, tmp, size);
    }
    if (use_heap) free(tmp);
}

// Heap‑sort sift‑down primitive for generic elements
static void sift_down(char* base, size_t start, size_t end, size_t size, int (*compar)(const void*, const void*)) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        if (child + 1 <= end && compar(base + child * size, base + (child + 1) * size) < 0) child++;
        if (compar(base + root * size, base + child * size) < 0) {
            swp(base + root * size, base + child * size, size);
            root = child;
        } else return;
    }
}

// Generic heapsort fallback for introsort worst‑case protection
static void heapsort_impl(char* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    if (nmemb < 2) return;
    size_t end = nmemb - 1;
    size_t start = (end - 1) / 2;
    do {
        sift_down(base, start, end, size, compar);
    } while (start-- > 0);

    while (end > 0) {
        swp(base, base + end * size, size);
        end--;
        sift_down(base, 0, end, size, compar);
    }
}

// Introsort core: three‑way DNF partition, median‑of‑3 pivot selection
static void introsort(char* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*), int depth_limit) {
    while (nmemb > 16) {
        if (depth_limit == 0) {
            heapsort_impl(base, nmemb, size, compar);
            return;
        }
        depth_limit--;

        char* lo = base;
        char* mid = base + (nmemb / 2) * size;
        char* hi = base + (nmemb - 1) * size;
        if (compar(lo, mid) > 0) swp(lo, mid, size);
        if (compar(lo, hi) > 0) swp(lo, hi, size);
        if (compar(mid, hi) > 0) swp(mid, hi, size);

        char pivot_buf[256];
        int use_buf = (size <= 256);
        if (use_buf) memcpy(pivot_buf, mid, size);
        else swp(lo, mid, size);
        const void* pivot_val = use_buf ? (const void*)pivot_buf : (const void*)lo;

        char* lt = lo;
        char* gt = hi;
        char* i = lo;

        while (i <= gt) {
            int cmp = compar(i, pivot_val);
            if (cmp < 0) {
                swp(lt, i, size);
                lt += size; i += size;
            } else if (cmp > 0) {
                swp(i, gt, size);
                gt -= size;
            } else {
                i += size;
            }
        }

        size_t left_nmemb = (lt - base) / size;
        size_t right_nmemb = (base + nmemb * size - (gt + size)) / size;

        if (left_nmemb < right_nmemb) {
            introsort(base, left_nmemb, size, compar, depth_limit);
            base = gt + size;
            nmemb = right_nmemb;
        } else {
            introsort(gt + 1, right_nmemb, size, compar, depth_limit);
            nmemb = left_nmemb;
        }
    }

    if (nmemb > 1) insertion_sort(base, nmemb, size, compar);
}

// Entry‑point: dispatch to monomorphized fast‑path or generic introsort
void swar_qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    if (nmemb < 2 || size == 0) return;

    if (size == 4 && compar == cmp_int) {
        i32_introsort((int32_t*)base, nmemb, get_depth_limit(nmemb));
        return;
    }else if(size == 8 && compar == cmp_int){
        i64_introsort((int64_t*)base,nmemb, get_depth_limit(nmemb));
        return;
    }

    introsort((char*)base, nmemb, size, compar, get_depth_limit(nmemb));
}

// Standard integer comparator for fast‑path matching
int cmp_int(const void* a, const void* b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

// Libc‑compatible bsearch wrapper
void* bsearch(const void* key, void* base, size_t nmemb, size_t size,
int (*compar)(const void* , const void* )){
    return swar_bsearch(key,base,nmemb,size,compar);
}

// Libc‑compatible qsort wrapper
void qsort(void* base, size_t nmemb, size_t size,
int (*compar)(const void* , const void* )){
    swar_qsort(base,nmemb,size,compar);
}
