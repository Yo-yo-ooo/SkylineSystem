#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "print/e9print.h"
#include "flanterm/flanterm.h"
#include "flanterm/backends/fb.h"
#include "klib/klib.h"
// GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
// DO NOT remove or rename these functions, or stuff will eventually break!
// They CAN be moved to a different .c file.


extern "C" void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }

    return dest;
}

extern "C" int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}


extern "C" void* memset(void* d, int c, size_t n) {
    auto* p = static_cast<char*>(d);
    while (n--) {
        *p++ = c;
    }
    return d;
}

extern "C" void* memcpy(void* dest, const void* src, size_t n) {
    auto* p1 = static_cast<char*>(dest);
    auto* p2 = static_cast<const char*>(src);
    while (n--) {
        *p1++ = *p2++;
    }
    return dest;
}


#if defined (__x86_64__)
extern void x86_64_init(void);
#endif
// Set the base revision to 2, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;




struct flanterm_context* ft_ctx = NULL;

uint64_t hhdm_offset = 0;
// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
extern "C" void kmain(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    // Ensure we got a framebuffer.
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    // Fetch the first framebuffer.
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        static_cast<uint32_t*>(fb->address), fb->width, fb->height, fb->pitch,
        fb->red_mask_size, fb->red_mask_shift,
        fb->green_mask_size, fb->green_mask_shift,
        fb->blue_mask_size, fb->blue_mask_shift,
        NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, 0, 0, 1,
        0, 0,
        0
    );

    if(hhdm_request.response == NULL) {
        e9_printf("Can not get (limine hhdm request)->response\n");
        hcf();
    }
    hhdm_offset = hhdm_request.response->offset;

    e9_printf("> Starting kernel...");

    
#ifdef __x86_64__
    x86_64_init();
#endif
    // We're done, just hang...
    e9_printf("> Kernel started! hanging...");
    hcf();
}
