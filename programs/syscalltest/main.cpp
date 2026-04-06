#include "syscall.h"

typedef struct FrameBuffer
{
	void* BaseAddress;
	size_t BufferSize;
	uint64_t Width;
	uint64_t Height;
	uint64_t PixelsPerScanLine;
}Framebuffer;

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1

int main(){
    const char *msg = "Hello, World!";
    FrameBuffer fb;
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    syscall(25, 6, (long)&fb, sizeof(Framebuffer), 0, 0, 0); // Get device info
    
    while (true);
    return 0;
}