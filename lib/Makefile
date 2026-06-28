# ==============================================
# SkylineSystem 库 Makefile
# 输出规范：obj/ 放中间文件，bin/ 放最终库
# This Makefile is a make rule for SkylineSystem User Library
# This Makefile Cannot use to link Kernel Object!
# ==============================================
include ../gdef.mk
# 工具链定义
CC      = gcc
AR      = ar
AS 		= as
RANLIB  = ranlib

# 目录定义
SRCDIRS = stdc
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
C_SRCS  = $(shell find $(SRCDIRS) -name "*.c")
S_SRCS  = $(shell find $(SRCDIRS) -name "*.S")

C_SRCS += $(shell find base/arch/$(BUILD_ARCH) -name "*.c")
S_SRCS += $(shell find base/arch/$(BUILD_ARCH) -name "*.S")


OBJS    = $(patsubst %.c, $(OBJDIR)/%.c.o, $(C_SRCS))
OBJS   += $(patsubst %.S, $(OBJDIR)/%.S.o, $(S_SRCS))

ifeq ($(BUILD_ARCH),x86_64)
CFLAGS += -DNOT_COMPILE_X86MEM=$(NOT_COMPILE_X86MEM)
ifeq ($(NOT_COMPILE_X86MEM),0)
CFLAGS += -I../ablib/arch/x86_64/x86mem
OBJS += ../ablib/arch/x86_64/x86mem/memops.o
endif
endif

# 生成依赖文件
DEPS    = $(OBJS:.o=.d)

# ==============================================
# 主规则
# ==============================================
all: $(OBJDIR) $(BINDIR) $(TARGET)
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
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "Library all cleaned."

.PHONY: all clean