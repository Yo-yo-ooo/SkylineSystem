#include "rnd.h"
#include "fb.h"
#include "../../flanterm/flanterm.h"
#include "../../flanterm/backends/fb.h"

extern struct flanterm_context* ft_ctx;

namespace Renderer
{
    void Clear(uint32_t col)
    {
        uint64_t fbBase = (uint64_t)Fb->BaseAddress;
        uint64_t pxlsPerScanline = Fb->PixelsPerScanLine;
        uint64_t fbHeight = Fb->Height;

        for (int64_t y = 0; y < Fb->Height; y++)
            for (int64_t x = 0; x < Fb->Width; x++)
                *((uint32_t*)(fbBase + 4 * (x + pxlsPerScanline * y))) = col;

        ft_ctx->clear(ft_ctx, true);       
    }
}