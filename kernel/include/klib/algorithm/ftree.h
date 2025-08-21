#pragma once
#ifndef _FILE_TREE_
#define _FILE_TREE_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>
#include <klib/klib.h>


typedef struct FTreeStruct{
    bool IsFirst;
    void *Elements;
    uint64_t *Next;
};

namespace FTree{
    FTreeStruct* Init();
    FTreeStruct* Find(void *Element,FTreeStruct *Tree);
}


#endif