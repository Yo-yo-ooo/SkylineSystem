#pragma once
#include <stdint.h>
#include <stddef.h>
//#include "../../cStdLib/cstrTools.h"

struct MStack
{
    const char* name;
    const char* filename;
    int32_t line;

    int32_t layer;
    uint64_t time;
    bool close;

    MStack(const char* name, const char* filename, int32_t line, int32_t layer, uint64_t time, bool close)
    ;

    MStack(const char* name, const char* filename, int32_t line)
    ;

    MStack(const char* name, const char* filename)
    ;

    MStack()
    ;

    bool operator==(MStack other)
    ;
};

