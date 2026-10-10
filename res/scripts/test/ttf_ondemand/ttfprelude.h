#include <stdio.h>
#include <stdint.h>

/* host: force binary mode (Skyline's fopen("r") is O_RDONLY) */
#define fopen(path, mode) fopen(path, "rb")

/* Host stand-in for Skyline's fsize() libc extension. */
static size_t fsize(FILE* f) {
    long pos = ftell(f);
    if (pos < 0) return 0;
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return (size_t)(end < 0 ? 0 : end);
}
