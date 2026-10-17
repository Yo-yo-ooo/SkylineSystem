# ==============================================================================
# dynprog.mk —— 通用"动态链接版用户态程序"规则
# ------------------------------------------------------------------------------
# 用法: 在程序自己的 Makefile 里, 于 CFLAGS / 源文件列表 / OBJ 定义**之后**加:
#
#     ELF_NAME := desktop           # 产出的 $(BINDIR)/desktop.elf
#     include ../dynprog.mk
#     all: $(DYN_ELF)               # 默认目标指向动态版本
#
# 这样 `make -C programs/xxx` (以及 `make cm`) 的默认产物就是动态链接版;
# 需要旧的静态版本时用 `make static`。
#
# 为什么入口点要单独准备一份 init.S:
#   libsky.so **有意不含** lib/base/arch/x86_64/init.S —— 那个文件定义 _start,
#   属于"可执行程序"而非"库"。所以动态链接的程序必须自己带一份,
#   否则链出来没有入口 (报 undefined reference to `main' 其实是因为
#   整个 OBJ 列表为空, 只剩一份 init.S)。
#
# 为什么必须 -pie:
#   非 PIC 的对象会让 ld 为 .so 里的大对象 (例如 flanterm 的全局状态)
#   生成 R_X86_64_COPY 重定位 —— 要求加载器把数据拷回主程序, 并且 .so 内部
#   对同一对象的引用还要指回主程序, 语义很绕。做成 PIE (ET_DYN) 后所有引用
#   都走 GOT/RELATIVE, 只剩 JUMP_SLOT / GLOB_DAT / RELATIVE 三类,
#   内核加载器 (kernel/src/elf/elf_dyn.cpp) 已全覆盖。
#
# 为什么 --no-dynamic-linker:
#   不生成 PT_INTERP。内核自己把 libsky.so 映射进进程并完成重定位,
#   不需要用户态 ld.so (本系统没有)。
# ==============================================================================

DYNLIB      := ../../lib/bin/libsky.so
DYN_LDFLAGS := -entry=_start -pie -m elf_x86_64 -z noexecstack --no-dynamic-linker
DYN_INIT_S  := ../../lib/base/arch/$(BUILD_ARCH)/init.S

# 默认目标必须显式钉成 all: 本文件里 `$(PYOBJDIR)/_dyn_init.S.o` 是第一个
# **显式**目标规则 (前面几条都是模式规则, 不会成为默认目标), make 会把它
# 当作默认目标 —— 于是 `make` 只编译一份 init.S 就"成功"退出, 既不链接也不
# 报错。这个坑和 lib/Makefile 里 `so:` 抢走默认目标是同一类。
.DEFAULT_GOAL := all

DYN_ELF     := $(BINDIR)/$(ELF_NAME).elf
STATIC_ELF  := $(BINDIR)/$(ELF_NAME).static.elf

# PIE 对象放独立目录, 与静态对象互不覆盖
PYOBJDIR    := $(OBJDIR)/$(PROJ_NAME)-pie
PY_CFLAGS   := $(filter-out -fno-pic -fno-pie -static,$(CFLAGS)) -fPIE
PY_CXXFLAGS := $(filter-out -fno-pic -fno-pie -static,$(CXXFLAGS)) -fPIE
PY_OBJ      := $(addprefix $(PYOBJDIR)/, \
                   $(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o)) \
               $(PYOBJDIR)/_dyn_init.S.o

$(PYOBJDIR)/%.c.o: %.c Makefile ../dynprog.mk
	@mkdir -p $(dir $@)
	@echo "CC(dyn)  $<"
	$(CC) $(PY_CFLAGS) -c $< -o $@

$(PYOBJDIR)/%.S.o: %.S Makefile ../dynprog.mk
	@mkdir -p $(dir $@)
	@echo "CC(dyn)  $<"
	$(CC) $(PY_CFLAGS) -c $< -o $@

$(PYOBJDIR)/%.cpp.o: %.cpp Makefile ../dynprog.mk
	@mkdir -p $(dir $@)
	@echo "C++(dyn) $<"
	$(CXX) $(PY_CXXFLAGS) -c $< -o $@

$(PYOBJDIR)/_dyn_init.S.o: $(DYN_INIT_S)
	@mkdir -p $(dir $@)
	@echo "CC(dyn)  $<"
	$(CC) $(PY_CFLAGS) -c $< -o $@

# 目标文件必须排在 -l 之前, 否则会被 --as-needed 丢掉 (表现为莫名其妙的
# "undefined reference to `main'")。libsky.so 缺失时给出可操作的提示,
# 而不是让 ld 抛 "cannot find -l:libsky.so"。
$(DYN_ELF): $(PY_OBJ) Makefile ../dynprog.mk
	@mkdir -p $(dir $@)
	@if [ ! -f $(DYNLIB) ]; then \
	    echo "ERROR: $(DYNLIB) 不存在 —— 先执行 'make -C lib' (或直接 'make cm')"; \
	    exit 1; \
	fi
	@echo "LD(dyn)  $@"
	$(LD) $(DYN_LDFLAGS) -o $@ $(PY_OBJ) --no-as-needed -L../../lib/bin -l:libsky.so

.PHONY: static
static: $(STATIC_ELF)

$(STATIC_ELF): $(OBJ) Makefile
	@mkdir -p $(dir $@)
	@echo "LD(static) $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJ) -L../../lib/bin -l:lib.a
