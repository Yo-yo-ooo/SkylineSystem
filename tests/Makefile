# tests/Makefile — 内核组件宿主测试
CXX      := g++
CC       := gcc
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
            -pthread -D__KERNEL_TEST_HOST__ -I common -I ../kernel/include
CFLAGS   := -O2 -g -Wall -Wno-unused-parameter -Wno-unused-function -D__KERNEL_TEST_HOST__ \
            -I common -I ../kernel/include
LDLIBS   := -pthread

HEAP_SRC := ../kernel/src/mem/heap.cpp
ART_SRC  := ../kernel/src/klib/algorithm/art.c
FC_SRC   := ../kernel/src/fs/fc.cpp

.PHONY: all slub slub-poison fc fc-asan sched clean
all: slub slub-poison fc sched

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
