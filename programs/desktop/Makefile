OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := DESKTOP

include ../../gdef.mk


CFILES   := $(shell find . -type f -name '*.c'   | LC_ALL=C sort | sed 's|^\./||')
ASFILES  := $(shell find . -type f -name '*.S'   | LC_ALL=C sort | sed 's|^\./||')
CPPFILES := $(shell find . -type f -name '*.cpp' | LC_ALL=C sort | sed 's|^\./||')

LD  := ld
CC  := gcc
CXX := g++
AS  := as

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

.PHONY: all clean dyn
all: $(BINDIR)/desktop.elf

# ==============================================================
# 动态链接版本 (共享库 libsky.so) —— docs/dynamic-linking-plan.md 阶段 4
# --------------------------------------------------------------
# 必须放在 OBJ 定义之后: DYN_OBJ 用 := 立即展开, 提前写会展开成空列表,
# 于是只有 init.S.o 被链进去, 报出误导性的 "undefined reference to `main'"
# (实测踩过这个坑)。
#
# 与静态版本的区别:
#   1. 不再 -static, 改为链 ../../lib/bin/libsky.so;
#   2. -pie: **必须**。否则链接器会为 .so 里的大对象 (例如 flanterm 的
#      全局状态) 生成 R_X86_64_COPY 重定位, 那要求加载器把数据从 .so 拷回
#      主程序, 并且 .so 内部对同一对象的引用还要指回主程序 —— 语义很绕。
#      做成 PIE (ET_DYN) 后所有引用都走 GOT/RELATIVE, 只剩
#      JUMP_SLOT / GLOB_DAT / RELATIVE 三类, 内核加载器已经全覆盖;
#   3. --no-dynamic-linker: 不生成 PT_INTERP。内核自己把 libsky.so 映射进
#      进程并完成重定位 (kernel/src/elf/elf_dyn.cpp), 不需要用户态 ld.so;
#   4. 额外编译一份 lib/base/arch/x86_64/init.S —— **入口点必须留在程序里**,
#      libsky.so 有意不含 init.S (否则 _start 会被带进共享库)。
# 默认目标 all 仍是静态版本, 保证现有链路不受影响; 动态版本用 `make dyn`。
# ==============================================================
DYN_LDFLAGS := -entry=_start -pie -m elf_x86_64 -z noexecstack --no-dynamic-linker
DYN_INIT_S  := ../../lib/base/arch/$(BUILD_ARCH)/init.S
DYN_OBJ     := $(OBJ) $(OBJDIR)/$(PROJ_NAME)/_dyn_init.S.o

# PIE 对象: 与静态对象分开一个目录, 避免互相覆盖。
# 加 -fPIE 的目的不是"要位置无关", 而是**消掉 R_X86_64_COPY**: 非 PIC 的
# 对象会让链接器为 .so 里的全局对象在主程序 BSS 里留一份副本, 那要求加载器
# 把数据拷过去 (内核侧已实现 COPY, 但"同一对象两份状态"的语义很别扭)。
# 用 -fPIE 编译后所有引用都走 GOT/PC 相对, 只剩 JUMP_SLOT/GLOB_DAT/RELATIVE。
PYOBJDIR := $(OBJDIR)/$(PROJ_NAME)-pie
PY_CFLAGS := $(filter-out -fno-pic -fno-pie -static,$(CFLAGS)) -fPIE
PY_OBJ := $(addprefix $(PYOBJDIR)/, \
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o) \
    $(CPPFILES:.cpp=.cpp.o) \
) $(PYOBJDIR)/_dyn_init.S.o

$(PYOBJDIR)/%.c.o: %.c Makefile
	@mkdir -p $(dir $@)
	@echo "CC(pie) $<"
	$(CC) $(PY_CFLAGS) -c $< -o $@

$(PYOBJDIR)/%.S.o: %.S Makefile
	@mkdir -p $(dir $@)
	@echo "CC(pie) $<"
	$(CC) $(PY_CFLAGS) -c $< -o $@

$(PYOBJDIR)/%.cpp.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	@echo "C++(pie) $<"
	$(CXX) $(PY_CFLAGS) -std=gnu++20 -fno-threadsafe-statics -c $< -o $@

$(PYOBJDIR)/_dyn_init.S.o: $(DYN_INIT_S)
	@mkdir -p $(dir $@)
	@echo "CC(pie) $<"
	$(CC) $(PY_CFLAGS) -c $< -o $@

$(BINDIR)/desktop.dyn.elf: $(PY_OBJ) Makefile ../../lib/bin/libsky.so
	@mkdir -p $(dir $@)
	@echo "LD(dyn) $@"
	$(LD) $(DYN_LDFLAGS) -o $@ $(PY_OBJ) --no-as-needed -L../../lib/bin -l:libsky.so

dyn: $(BINDIR)/desktop.dyn.elf
	@ls -l $(BINDIR)/desktop.elf $(BINDIR)/desktop.dyn.elf 2>/dev/null || true


 $(BINDIR)/desktop.elf: $(OBJ) Makefile
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ) -L../../lib/bin -l:lib.a 

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