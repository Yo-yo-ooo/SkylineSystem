//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/display.h —— 私有契约 kernel.display.v1
 * ============================================================================
 * 提供者：内建组件 kernel.display.fb（kernel/src/arch/x86_64/components/display_fb.cpp）
 * 消费者：用户态桌面/合成器的内核侧代理组件（kernel.user.init 间接使用），
 *         以及未来 .ko 形式的显示设备/覆盖层实现（可热替换 provider）。
 *
 * 对应现有实现：kernel/src/drivers/framebuffer/fb.cpp
 *   - FrameBufferDevice::Init()        -> 组件 init 调用（注册 VsDevType::FrameBuffer）
 *   - FrameBufferDevice::MemoryMap()   -> display.map（用户态 mmap 帧缓冲）
 *   - Fb（全局 Framebuffer*）          -> display.get_info
 *   - 显存由 Limine 直接映射（HHDM），无独立刷新队列 -> display.flush 为 no-op
 *     （诚实声明：置 1 表示"已同步"，不是真的有 DMA 翻转）。
 *
 * 布局冻结：首部固定 { uint32_t version; uint32_t size; }，static_assert 钉死。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACTS_DISPLAY_H_
#define _KMOD_CONTRACTS_DISPLAY_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMOD_CONTRACT_DISPLAY_V1  "kernel.display.v1"

#define KMOD_DISPLAY_OPS_VERSION  1u
#define KMOD_DISPLAY_INFO_VERSION 1u

/* info.flags */
#define KMOD_DISPLAY_F_TEXT_MODE  (1u << 0)
#define KMOD_DISPLAY_F_LINEAR     (1u << 1)   /* 线性 pitch（现有实现恒真） */
#define KMOD_DISPLAY_F_HHDM       (1u << 2)   /* base_phys 可经 HHDM 直接访问 */

/* 像素格式（v1 只有 32bpp XRGB8888 由现有 fb 保证；其余位预留） */
#define KMOD_DISPLAY_BPP_32       32u

/* ------------------------------------------------------------------ */
/* 帧缓冲信息。布局冻结：48 字节。                                    */
/* ------------------------------------------------------------------ */
typedef struct kmod_display_info {
    uint32_t version;                       /* = KMOD_DISPLAY_INFO_VERSION */
    uint32_t size;                          /* = sizeof(kmod_display_info_t) */
    uint64_t base_phys;                     /* 物理基址（HHDM 下 +hhdm_offset 可写） */
    uint64_t size_bytes;                    /* pitch * height */
    uint32_t width;                         /* 像素 */
    uint32_t height;                        /* 像素 */
    uint32_t pitch_bytes;                   /* 每扫描行字节数 */
    uint32_t bpp;                           /* KMOD_DISPLAY_BPP_32 */
    uint32_t flags;                         /* KMOD_DISPLAY_F_* */
    uint32_t reserved;                      /* 必须为 0 */
} kmod_display_info_t;

/* ------------------------------------------------------------------ */
/* 显示契约 ops。布局冻结：32 字节（8 + 3 个函数指针）。              */
/* map(): 把帧缓冲映射到当前用户进程；返回用户 VADDR，0 = 失败。      */
/*        等价 FrameBufferDevice::MemoryMap(length,prot,offset,VADDR)。*/
/* flush(): v1 直接映射显示，恒返回 0（无显式刷新）；保留给未来 DMA 实现。*/
/* ------------------------------------------------------------------ */
typedef struct kmod_display_ops {
    uint32_t version;                       /* = KMOD_DISPLAY_OPS_VERSION */
    uint32_t size;                          /* = sizeof(kmod_display_ops_t) */
    int32_t (*get_info)(kmod_display_info_t *out);
    int32_t (*flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    uint64_t (*map)(uint64_t length, uint64_t prot, uint64_t offset,
                    uint64_t vaddr);
} kmod_display_ops_t;

#if defined(__cplusplus)
static_assert(sizeof(kmod_display_info_t) == 48u,
              "kmod_display_info_t layout must stay 48 bytes (kernel.display.v1)");
static_assert(sizeof(kmod_display_ops_t) == 32u,
              "kmod_display_ops_t layout must stay 32 bytes (kernel.display.v1)");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _KMOD_CONTRACTS_DISPLAY_H_ */
