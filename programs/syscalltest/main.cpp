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
    //Write Test
    const char* msg = "Hello, SkylineSystem Syscall Test!\n";
    syscall(1, 1, (long)msg, 34, 0, 0, 0); //Syscall Write

    FrameBuffer fb; 
    syscall(5,6,0,(uint64_t)&fb,0,0,0);
    syscall(4,6,0,0,PROT_READ | PROT_WRITE,0,0);

    //Do Exit
    syscall(60, 0, 0, 0, 0, 0, 0); //Syscall Exit
    //Test Exit (PROC Shouldn't run to here) --- Should not print this
    syscall(1, 1, (long)"Should not be printed", 22, 0, 0, 0); 
    while (true);
    return 0;
}