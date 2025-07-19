#include <arch/x86_64/allin.h>

void PrintMStackTrace(MStack stack[], int64_t size)
{
#if RECORD_STACK_TRACE  
    int32_t count = 0;
    for (int32_t i = 0; i < size; i++)
    {
        int32_t index = (size - i) - 1;
        if (stack[index].line != -1)
            count++;
    }
    //renderer->Println("STACK TRACE: ({} Elements)\n", to_string(count), col);
    //Serial::TWritelnf("STACK TRACE: (%d Elements)\n", count);
    kinfo("STACK TRACE: (%d Elements)\n", count);
    if (size != 1000)
    {
        if (size > 50)
            size = 50;
        for (int32_t i = 0; i < size; i++)
        {
            int32_t index = (size - i) - 1;
            if (stack[index].line != -1)
            {
                printf_("> At \"{}\"\n", stack[index].name);
                printf_("  > in file \"{}\" ", stack[index].filename);
                printf_("at line \"{}\"\n", to_string(stack[index].line));
            }
        }
    }
    else
    {
        if (size > 50)
            size = 50;
        for (int32_t i = 0; i < size; i++)
        {
            int32_t index = i;
            if (stack[index].line != -1)
            {
                printf_("> At \"{}\"\n", stack[index].name);
                printf_("  > in file \"{}\" ", stack[index].filename);
                printf_("at line \"{}\"\n", to_string(stack[index].line));
            }
        }
    }
    printf_("\n");
    //renderer->Println();
    //Serial::Writeln();

#else

    kerror("STACK TRACE: (STACK TRACE DISABLED)\n");

#endif
}

namespace MStackData
{
    MStack stackArr[1000];
    int64_t stackPointer = 0;

    MStack BenchmarkStackArr1[BenchmarkStackSize];
    MStack BenchmarkStackArr2[BenchmarkStackSize];
    MStack BenchmarkStackArrSave[BenchmarkStackSize];

    int64_t BenchmarkStackPointer1 = 0;
    int64_t BenchmarkStackPointer2 = 0;
    int64_t BenchmarkStackPointerSave = 0;
    bool BenchmarkEnabled = false;
    int32_t BenchmarkMode = 0;
};

void _AddTheBenchmark(MStack thing)
{
    if (PIT::Inited)
        thing.time = PIT::TimeSinceBootMicroS();
    else
        thing.time = 0;
    thing.layer = MStackData::stackPointer + 1;   
    thing.close = false;

    if (MStackData::BenchmarkMode == 0)
    {
        if (MStackData::BenchmarkStackPointer1 >= BenchmarkStackSize)
        {
            return;
            MStackData::BenchmarkStackPointer1 = 0;
            MStackData::BenchmarkEnabled = false;
            Panic("BENCHMARK OUT OF BOUNDS!", true);
        }
        MStackData::BenchmarkStackArr1[MStackData::BenchmarkStackPointer1++] = thing;
    }
    else
    {
        if (MStackData::BenchmarkStackPointer2 >= BenchmarkStackSize)
        {
            return;
            MStackData::BenchmarkStackPointer2 = 0;
            MStackData::BenchmarkEnabled = false;
            Panic("BENCHMARK OUT OF BOUNDS!", true);
        }
        MStackData::BenchmarkStackArr2[MStackData::BenchmarkStackPointer2++] = thing;
    }
}

void _RemoveBenchmark(MStack thing)
{
    if (PIT::Inited)
        thing.time = PIT::TimeSinceBootMicroS();
    else
        thing.time = 0;
    thing.layer = MStackData::stackPointer;   
    thing.close = true;

    if (MStackData::BenchmarkMode == 0)
    {
        if (MStackData::BenchmarkStackPointer1 >= BenchmarkStackSize)
        {
            return;
            MStackData::BenchmarkStackPointer1 = 0;
            MStackData::BenchmarkEnabled = false;
            Panic("BENCHMARK OUT OF BOUNDS!", true);
        }
        MStackData::BenchmarkStackArr1[MStackData::BenchmarkStackPointer1++] = thing;
    }
    else
    {
        if (MStackData::BenchmarkStackPointer2 >= BenchmarkStackSize)
        {
            return;
            MStackData::BenchmarkStackPointer2 = 0;
            MStackData::BenchmarkEnabled = false;
            Panic("BENCHMARK OUT OF BOUNDS!", true);
        }
        MStackData::BenchmarkStackArr2[MStackData::BenchmarkStackPointer2++] = thing;
    }
}














void AddToTheMStack(MStack thing)
{
    if (MStackData::BenchmarkEnabled)
        _AddTheBenchmark(thing);

    if (MStackData::stackPointer < 1000)
    {
        MStackData::stackArr[MStackData::stackPointer] = thing;
        MStackData::stackPointer++;
    }
    else
        Panic("Stack overflow", true);
}

void RemoveTheLastElementFromTheMStack()
{
    if (MStackData::BenchmarkEnabled)
        _RemoveBenchmark(MStackData::stackArr[MStackData::stackPointer-1]);

    if (MStackData::stackPointer > 0)
        MStackData::stackPointer--;
    else
    {
        MStackData::stackPointer = 900;
        Panic("Stack underflow", true);
    }
}


void SaveBenchmarkStack(int32_t mode)
{
    if (mode == 0)
    {
        for (int32_t i = 0; i < MStackData::BenchmarkStackPointer1; i++)
            MStackData::BenchmarkStackArrSave[i] = MStackData::BenchmarkStackArr1[i];
        MStackData::BenchmarkStackPointerSave = MStackData::BenchmarkStackPointer1;
    }
    else
    {
        for (int32_t i = 0; i < MStackData::BenchmarkStackPointer2; i++)
            MStackData::BenchmarkStackArrSave[i] = MStackData::BenchmarkStackArr2[i];
        MStackData::BenchmarkStackPointerSave = MStackData::BenchmarkStackPointer2;
    }
}