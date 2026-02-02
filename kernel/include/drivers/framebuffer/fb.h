#pragma once
#ifndef _FRAME_BUFFER_H_
#define _FRAME_BUFFER_H_

#include <stdint.h>
#include <stddef.h>

namespace FrameBufferDevice{

    uint64_t MemoryMap(
        uint64_t length,uint64_t prot,
        uint64_t offset,uint64_t VADDR
    );

    void Init();
}

#endif