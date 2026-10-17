# ==============================================
# SkylineSystem 库 Makefile
# 输出规范：obj/ 放中间文件，bin/ 放最终库
# This Makefile is a make rule for SkylineSystem User Library
# This Makefile Cannot use to link Kernel Object!
# ==============================================
include ../gdef.mk
# 工具链定义
CC      = gcc
CPP 	= g++
AR      = ar
AS 		= as
LD      = ld
RANLIB  = ranlib

# 目录定义
SRCDIRS = stdc base graphic
INCDIR  = include
OBJDIR  = obj
BINDIR  = bin

# 编译选项
CFLAGS  = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -nostdlib -nostdinc
CFLAGS += -Wall -Wextra -Werror -O2 -g -I./include/stdc -I../ablib/freestndchdrs/$(BUILD_ARCH)/include
CFLAGS += -I./$(INCDIR) -I../ablib



# 输出文件
TARGET  = $(BINDIR)/lib.a

# 自动递归查找源文件
# 注意: base/ 已经被 SRCDIRS 递归覆盖, 再单独加 base/arch/$(BUILD_ARCH)
# 会把同一份源收两次 —— 静态库用 ar 时只是静默覆盖, 但链接 .so 时会直接
# 报 "multiple definition of sys_gettid" (实测)。用 sort 去重。
C_SRCS  = $(shell find $(SRCDIRS) -name "*.c")
S_SRCS  = $(shell find $(SRCDIRS) -name "*.S")
CPP_SRCS  = $(shell find $(SRCDIRS) -name "*.cpp")

C_SRCS  += $(shell find base/arch/$(BUILD_ARCH) -name "*.c")
S_SRCS  += $(shell find base/arch/$(BUILD_ARCH) -name "*.S")
CPP_SRCS += $(shell find base/arch/$(BUILD_ARCH) -name "*.cpp")

C_SRCS   := $(sort $(C_SRCS))
S_SRCS   := $(sort $(S_SRCS))
CPP_SRCS := $(sort $(CPP_SRCS))

OBJS    = $(patsubst %.c, $(OBJDIR)/%.c.o, $(C_SRCS))
OBJS   += $(patsubst %.S, $(OBJDIR)/%.S.o, $(S_SRCS))
OBJS   += $(patsubst %.cpp, $(OBJDIR)/%.cpp.o, $(CPP_SRCS))

ifeq ($(BUILD_ARCH),x86_64)
# lib **总是** 链接 x86mem/memops.o, 所以宏值必须确定为 0 —— 不能跟
# gdef.mk 的宿主机探测结果走 (那个值在子 make/不同调用方式下会翻成 1,
# 导致 x86mem.h 找不到、随后 -Werror 中断; 而且会和下面的显式 -D 冲突)。
NOT_COMPILE_X86MEM := 0
CFLAGS += -DNOT_COMPILE_X86MEM=0
X86MEM_CFLAGS += -I../ablib/arch/x86_64/x86mem
X86MEM_OBJ    += ../ablib/arch/x86_64/x86mem/memops.o
# 共享库需要 PIC 版 memops: 静态版是 -fpie 编的, 其 R_X86_64_PC32 重定位
# 在 .so 里非法。构建 .so 前先 make -C x86mem pic。
X86MEM_PIC_OBJ += ../ablib/arch/x86_64/x86mem/memops.pic.o
CFLAGS += $(X86MEM_CFLAGS)
OBJS   += $(X86MEM_OBJ)
endif

# 生成依赖文件
DEPS    = $(OBJS:.o=.d)

# ==============================================
# 共享库 (动态链接) —— 阶段 1
# ----------------------------------------------
# 目标: 产出一份 libsky.so, 让用户态程序不再各自静态链一份 libc。
# 要点:
#   * 全部用 -fPIC 重新编译 (独立 objdir obj-so/, 不污染静态库的 obj/);
#   * 链接成 -shared, 带 SONAME;
#   * **不包含 _start**: 入口点必须留在程序里 (init.S 定义 _start, 且它会
#     调用 _init_runtime_and_global_variables —— 程序自有副本, .so 只提供
#     可被重定位的函数). 内核侧的加载器见 docs/dynamic-linking-plan.md。
# ==============================================
SOBJDIR = obj-so
SOBJDIR_NAME = $(SOBJDIR)
SOTARGET = $(BINDIR)/libsky.so

# 与静态库相同的源, 但排除提供入口点的 init.S
SO_C_SRCS   = $(C_SRCS)
SO_CPP_SRCS = $(CPP_SRCS)
SO_S_SRCS   = $(filter-out %/init.S, $(S_SRCS))

SOBJS  = $(patsubst %.c, $(SOBJDIR)/%.c.o, $(SO_C_SRCS))
SOBJS += $(patsubst %.S, $(SOBJDIR)/%.S.o, $(SO_S_SRCS))
SOBJS += $(patsubst %.cpp, $(SOBJDIR)/%.cpp.o, $(SO_CPP_SRCS))
# 用 PIC 版 memops (不是静态库用的那份 -fpie 版本)
SOBJS += $(X86MEM_PIC_OBJ)

# TLS 模型 (实测结论, 见 /tmp/tls_*.o 的对照实验):
#   -fPIC + -ftls-model=initial-exec          -> R_X86_64_GOTTPOFF  (.so 合法)
#   -fPIC + -fPIE + -ftls-model=initial-exec  -> R_X86_64_TPOFF32  (.so 非法!)
# 关键点: **-fPIE 会覆盖 -ftls-model**, 把静态 __thread 退化成 local-exec。
# 因此共享库的编译里绝不能带 -fPIE (主程序本来就是固定的 EXEC, 也不需要它)。
SO_CFLAGS = $(CFLAGS) -fPIC -ftls-model=initial-exec
# -z noexecstack: 明确的栈不可执行; --hash-style=sysv: 减少对 GNU hash 的依赖
SO_LDFLAGS = -shared -m elf_x86_64 -z noexecstack --hash-style=sysv \
             -soname libsky.so

SO_DEPS = $(SOBJS:.o=.d)

.PHONY: so
so: $(BINDIR) memops-pic $(SOTARGET)

# PIC 版 memops 由 ablib 的 x86mem 构建产出
.PHONY: memops-pic
memops-pic:
	@$(MAKE) -s -C ../ablib/arch/x86_64/x86mem pic >/dev/null

# 依赖里带上 memops-pic: 直接 `make all` 时也要先把 PIC 版 memops 编出来
$(SOTARGET): $(SOBJS) Makefile memops-pic
	@echo "LD      $@"
	@$(LD) $(SO_LDFLAGS) -o $@ $(SOBJS)
	@echo "Shared library built: $@"

$(SOBJDIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "AS(so)  $<"
	@$(CC) $(SO_CFLAGS) -MMD -c $< -o $@

$(SOBJDIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC(so)  $<"
	@$(CC) $(SO_CFLAGS) -MMD -c $< -o $@

$(SOBJDIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "C++(so) $<"
	@$(CPP) $(SO_CFLAGS) -MMD -c $< -o $@

-include $(SO_DEPS)

# ==============================================
# 主规则
# ==============================================
all: $(OBJDIR) $(BINDIR) $(TARGET) $(SOTARGET)
$(info $(OBJS))
# 构建静态库
$(TARGET): $(OBJS)
	@echo "AR      $@"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@
	@echo "Library built: $@"

# 编译汇编文件
$(OBJDIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "AS      $<"
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

# 编译 C 文件
$(OBJDIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC      $<"
	$(CC) $(CFLAGS) -MMD -c $< -o $@

# 编译 C++ 文件
$(OBJDIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "C++      $<"
	$(CPP) $(CFLAGS) -MMD -c $< -o $@

# 创建目录
$(OBJDIR):
	@mkdir -p $(OBJDIR)
$(BINDIR):
	@mkdir -p $(BINDIR)

# 包含依赖文件
-include $(DEPS)

# ==============================================
# 清理规则
# ==============================================
clean:
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "Library all cleaned."

.PHONY: all clean