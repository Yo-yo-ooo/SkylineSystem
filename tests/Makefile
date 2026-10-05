# tests/Makefile — Kernel Core ALGO / ABLIB TEST
CXX      := g++
CC       := gcc
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
            -pthread -D__KERNEL_TEST_HOST__ -I common -I ../kernel/include
CFLAGS   := -O2 -g -Wall -Wno-unused-parameter -Wno-unused-function -D__KERNEL_TEST_HOST__ \
            -I common -I ../kernel/include
LDLIBS   := -pthread

HEAP_SRC := ../kernel/src/mem/heap.cpp ../kernel/src/mem/slab.cpp ../kernel/src/mem/slub.cpp
ART_SRC  := ../kernel/src/klib/algorithm/art.c
FC_SRC   := ../kernel/src/fs/fc.cpp ../kernel/src/fs/fc_idle.cpp

# P4-89: 游离工具纳入目标 (art_mini = ART 前缀冲突隔离, art_replay = 复现工具)
art-mini: bin/art_mini
bin/art_mini: art_mini.cpp common/kernel_shim.cpp $(HEAP_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

art-replay: bin/art_replay
bin/art_replay: art_replay.cpp common/kernel_shim.cpp $(HEAP_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# P4-86: 用户态分配器宿主测试 (allocator.c + mmap 桩)
alloc: bin/alloc_test
bin/alloc_test: alloc/alloc_main.cpp ../lib/stdc/allocator/allocator.c ../lib/stdc/allocator/sct.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -I ../lib/include -I ../ablib -c ../lib/stdc/allocator/allocator.c -o bin/allocator_host.o
	$(CC) $(CFLAGS) -I ../lib/include -I ../ablib -c ../lib/stdc/allocator/sct.c -o bin/sct_host.o
	$(CXX) $(CXXFLAGS) -I ../lib/include -I ../ablib \
	  alloc/alloc_main.cpp bin/allocator_host.o bin/sct_host.o -o $@ $(LDLIBS)

# 债务 #6: 内核 vsnprintf 宿主模糊测试 (ASAN+UBSAN)
fuzz: bin/kprintf_fuzz
bin/kprintf_fuzz: fuzz/kprintf_fuzz.cpp fuzz/fuzz_shim.cpp ../kernel/src/klib/kprintf.cpp
	@mkdir -p bin
	$(CXX) -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -D__KERNEL_TEST_HOST__ -I common -I ../kernel/include -I ../ablib \
	  fuzz/kprintf_fuzz.cpp fuzz/fuzz_shim.cpp ../kernel/src/klib/kprintf.cpp \
	  -o $@ -pthread

# ============================================================================
# x86mem (ablib/arch/x86_64/x86mem) 宿主正确性 + 基准
#   四个指令集 tier 分别编译后 ld -r 合成胖对象, 再与测试/基准 TU 链接。
#   V0..V3 的取舍由 CPUID + MEMOPS_SupportVn 在运行期决定, 未支持的 tier
#   自动跳过 (V3 需宿主支持 AVX512F, 否则仅做编译/链接校验)。
# ============================================================================
MEM_DIR  := ../ablib/arch/x86_64/x86mem
MEM_SRCS := $(addprefix $(MEM_DIR)/,memcpy.c memmove.c memset.c memcmp.c x86mem_init.c)
MEM_INC  := -I $(MEM_DIR) -isystem ../ablib/freestndchdrs/x86_64/include
MEM_CFLAGS := -O3 -g3 --std=gnu17 -Wall -Wextra -Wno-unused-parameter \
              $(MEM_INC) -fno-merge-all-constants -ffunction-sections \
              -fomit-frame-pointer

MEM_FLAGS_base   := -msse4.2 -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512
MEM_FLAGS_avx    := -mavx -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512
MEM_FLAGS_avx2   := -mavx -mavx2 -DX86MEM_NOT_COMPILE_AVX512
MEM_FLAGS_avx512 := -mavx -mavx2 -mavx512f -mprefer-vector-width=512
MEM_TIERS := base avx avx2 avx512

# 工具链指纹: Windows(MinGW/COFF) 与 WSL(ELF) 交叉构建共享 bin/ 时, make 按
# 时间戳会复用对方格式的对象 (COFF 对象被 ELF 链接器处理时报
# R_AMD64_IMAGEBASE with __ImageBase undefined 的危险重寻址)。指纹变化 →
# 所有 tier 对象与 shim 对象强制重编, 无需手工 make clean。
CC_ID := $(shell $(CC) -dumpmachine 2>/dev/null || printf unknown)
.PHONY: FORCE
FORCE:

bin/.toolchain-id: FORCE
	@mkdir -p bin
	@if [ "$$(cat $@ 2>/dev/null)" != "$(CC_ID)" ]; then printf '%s\n' '$(CC_ID)' > $@; fi

define MEM_TIER_RULE
bin/memops_$(1).o: $(MEM_SRCS) bin/.toolchain-id
	@mkdir -p bin
	$(CC) $(MEM_CFLAGS) $(MEM_FLAGS_$(1)) -c $(MEM_DIR)/memcpy.c  -o bin/memcpy_$(1).o
	$(CC) $(MEM_CFLAGS) $(MEM_FLAGS_$(1)) -c $(MEM_DIR)/memmove.c -o bin/memmove_$(1).o
	$(CC) $(MEM_CFLAGS) $(MEM_FLAGS_$(1)) -c $(MEM_DIR)/memset.c  -o bin/memset_$(1).o
	$(CC) $(MEM_CFLAGS) $(MEM_FLAGS_$(1)) -c $(MEM_DIR)/memcmp.c  -o bin/memcmp_$(1).o
	$(CC) $(MEM_CFLAGS) $(MEM_FLAGS_$(1)) -c $(MEM_DIR)/x86mem_init.c -o bin/x86mem_init_$(1).o
	# 用 $(CC) -r 而不是裸 ld: 编译器驱动保证调用与 CC 目标匹配的链接器
	# (WSL/ELF 与 Windows/MinGW 工具链各自一致; 裸 ld 受 PATH 影响, 曾出现
	#  ELF gcc + PE ld 误选 emulation, 处理 Windows 侧残留的 COFF 对象时报
	#  R_AMD64_IMAGEBASE with __ImageBase undefined 的危险重寻址)
	$(CC) -r bin/memcpy_$(1).o bin/memmove_$(1).o bin/memset_$(1).o bin/memcmp_$(1).o \
	      bin/x86mem_init_$(1).o -o $$@
	@rm -f bin/memcpy_$(1).o bin/memmove_$(1).o bin/memset_$(1).o bin/memcmp_$(1).o \
	      bin/x86mem_init_$(1).o
endef
$(foreach t,$(MEM_TIERS),$(eval $(call MEM_TIER_RULE,$(t))))

MEMOPS_OBJS := $(foreach t,$(MEM_TIERS),bin/memops_$(t).o)

# MEMOPS_SupportVn 的强定义 (PE-COFF 下必需, 见 memops/memops_shim.c 注释)
bin/memops_shim.o: memops/memops_shim.c bin/.toolchain-id
	@mkdir -p bin
	$(CC) $(CFLAGS) -c memops/memops_shim.c -o $@

memops: bin/memops_test
bin/memops_test: memops/main.cpp bin/memops_shim.o $(MEMOPS_OBJS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(MEM_INC) memops/main.cpp bin/memops_shim.o $(MEMOPS_OBJS) -o $@ $(LDLIBS)

memops-bench: bin/memops_bench
bin/memops_bench: memops/bench.cpp bin/memops_shim.o $(MEMOPS_OBJS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(MEM_INC) memops/bench.cpp bin/memops_shim.o $(MEMOPS_OBJS) -o $@ $(LDLIBS)

# ASAN 变体: 抓 UAF/越界。
# 注意: 依赖 ld -r 合并 16 个 tier 对象; ELF 下多个 weak 定义能正常合并,
# PE-COFF (MinGW) 下会被改名成 .weak.<sym>.<file> 而报 multiple definition,
# 因此该目标只在 Linux/WSL 工具链下可用。通配符必须限定在这四个 .c 上,
# 否则会把其它目标留在 bin/ 里的 *_asan.o (如 art_asan.o) 一起卷进来。
memops-asan: bin/memops_asan
bin/memops_asan: memops/main.cpp memops/memops_shim.c
	@mkdir -p bin
	for t in $(MEM_TIERS); do \
	  for f in memcpy memmove memset memcmp x86mem_init; do \
	    $(CC) $(MEM_CFLAGS) -fsanitize=address -fno-omit-frame-pointer \
	      $(MEM_FLAGS_$$t) -c $(MEM_DIR)/$$f.c -o bin/$$f\_$$t\_asan.o || exit 1; \
	  done; \
	done
	ld -r bin/memcpy_*_asan.o bin/memmove_*_asan.o bin/memset_*_asan.o \
	      bin/memcmp_*_asan.o bin/x86mem_init_*_asan.o -o bin/memops_asan_obj.o
	@rm -f bin/memcpy_*_asan.o bin/memmove_*_asan.o bin/memset_*_asan.o \
	      bin/memcmp_*_asan.o bin/x86mem_init_*_asan.o
	$(CC) $(CFLAGS) -fsanitize=address -c memops/memops_shim.c -o bin/memops_shim_asan.o
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address -fno-omit-frame-pointer \
	  $(MEM_INC) memops/main.cpp bin/memops_shim_asan.o bin/memops_asan_obj.o -o $@ $(LDLIBS)

.PHONY: all slub slub-poison fc fc-asan sched clean art-mini art-replay alloc fuzz \
        memops memops-bench memops-asan
all: slub slub-poison fc sched memops

slub: bin/slub_test
bin/slub_test: common/kernel_shim.cpp slub/main.cpp $(HEAP_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# 红区越界负向用例: heap.cpp 以 -DSLAB_DEBUG_POISON 重编
slub-poison: bin/slub_poison
bin/slub_poison: common/kernel_shim.cpp slub/poison_main.cpp $(HEAP_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -DSLAB_DEBUG_POISON $^ -o $@ $(LDLIBS)

# 多线程 per-CPU 并发 (真实自旋锁竞争 + 跨线程 free)
slub-mt: bin/slub_mt
bin/slub_mt: common/kernel_shim.cpp slub/mt_main.cpp $(HEAP_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# TSAN 变体: 数据竞争检测
slub-mt-tsan: bin/slub_mt_tsan
bin/slub_mt_tsan: common/kernel_shim.cpp slub/mt_main.cpp $(HEAP_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread $^ -o $@ $(LDLIBS)

# gcov 覆盖率构建 (跑完套件后: gcov -b ../kernel/src/mem/heap.cpp 等)
slub-cov: bin/slub_cov
bin/slub_cov: common/kernel_shim.cpp slub/main.cpp $(HEAP_SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -O0 --coverage $^ -o $@ $(LDLIBS)

fc-cov: bin/fc_cov
bin/fc_cov: common/kernel_shim.cpp fc/main.cpp $(HEAP_SRC) $(FC_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -O0 --coverage -c $(ART_SRC) -o bin/art_cov.o
	$(CXX) $(CXXFLAGS) -O0 --coverage -no-pie common/kernel_shim.cpp fc/main.cpp $(HEAP_SRC) $(FC_SRC) bin/art_cov.o -o $@ $(LDLIBS)

fc: bin/fc_test
bin/fc_test: common/kernel_shim.cpp fc/main.cpp $(HEAP_SRC) $(FC_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -c $(ART_SRC) -o bin/art.o
	$(CXX) $(CXXFLAGS) -no-pie common/kernel_shim.cpp fc/main.cpp $(HEAP_SRC) $(FC_SRC) bin/art.o -o $@ $(LDLIBS)

# ASAN 变体: 抓 UAF/越界
fc-asan: bin/fc_asan
bin/fc_asan: common/kernel_shim.cpp fc/main.cpp $(HEAP_SRC) $(FC_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -fsanitize=address -c $(ART_SRC) -o bin/art_asan.o
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=address -fno-omit-frame-pointer common/kernel_shim.cpp fc/main.cpp $(HEAP_SRC) $(FC_SRC) bin/art_asan.o -o $@ $(LDLIBS)

# 文件缓存多线程并发 (跨实例广播 + 真实自旋锁竞争)
fc-mt: bin/fc_mt
bin/fc_mt: common/kernel_shim.cpp fc/mt_main.cpp $(HEAP_SRC) $(FC_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -c $(ART_SRC) -o bin/art.o
	$(CXX) $(CXXFLAGS) -no-pie common/kernel_shim.cpp fc/mt_main.cpp $(HEAP_SRC) $(FC_SRC) bin/art.o -o $@ $(LDLIBS)

# 文件缓存 TSAN 变体
fc-mt-tsan: bin/fc_mt_tsan
bin/fc_mt_tsan: common/kernel_shim.cpp fc/mt_main.cpp $(HEAP_SRC) $(FC_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -O1 -g -fsanitize=thread -c $(ART_SRC) -o bin/art_tsan.o
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -no-pie common/kernel_shim.cpp fc/mt_main.cpp $(HEAP_SRC) $(FC_SRC) bin/art_tsan.o -o $@ $(LDLIBS)

# 调度器账本模型混沌仿真 (常量/公式对照 sched.cpp, 见 sched/main.cpp 声明)
sched: bin/sched_test
bin/sched_test: sched/main.cpp
	@mkdir -p bin
	$(CXX) -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter sched/main.cpp -o $@

# 三个已修缺陷的最小确定性回归
fc-reg: bin/fc_reg
bin/fc_reg: common/kernel_shim.cpp fc/regression.cpp $(HEAP_SRC) $(FC_SRC) $(ART_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -c $(ART_SRC) -o bin/art.o
	$(CXX) $(CXXFLAGS) -no-pie common/kernel_shim.cpp fc/regression.cpp $(HEAP_SRC) $(FC_SRC) bin/art.o -o $@ $(LDLIBS)

clean:
	rm -rf bin
