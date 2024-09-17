#include <limine.h>

typedef struct KernelBootInfo{
    struct limine_framebuffer *fb;
    struct limine_memmap_entry *memmap;
}KBInfo;