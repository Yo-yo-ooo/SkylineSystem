# ==============================================================================
# programs/settings/Makefile —— Settings（主题/桌面设置 GUI，动态 WM client）
# ------------------------------------------------------------------------------
# 与 programs/notepad/Makefile 同构：默认产物是动态链接的 $(BINDIR)/settings.elf
# （PIE + libsky.so，内核加载期兜底映射 0x400000 WM 协议页）；规则见
# ../dynprog.mk。`make -C programs/settings` 即可独立构建。
# ==============================================================================
OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := SETTINGS
ELF_NAME  := settings

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

.PHONY: all clean
# 动态链接版 (PIE + libsky.so)。内核在加载期兜底映射 0x400000 的 WM 协议页。
include ../dynprog.mk

all: $(DYN_ELF)

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
