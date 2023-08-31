#pragma once
#include <stddef.h>

#include <libm/rendering/framebuffer.h>

struct PointerFramebuffer
{
	void* BaseAddress;
	size_t BufferSize;
	unsigned int Width;
	unsigned int Height;
};
