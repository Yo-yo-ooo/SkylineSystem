OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := DESKTOP
ELF_NAME  := desktop

include ../../gdef.mk


CFILES   := $(shell find . -type f -name '*.c'   | LC_ALL=C sort | sed 's|^\./||')
ASFILES  := $(shell find . -type f -name '*.S'   | LC_ALL=C sort | sed 's|^\./||')
CPPFILES := $(shell find . -type f -name '*.cpp' | LC_ALL=C sort | sed 's|^\./||')

LD  := ld
CC  := gcc
CXX := g++
AS  := as

# 静态链接 (现在只是备选: `make static`)
LDFLAGS := -entry=_start -static -nostdlib -m elf_x86_64 -z noexecstack 

CFLAGS := -nostdlib -nostdinc \
    -I../../ablib/freestndchdrs/$(BUILD_ARCH)/include \
    -I./include -I. -I../../lib/include/stdc -I../../lib/include -I../../lib/include/base/arch/$(BUILD_ARCH) \
    -ffreestanding -fshort-wchar -mno-red-zone \
    -fno-omit-frame-pointer -fno-exceptions \
    -ffunction-sections -fdata-sections -O2 -m64 -static -g3 \
    -Wall -Wextra   # P5-106: 与 lib 看齐


CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics 


OBJ := $(addprefix $(OBJDIR)/$(PROJ_NAME)/, \
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o) \
    $(CPPFILES:.cpp=.cpp.o) \
)

# ==============================================================
# 动态链接 (默认) —— 见 programs/dynprog.mk 与 docs/dynamic-linking-plan.md
# --------------------------------------------------------------
# `make` / `make -C programs/desktop` 产出 $(BINDIR)/desktop.elf, 它是**动态
# 链接**的 (NEEDED libsky.so, 无 PT_INTERP), 由内核在加载期把 libsky.so
# 映射进进程并完成重定位。
#
# 旧的静态版本仍然保留: `make static` 产出 $(BINDIR)/desktop.static.elf。
# 需要把它当作 /mp/desktop.elf 用时, 手工 e2cp 进镜像即可。
#
# 必须放在 OBJ 定义之后: dynprog.mk 里的 PY_OBJ 用 := 立即展开。
# ==============================================================
include ../dynprog.mk

.PHONY: all clean
all: $(DYN_ELF)


 $(OBJDIR)/$(PROJ_NAME)/%.c.o: %.c Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

 $(OBJDIR)/$(PROJ_NAME)/%.S.o: %.S Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


 $(OBJDIR)/$(PROJ_NAME)/%.s.o: %.s Makefile
	@echo "AS $<"
	@mkdir -p $(dir $@)
	$(AS) -o $@ $<


 $(OBJDIR)/$(PROJ_NAME)/%.cpp.o: %.cpp Makefile
	@echo "C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(BINDIR)
