#SPDX-FileCopyrightText: 2026 Yo-yo-ooo
#SPDX-License-Identifier: MIT
#=============================================================
# tests/stdcpp/Makefile — stdcpp 独立测试工程
#
# 刻意与主测试体系解耦：
#   * 不被 tests/Makefile 的 all / ci.sh 引用
#   * 不被 .github/workflows/ci.yml 调用
#   * 只在本目录显式执行 `make` 时才构建
# 这样 STL 的语义测试（含故意触发的 OOM/异常路径）不会拖慢或影响
# 内核回归门禁，符合“独立测试”的定位。
#
# 用法：
#   make              构建并运行（gnu++17，内核基线）
#   make STD=gnu++20  以用户态标准（gnu++20）复跑
#   make asan         构建 ASAN+UBSAN 版本并运行
#   make clean        清理
#=============================================================
CXX      ?= g++
STD      ?= gnu++17

SRCDIR   := .
INCDIR   := ../../lib/include/stdcpp
OBJDIR   := ../bin/stdcpp_obj
BINDIR   := ../bin
TARGET   := $(BINDIR)/stdcpp_test

SRCS := $(wildcard framework/*.cpp) $(wildcard t_*.cpp) main.cpp
OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SRCS))

CPPFLAGS := -I $(INCDIR) -I framework -DSTDCPP_DEBUG
CXXFLAGS := -std=$(STD) -O1 -g -Wall -Wextra -Wno-unused-parameter \
            -fno-omit-frame-pointer -pthread
LDFLAGS  :=

.PHONY: all run clean asan cxx20 clean-for-std

all: run

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)

run: $(TARGET)
	@echo "== stdcpp tests ($(STD)) =="
	@./$(TARGET)

# 用户态标准（programs/*/Makefile 使用 gnu++20）
cxx20:
	@$(MAKE) --no-print-directory clean-for-std
	@$(MAKE) --no-print-directory STD=gnu++20 run

# ASAN/UBSAN 版：抓 UAF / 越界 / 未定义行为
asan:
	@mkdir -p $(BINDIR)
	$(CXX) $(CPPFLAGS) -std=$(STD) -O1 -g -fsanitize=address,undefined \
	  -fno-omit-frame-pointer $(SRCS) -o $(BINDIR)/stdcpp_test_asan
	@echo "== stdcpp tests (ASAN+UBSAN) =="
	@ASAN_OPTIONS=detect_leaks=0 ./$(BINDIR)/stdcpp_test_asan

clean:
	rm -rf $(OBJDIR) $(TARGET) $(BINDIR)/stdcpp_test_asan

clean-for-std:
	rm -rf $(OBJDIR) $(TARGET)
