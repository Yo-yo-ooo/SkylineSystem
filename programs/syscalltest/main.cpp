#include "syscall.h"

typedef struct FrameBuffer
{
	void* BaseAddress;
	size_t BufferSize;
	uint64_t Width;
	uint64_t Height;
	uint64_t PixelsPerScanLine;
}Framebuffer;


int main(){
    //Write Test
    const char* msg = "Hello, SkylineSystem Syscall Test!\n";
    syscall(1, 1, (long)msg, 34, 0, 0, 0); //Syscall Write

   

    //Do Exit
    syscall(60, 0, 0, 0, 0, 0, 0); //Syscall Exit
    //Test Exit (PROC Shouldn't run to here) --- Should not print this
    syscall(1, 1, (long)"Should not be printed", 22, 0, 0, 0); 
    while (true);
    return 0;
}