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
