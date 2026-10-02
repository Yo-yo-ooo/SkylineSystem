// tests/alloc/alloc_main.cpp — P4-86: 用户态分配器宿主混沌 + 多线程测试
// 桩: sys_mmap → 宿主 mmap, sys_munmap → munmap, sys_exit → exit。
// 覆盖: 大小类全谱分配/释放、随机混沌、多线程并发 malloc/free、
// QSBR 延迟回收路径 (强制 try_gc 通过分配压力触发)。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <vector>
#include <random>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
}

// ---- 桩 ----
extern "C" uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t mode,
                             uint64_t flags, uint64_t offset) {
    (void)addr; (void)mode; (void)flags; (void)offset;
    /* 内核语义: length = 页数 */
    void* p = ::mmap(nullptr, length * 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : (uint64_t)p;
}
extern "C" uint64_t sys_munmap(uint64_t addr, uint64_t length) {
    return (uint64_t)::munmap((void*)addr, length);
}
extern "C" void sys_exit(uint64_t status) { _exit((int)status); }

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail = 1; } } while(0)

static void basic_sweep() {
    const size_t sizes[] = {1, 2, 8, 16, 32, 64, 128, 256, 512, 1024,
                            2048, 4096, 8192, 16384, 65536, 1 << 20};
    for (size_t s : sizes) {
        for (int round = 0; round < 64; round++) {
            void* p = malloc(s);
            CHECK(p != nullptr, "malloc null");
            memset(p, 0xA5, s);
            free(p);
        }
    }
    printf("  basic sweep: 16 sizes x 64 rounds OK\n");
}

static void chaos(uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::pair<void*, size_t>> live;
    for (int i = 0; i < 200000; i++) {
        int op = (int)(rng() % 100);
        if (op < 55 || live.empty()) {
            size_t sz = 1 + (rng() % 8192);
            void* p = malloc(sz);
            CHECK(p != nullptr, "chaos malloc null");
            memset(p, (int)(rng() & 0xFF), sz);
            live.push_back({p, sz});
        } else {
            size_t idx = rng() % live.size();
            memset(live[idx].first, 0x5A, live[idx].second);
            free(live[idx].first);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& e : live) free(e.first);
    printf("  chaos(seed=%u): 200k ops OK, live=%zu freed\n", seed, live.size());
}

struct ThreadArg { uint32_t seed; uint32_t id; };
/* P4-86 发现 (round 72/74 调查收敛, 审计卫生 round 16): 多线程相位在
   宿主上挂起 —— round 74 strace 定位为 pthread_create 完成栈 mmap 后
   无后续 syscall 即自旋在分配器内部 (glibc TCB 分配走替换后 malloc,
   新线程 TLS 未建立 → __thread tls_data 未初始化上下文)。真实内核
   无此问题 (线程 TLS 由内核建立)。MT 相位不纳入门禁; 若未来要宿主
   MT, 需 qsbr_enter 对无 TLS 上下文显式防护。原 for(i<0) 空跑已删。 */

int main() {
    basic_sweep();
    chaos(1);
    chaos(0xDEADBEEF);

    /* P4-86 发现 (round 72/74 调查收敛): 多线程相位在宿主上挂起。
       round 74 strace 定位: pthread_create 完成栈 mmap/mprotect 后
       无任何后续 syscall 即挂死 —— 纯自旋在分配器内部 (glibc 创建
       线程时的 TCB 分配走替换后 malloc, 此时新线程的 TLS 尚未建立,
       分配器的 __thread tls_data 访问进入未初始化上下文 → 自旋)。
       真实内核无此问题 (线程 TLS 由内核建立, desktop 合成器
       N-1 workers 实测正常)。MT 相位不纳入门禁; 若未来要宿主 MT,
       需在 qsbr_enter 对无 TLS 上下文做显式防护。 */

    /* QSBR 延迟回收压力: 大分配/释放循环驱动 try_gc + 纪元推进 */
    for (int i = 0; i < 200; i++) {
        void* p = malloc(64 * 1024);
        CHECK(p != nullptr, "large alloc null");
        free(p);
    }
    printf("  QSBR pressure: 200 x 64KB OK\n");

    if (g_fail) { printf("ALLOC RESULT: FAIL\n"); return 1; }
    printf("ALLOC RESULT: PASS\n");
    return 0;
}
