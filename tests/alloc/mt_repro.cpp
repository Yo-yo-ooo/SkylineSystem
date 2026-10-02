// tests/alloc/mt_repro.cpp — MT 挂起最小复现 (round 74 调查)
// 假设: pthread_create 内部经替换后 malloc 走分配器路径时挂起。
#include <cstdio>
#include <cstdint>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>

extern "C" {
void* malloc(size_t size);
void free(void* ptr);
}

extern "C" uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t mode,
                             uint64_t flags, uint64_t offset) {
    (void)addr; (void)mode; (void)flags; (void)offset;
    void* p = ::mmap(nullptr, length * 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : (uint64_t)p;
}
extern "C" uint64_t sys_munmap(uint64_t addr, uint64_t length) {
    return (uint64_t)::munmap((void*)addr, length);
}
extern "C" void sys_exit(uint64_t status) { _exit((int)status); }

static void* worker(void*) {
    fprintf(stderr, "[t] worker entered\n");
    void* p = malloc(64);
    fprintf(stderr, "[t] malloc(64)=%p\n", p);
    free(p);
    fprintf(stderr, "[t] free done\n");
    return nullptr;
}

int main() {
    fprintf(stderr, "[m] pre-warm malloc\n");
    void* w = malloc(64);
    fprintf(stderr, "[m] pre-warm malloc=%p\n", w);
    free(w);
    fprintf(stderr, "[m] spawning thread\n");
    pthread_t th;
    pthread_create(&th, nullptr, worker, nullptr);
    fprintf(stderr, "[m] join\n");
    pthread_join(th, nullptr);
    fprintf(stderr, "[m] joined\n");
    return 0;
}
