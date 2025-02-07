#pragma once

#include <arch/x86_64/MStack/MStackS.h>

struct Stackframe
{
    struct Stackframe* ebp;
    uint64_t eip;
};

void PrintMStackTrace(MStack stack[], int64_t size);

namespace MStackData
{
    extern MStack stackArr[];
    extern int64_t stackPointer;

    #define BenchmarkStackSize 10000

    extern MStack BenchmarkStackArr1[];
    extern MStack BenchmarkStackArr2[];
    extern MStack BenchmarkStackArrSave[];

    extern int64_t BenchmarkStackPointer1;
    extern int64_t BenchmarkStackPointer2;
    extern int64_t BenchmarkStackPointerSave;
    extern bool BenchmarkEnabled;
    extern int BenchmarkMode;
}

// void AddToTheMStack(MStack thing);
// void RemoveTheLastElementFromTheMStack();

//#include "../../interrupts/panic.h"
// #include "MStackBRO.h"

// void bleh(MStack a);

void AddToTheMStack(MStack thing);
// {
//     //bleh(thing);
//     //Panic("BRO", true);
//     //_BroAddTheBenchmarkIBegOfYou(thing);
//     // if (MStackData::BenchmarkEnabled)
//     //     _AddBenchmark(thing);

//     if (MStackData::stackPointer < 1000)
//     {
//         MStackData::stackArr[MStackData::stackPointer] = thing;
//         MStackData::stackPointer++;
//     }
//     else
//         Panic("Stack overflow", true);
// }

void RemoveTheLastElementFromTheMStack();
// {
//     // if (MStackData::BenchmarkEnabled)
//     //     _RemoveBenchmark(MStackData::stackArr[MStackData::stackPointer]);

//     if (MStackData::stackPointer > 0)
//         MStackData::stackPointer--;
//     else
//     {
//         MStackData::stackPointer = 900;
//         Panic("Stack underflow", true);
//     }
// }

//#include "../../kernelStuff/stuff/stackmacro.h"

void SaveBenchmarkStack(int mode);



#define RECORD_STACK_TRACE true



#define AddToStack() AddToTheMStack(MStack(__PRETTY_FUNCTION__, __FILE__, __LINE__));

#define RemoveFromStack() RemoveTheLastElementFromTheMStack();


#if !RECORD_STACK_TRACE

    #undef AddToStack
    #define AddToStack() ;

    #undef RemoveFromStack
    #define RemoveFromStack() ;

#endif