//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * display_fb.cpp —— 内建组件 kernel.display.fb（帧缓冲设备）
 * ----------------------------------------------------------------------------
 * 精确迁移自 x86_64_init（原 init.cpp line 259）：`FrameBufferDevice::Init();`
 *   - init：把 Limine 提供的线性帧缓冲注册为 VsDevType::FrameBuffer 虚拟设备
 *           （含 dev_mmap/dev_ioctl 所需的 MemoryMap/ioctl OPS），导出
 *           kernel.display.v1（帧缓冲信息 / flush / 用户态映射）；
 *   - 行为不变：仍调用既有 FrameBufferDevice::Init()，不复制其逻辑；
 *   - 失败语义：Fb 为空（kmain 未取到 framebuffer，实际不可能）返回
 *           KMOD_ERR_STATE；设备注册本身为 void，不 panic。
 * 依赖：kernel.dev.core —— FrameBufferDevice::Init() 里 Dev::AddDevice() 的
 *       设备表（devl.cpp 自带懒初始化，但语义上属于 dev 核心组件，显式声明）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/display.h>

#include <arch/x86_64/schedule/sched.h>
#include <drivers/framebuffer/fb.h>
#include <klib/errno.h>
#include <klib/renderer/fb.h>

static int32_t comp_display_fb_init(void);
static void comp_display_fb_exit(void);

KCOMPONENT_BEGIN(display_fb, "kernel.display.fb", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_DEVICE | KMOD_CAP_MMIO | KMOD_CAP_LOG),
                 0, "Limine framebuffer device registration + kernel.display.v1",
                 comp_display_fb_init, comp_display_fb_exit, 0, 0, 500u, 100u)

KCOMPONENT_IMPORT_FN(display_fb, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(display_fb, "kernel.dev.core", dev_core)

namespace {

bool g_fb_ready = false;

/* ---- kernel.display.v1 ops ---- */
int32_t display_ops_get_info(kmod_display_info_t *out)
{
    if (!out)
        return -EINVAL;
    if (out->version != KMOD_DISPLAY_INFO_VERSION ||
        out->size != (uint32_t)sizeof(kmod_display_info_t))
        return -EINVAL;
    if (!Fb)
        return -ENODEV;
    out->base_phys = (uint64_t)Fb->BaseAddress;
    out->size_bytes = Fb->BufferSize;
    out->width = (uint32_t)Fb->Width;
    out->height = (uint32_t)Fb->Height;
    out->pitch_bytes = (uint32_t)(Fb->PixelsPerScanLine * 4u);
    out->bpp = KMOD_DISPLAY_BPP_32;
    out->flags = KMOD_DISPLAY_F_LINEAR | KMOD_DISPLAY_F_HHDM;
    out->reserved = 0u;
    return 0;
}

int32_t display_ops_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    /* v1：帧缓冲经 HHDM 直接映射，合成器写显存即刻可见，没有独立刷新队列。
     * 返回 0 = "已同步"（诚实声明：不是真正的 DMA 翻页）。 */
    return 0;
}

uint64_t display_ops_map(uint64_t length, uint64_t prot, uint64_t offset,
                         uint64_t vaddr)
{
    /* 映射进当前用户进程；bootstrap/内核线程上下文没有用户 pagemap -> 失败。 */
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->pagemap)
        return 0;
    return FrameBufferDevice::MemoryMap(length, prot, offset, vaddr);
}

kmod_display_ops_t g_display_ops = {
    KMOD_DISPLAY_OPS_VERSION,
    (uint32_t)sizeof(kmod_display_ops_t),
    display_ops_get_info,
    display_ops_flush,
    display_ops_map,
};

} /* namespace */

KCOMPONENT_EXPORT_CONTRACT(display_fb, KMOD_CONTRACT_DISPLAY_V1, g_display_ops, 1)

/* ==================================================================== */
/* 生命周期                                                              */
/* ==================================================================== */
static int32_t comp_display_fb_init(void)
{
    if (g_fb_ready)
        return KMOD_OK; /* 幂等 */

    if (!Fb) {
        kprintf_("[display.fb] init FAILED: Fb is NULL (no Limine framebuffer?)\n");
        return KMOD_ERR_STATE;
    }
    /* 既有初始化：注册 VsDevType::FrameBuffer（MemoryMap/ioctl OPS 由 fb.cpp 提供） */
    FrameBufferDevice::Init();
    g_fb_ready = true;
    kprintf_("[display.fb] ready: %ux%u pitch=%u bytes, dev type FrameBuffer(%u), "
             "kernel.display.v1 published\n",
             (unsigned)Fb->Width, (unsigned)Fb->Height,
             (unsigned)(Fb->PixelsPerScanLine * 4u),
             (unsigned)VsDevType::FrameBuffer);
    return KMOD_OK;
}

static void comp_display_fb_exit(void)
{
    /* 设备表项无法反注册（Dev 层 v1 无 remove API）：exit 只清状态位。 */
    if (g_fb_ready)
        kprintf_("[display.fb] exit: framebuffer device entry stays in Dev table\n");
    g_fb_ready = false;
}

KCOMPONENT_END(display_fb)
