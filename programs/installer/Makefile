OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := INSTALLER
ELF_NAME  := installer

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
    -I./include -I. -I../../lib/include/stdc -I../../lib/include \
    -I../../lib/include/base/arch/$(BUILD_ARCH) \
    -ffreestanding -fshort-wchar -mno-red-zone \
    -fno-omit-frame-pointer -fno-exceptions \
    -ffunction-sections -fdata-sections -O2 -m64 -static -g3

CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics

OBJ := $(addprefix $(OBJDIR)/$(PROJ_NAME)/, \
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o) \
    $(CPPFILES:.cpp=.cpp.o) \
)

.PHONY: all clean

# 动态链接版 (PIE + libsky.so)：WM 客户端统一入口，内核在加载期映射 0x400000
# 的窗口协议页（content VA/几何/CLIENT_READY 握手）；见 docs/dynamic-clients-plan.md。
include ../dynprog.mk

all: $(DYN_ELF)

$(OBJDIR)/$(PROJ_NAME)/%.c.o: %.c Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/$(PROJ_NAME)/%.S.o: %.S Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/$(PROJ_NAME)/%.cpp.o: %.cpp Makefile
	@echo "C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 只清理本程序的 PIE 对象与产物，绝不删共享的 ../object 或 ../bin（其他程序在用）。
clean:
	rm -rf $(PYOBJDIR) $(DYN_ELF) $(STATIC_ELF)
