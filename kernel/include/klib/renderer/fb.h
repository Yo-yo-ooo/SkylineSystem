// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct alignas(16) FrameBuffer
{
	void* BaseAddress;
	uint64_t BufferSize;
	uint64_t Width;
	uint64_t Height;
	uint64_t PixelsPerScanLine;
}Framebuffer;

extern Framebuffer* Fb;