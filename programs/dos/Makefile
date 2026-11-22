# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT

# ==============================================================================
# programs/dos/Makefile —— DOS 80x25 文本屏窗口客户端（动态 WM client）
# ------------------------------------------------------------------------------
# 与 programs/settings/Makefile 同构：默认产物是动态链接的 $(BINDIR)/dos.elf
# （PIE + libsky.so，内核加载期兜底映射 0x400000 WM 协议页）；规则见
# ../dynprog.mk。`make -C programs/dos` 即可独立构建。
# ==============================================================================
OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := DOS
ELF_NAME  := dos

include ../../gdef.mk


CFILES   := $(shell find . -type f -name '*.c'   | LC_ALL=C sort | sed 's|^\./||')
ASFILES  := $(shell find . -type f -name '*.S'   | LC_ALL=C sort | sed 's|^\./||')
CPPFILES := $(shell find . -type f -name '*.cpp' | LC_ALL=C sort | sed 's|^\./||')

LD  := ld
CC  := gcc
CXX := g++
AS  := as

LDFLAGS := -entry=_start -static -nostdlib -m elf_x86_64 -z noexecstack -T ./link/console_x86_64.ld

CFLAGS := -nostdlib -nostdinc \
    -I../../ablib/freestndchdrs/$(BUILD_ARCH)/include \
    -I./include -I. -I../../lib/include/stdc -I../../lib/include -I../../lib/include/base/arch/$(BUILD_ARCH) \
    -ffreestanding -fshort-wchar -mno-red-zone \
    -fno-omit-frame-pointer -fno-exceptions \
    -ffunction-sections -fdata-sections -O2 -m64 -static -g3


CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics


OBJ := $(addprefix $(OBJDIR)/$(PROJ_NAME)/, \
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o) \
    $(CPPFILES:.cpp=.cpp.o) \
)

.PHONY: all clean com
# 动态链接版 (PIE + libsky.so)。内核在加载期兜底映射 0x400000 的 WM 协议页。
include ../dynprog.mk

# ---- 16-bit DOS payloads -----------------------------------------------------
# 同目录的 .asm 用 nasm -f bin 汇成 flat binary，直通 DOSVM 的 16 位装载区。
# .com 是构建产物（见仓库根 .gitignore），仓库里只留 .asm 源。
# trampoline.nasm 不走这条规则：它是 long->compat->protected16->real 的切换
# blob，由 res/scripts/gen_dosvm_trampoline.py 生成 dosvm_trampoline.h。
COM_ASMS := hello.asm filedemo.asm
COM_OUT  := $(addprefix $(BINDIR)/,$(COM_ASMS:.asm=.com))
NASM     ?= nasm

$(BINDIR)/%.com: %.asm Makefile
	@echo "NASM $<"
	@mkdir -p $(BINDIR)
	@command -v $(NASM) >/dev/null 2>&1 || { \
	    echo "ERROR: 找不到 $(NASM) —— 16 位 .COM 需要 NASM"; \
	    echo "       Debian/Ubuntu: sudo apt install nasm"; \
	    exit 1; \
	}
	$(NASM) -f bin -Wall -Werror -o $@ $<

com: $(COM_OUT)

all: $(DYN_ELF) $(COM_OUT)

 $(OBJDIR)/$(PROJ_NAME)/%.c.o: %.c Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

 $(OBJDIR)/$(PROJ_NAME)/%.S.o: %.S Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

 $(OBJDIR)/$(PROJ_NAME)/%.cpp.o: %.cpp Makefile
	@echo "C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 只清理本程序自己的产物，避免与并行构建的其它 program 互删共享目录
clean:
	rm -rf $(OBJDIR)/$(PROJ_NAME) $(OBJDIR)/$(PROJ_NAME)-pie
	rm -f $(BINDIR)/$(ELF_NAME).elf $(BINDIR)/$(ELF_NAME).static.elf
	rm -f $(COM_OUT)
