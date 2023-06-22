#include "../memory/heap.h"
#include "EfficientMalloc.h"

#define NALLOC 1024

typedef long Align;

union header{
    struct {
        union header *ptr;
        unsigned size;
    }s;
    Align x;
};

typedef union header Header;

static Header base;
static Header *freep = NULL;

static Header *morecore(unsigned int nu);

char *sbrk(intptr_t increment) {  
    static char *heap_end = 0;  
    char *prev_heap_end;  

    if (heap_end == 0) { 
        heap_end = (char *)_Malloc1(1024 * 1024);  // 初始化堆起始位置，以 1MB 为例  
        if (heap_end == NULL) {
            return (char *) -1;
        }
    }
    
    prev_heap_end = heap_end;
    heap_end += increment;  // 修改堆大小
    
    return (char *) prev_heap_end;
}

template<typename T>
T Fmalloc(unsigned int size){
    (T)malloc(size);
}

/* malloc: general-purpose storage allocator */
void *malloc(unsigned int nbytes) {
    Header *p, *prevp;
    //Header *morecore(unsigned);
    unsigned int nunits;

    nunits = (nbytes+sizeof(Header)-1) / sizeof(Header) + 1;
    if ((prevp = freep) == NULL) { /* No free list yet */
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }
    for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
        if (p->s.size >= nunits) { /* Big enough */
            if (p->s.size == nunits) /* Exactly */
                prevp->s.ptr = p->s.ptr;
            else { /* Allocate tail end */
                p->s.size -= nunits;
                p += p->s.size;
                p->s.size = nunits;
            }
            freep = prevp;
            return (void *)(p+1);
        }
        if (p == freep) /* Wrapped around free list */
            if ((p = morecore(nunits)) == NULL)
                return NULL; /* None left */
    }
}

#define NALLOC 1024 /* Minimum #units to request */

/* morecore: ask system for more memory */
static Header *morecore(unsigned int nu) {
    char *cp;
    Header *up;
    if (nu < NALLOC)
        nu = NALLOC;
    cp = sbrk(nu * sizeof(Header));
    if (cp == (char *) -1) /* No space at all */
        return NULL;
    up = (Header *) cp;
    up->s.size = nu;
    free((void *)(up+1));
    return freep;
}

/* free: put block ap in free list */
void free(void *ap) {
    Header *bp, *p;

    bp = (Header *)ap - 1; /* Point to block header */
    for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
        if (p >= p->s.ptr && (bp > p || bp < p->s.ptr))
            break; /* Freed block at start or end of arena */

    if (bp + bp->s.size == p->s.ptr) { /* Join to upper nbr */
        bp->s.size += p->s.ptr->s.size;
        bp->s.ptr = p->s.ptr->s.ptr;
    } else
        bp->s.ptr = p->s.ptr;
    if (p + p->s.size == bp) { /* Join to lower nbr */
        p->s.size += bp->s.size;
        p->s.ptr = bp->s.ptr;
    } else
        p->s.ptr = bp;
    freep = p;
}