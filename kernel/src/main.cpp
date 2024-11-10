#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "print/e9print.h"
#include "flanterm/flanterm.h"
#include "flanterm/backends/fb.h"
#include "klib/klib.h"
#include "mem/pmm.h"


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

__attribute__((used, section(".requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_boot_time_request boot_time_request = {
    .id = LIMINE_BOOT_TIME_REQUEST,
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
uint64_t paging_mode = 0;
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
        e9_printf("[INFO] Can not get (limine hhdm request)->response\n");
        hcf();
    }
    hhdm_offset = hhdm_request.response->offset;

    if(paging_mode_request.response == NULL) {
        e9_printf("[INFO] Can not get (limine paging_mode_request)->response\n");
        hcf();
    }
    paging_mode = paging_mode_request.response->mode;
    
    if(boot_time_request.response == NULL) {
        e9_printf("[INFO] Can not get (limine boot_time_request)->response\n");
        hcf();
    }

    e9_printf("[INFO] Starting kernel...");
    kprintf("[INFO] Boot SkylineSystem kernel time: %ld\n", boot_time_request.response->boot_time);
    
#ifdef __x86_64__
    x86_64_init();
#endif
    
    // We're done, just hang...
    kprintf("[INFO] \033[38;2;0;255;255mSkylineSystem\033[0m Booted successfully hanging...");
    hcf();
}
