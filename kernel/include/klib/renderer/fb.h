#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct FrameBuffer
{
	void* BaseAddress;
	size_t BufferSize;
	uint64_t Width;
	uint64_t Height;
	uint64_t PixelsPerScanLine;
}Framebuffer;

extern Framebuffer* Fb;