#pragma once
#ifndef _ENTROPY_POOL_H_
#define _ENTROPY_POOL_H_

#include <stdint.h>
#include <stddef.h>

typedef struct entropy_list_t {
    entropy_list_t *next; // next entropy in the list
    void *entropy;
    uint64_t size; // size of entropy data
} entropy_list_t;

typedef struct MasterEntropyRecord{
    entropy_list_t *Head;
    entropy_list_t *Tail;
}MasterEntropyRecord;

class EntropyPool
{
private:
    MasterEntropyRecord record;
public:
    EntropyPool();
    ~EntropyPool();
    void AddEntropy(void *entropy, uint64_t size);
    void AddEntropy(uint8_t entropy);
    void AddEntropy(uint16_t entropy);
    void AddEntropy(uint32_t entropy);
    void AddEntropy(uint64_t entropy);
};

#endif