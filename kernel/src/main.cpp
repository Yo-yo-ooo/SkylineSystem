#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "print/e9print.h"
#include "flanterm/flanterm.h"
#include "flanterm/backends/fb.h"
#include "klib/klib.h"
#include "mem/pmm.h"
#include "klib/renderer/fb.h"
#include <klib/renderer/rnd.h>

#if defined (__x86_64__)
extern void __init x86_64_init(void);
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
static volatile struct limine_rsdp_request rsdp_request = {
  .id = LIMINE_RSDP_REQUEST,
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

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_smp_request smp_request = {
  .id = LIMINE_SMP_REQUEST,
  .revision = 0
};


// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;


bool checkStringEndsWith(const char* str, const char* end)
{
    const char* _str = str;
    const char* _end = end;

    while(*str != 0)
        str++;
    str--;

    while(*end != 0)
        end++;
    end--;

    while (true)
    {
        if (*str != *end)
            return false;

        str--;
        end--;

        if (end == _end || (str == _str && end == _end))
            return true;

        if (str == _str)
            return false;
    }

    return true;
}



limine_file* getFile(const char* name)
{
    limine_module_response *module_response = module_request.response;
    for (size_t i = 0; i < module_response->module_count; i++) {
        limine_file *f = module_response->modules[i];
        if (checkStringEndsWith(f->path, name))
            return f;
    }
    return NULL;
}

struct flanterm_context* ft_ctx = NULL;

uint64_t hhdm_offset = 0;
uint64_t paging_mode = 0;
uint64_t RSDP_ADDR = 0;
uint32_t bsp_lapic_id = 0;
uint64_t smp_cpu_count = 0;
struct limine_smp_response* smp_response;
struct limine_framebuffer *fb;
Framebuffer FB;
Framebuffer *Fb;
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
    fb = framebuffer_request.response->framebuffers[0];
    FB.BaseAddress = fb->address;
    FB.Width = fb->width;
    FB.Height = fb->height;
    FB.PixelsPerScanLine = fb->pitch / 4;
    FB.BufferSize = fb->pitch * fb->height;

    Fb = &FB;

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
        kerror("Can not get (limine hhdm request)->response\n");
        hcf();
    }
    hhdm_offset = hhdm_request.response->offset;

    if(paging_mode_request.response == NULL) {
        kerror("Can not get (limine paging_mode_request)->response\n");
        hcf();
    }
    paging_mode = paging_mode_request.response->mode;
    
    if(boot_time_request.response == NULL) {
        kerror("Can not get (limine boot_time_request)->response\n");
        hcf();
    }

    if(rsdp_request.response == NULL){
        kerror("ACPI::Init(): RSDP request is NULL.\n");
    }
    RSDP_ADDR = (rsdp_request.response->address);

    if(smp_request.response == NULL){
        kerror("SMP::Init(): SMP request is NULL.\n");
    }

    smp_response  = smp_request.response;

    bsp_lapic_id = smp_response->bsp_lapic_id;
    smp_cpu_count = smp_response->cpu_count;

    kinfo("Starting kernel...\n");
    kinfo("Boot SkylineSystem kernel time: %ld\n", boot_time_request.response->boot_time);

#ifdef __x86_64__
    x86_64_init();
#endif
    
    // We're done, just hang...
    kpok("Kernel started.\n");
    ft_ctx->clear(ft_ctx, true);    
    kinfoln("Now Kernel is started");
    kinfoln("You can press any key,your press key will display on the srceen");

    hcf();
}


extern "C" {

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = static_cast<uint8_t *>(dest);
    const uint8_t *psrc = static_cast<const uint8_t *>(src);

    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memset(void *s, int32_t c, size_t n) {
    uint8_t *p = static_cast<uint8_t *>(s);

    for (size_t i = 0; i < n; i++) {
        p[i] = static_cast<uint8_t>(c);
    }

    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = static_cast<uint8_t *>(dest);
    const uint8_t *psrc = static_cast<const uint8_t *>(src);

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

int32_t memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = static_cast<const uint8_t *>(s1);
    const uint8_t *p2 = static_cast<const uint8_t *>(s2);

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}

}