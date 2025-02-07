#pragma once

#include <stdint.h>

namespace Renderer
{
    void Clear(uint32_t col);
} // namespace Renderer


static struct col
{ 
    static const uint32_t 
    white =  0xffffffff,
    silver = 0xffc0c0c0,
    gray =   0xff808080,
    bgray =  0xffC0C0C0,
    dgray =  0xff404040,
    black =  0xff000000,
    pink =   0xffFF1493,
    green =  0xff008000,
    red =    0xff800000,
    purple = 0xff800080,
    orange = 0xffFF4500,
    cyan =   0xff008080,
    yellow = 0xffFFD700,
    brown =  0xffA52A2A,
    blue =   0xff000080,
    dblue =  0xff000030,
    bred =   0xffFF0000,
    bblue =  0xff0000FF,
    bgreen = 0xff00FF00,
    tblack =  0x00000000,
    lime =    0xffC0FF00;
} Colors;