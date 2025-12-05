#include <arch/x86_64/entropy/ep.h>
#include <mem/heap.h>

void EntropyPool::AddEntropy(void *entropy, uint64_t size){
    entropy_list_t *buf;
    buf->entropy = entropy;
    buf->size = size;
    buf->next = nullptr;
    this->record.Tail->next = buf;
    this->record.Tail = this->record.Tail->next;
}
void EntropyPool::AddEntropy(uint8_t entropy){
    this->AddEntropy((void*)&entropy, 1);
}
void EntropyPool::AddEntropy(uint16_t entropy){
    this->AddEntropy((void*)&entropy, 2);
}
void EntropyPool::AddEntropy(uint32_t entropy){
    this->AddEntropy((void*)&entropy, 4);
}
void EntropyPool::AddEntropy(uint64_t entropy){
    this->AddEntropy((void*)&entropy, 8);
}

