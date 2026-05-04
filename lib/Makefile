# ==============================================
# SkylineSystem 库 Makefile
# 输出规范：obj/ 放中间文件，bin/ 放最终库
# ==============================================
include ../gdef.mk
# 工具链定义
CC      = gcc
AR      = ar
RANLIB  = ranlib

# 目录定义
SRCDIRS = base init stdc stl
INCDIR  = include
OBJDIR  = obj
BINDIR  = bin

# 编译选项
CFLAGS  = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -nostdlib -nostdinc
CFLAGS += -Wall -Wextra -Werror -O2 -g -I./include/stdc -I../kernel/freestnd-c-hdrs/$(BUILD_ARCH)/include
CFLAGS += -I./$(INCDIR)



# 输出文件
TARGET  = $(BINDIR)/lib.a

# 自动递归查找源文件
C_SRCS  = $(shell find $(SRCDIRS) -name "*.c")
S_SRCS  = $(shell find $(SRCDIRS) -name "*.S")

# 生成对应的 .o 文件路径（统一放到 obj/ 下）
OBJS    = $(patsubst %.c, $(OBJDIR)/%.o, $(C_SRCS))
OBJS   += $(patsubst %.S, $(OBJDIR)/%.o, $(S_SRCS))

ifeq ($(BUILD_ARCH),x86_64)
CFLAGS += -DNOT_COMPILE_X86MEM=$(NOT_COMPILE_X86MEM)
ifeq ($(NOT_COMPILE_X86MEM),0)
CFLAGS += -I../x86mem
OBJS += ../x86mem/memcpy.o ../x86mem/memmove.o ../x86mem/memset.o ../x86mem/memcmp.o
endif
endif

# 生成依赖文件
DEPS    = $(OBJS:.o=.d)

# ==============================================
# 主规则
# ==============================================
all: $(OBJDIR) $(BINDIR) $(TARGET)

# 构建静态库
$(TARGET): $(OBJS)
	@echo "AR      $@"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@
	@echo "Library built: $@"

# 编译 C 文件
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC      $<"
	$(CC) $(CFLAGS) -MMD -c $< -o $@

# 编译汇编文件
$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "AS      $<"
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

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