# tests/stdc/Makefile — 用户态 libc (lib/stdc) 宿主正确性测试
#
# 用法 (由 tests/Makefile 的 stdc 目标转发):
#   cd tests && make -B stdc && ./bin/stdc_test
#
# 编译策略:
#   * 用本仓库头文件 (-I lib/include/stdc ...) + 宿主 gcc 链接;
#   * 显式对象定义的符号 (memcpy/strcmp/setjmp/sin/fopen/...) 优先于 glibc
#     的同名符号, 所以测试跑的是 lib/stdc 的实现, 而不是宿主 libc;
#   * outfb/printf.c 依赖 flanterm/graphic, 不能宿主链接 —— 由
#     host_printf_shim.c 提供 vsnprintf_ (薄封装 glibc vsnprintf),
#     用于测试 stdio.c 的 fprintf/vfprintf 管线;
#   * time.c 通过 -DSKY_TIME_SOURCE=host_rtc_seconds /
#     -DSKY_CLOCK_SOURCE=host_monotonic_ms 注入宿主桩, 避免在宿主进程里
#     执行 Skyline 的系统调用号;
#   * sys_* 文件系统调用由 host_fs_shim.c 用内存文件模拟。

CC := gcc
ROOT := ../..
LIB := $(ROOT)/lib
ABLIB := $(ROOT)/ablib
OBJDIR := ../bin/stdc_obj
TARGET := ../bin/stdc_test

SKY_INC := -I$(LIB)/include/stdc -I$(ABLIB)/freestndchdrs/x86_64/include \
           -I$(LIB)/include -I$(ABLIB)
SKY_CFLAGS := -O2 -g -ffreestanding -fno-builtin -Wall -Wextra -Werror $(SKY_INC)
# 宿主 shim: 用系统头文件, 不能带本仓库 -I (否则 stdio.h 的宏重命名会生效)
SYS_CFLAGS := -O2 -g -Wall -Wextra -Werror -ffreestanding

LDLIBS := -lm

OBJS := $(OBJDIR)/memory.o \
        $(OBJDIR)/str.o \
        $(OBJDIR)/ctype.o \
        $(OBJDIR)/errno.o \
        $(OBJDIR)/math.o \
        $(OBJDIR)/math_extra.o \
        $(OBJDIR)/string_extra.o \
        $(OBJDIR)/locale.o \
        $(OBJDIR)/time.o \
        $(OBJDIR)/stdio.o \
        $(OBJDIR)/setjmp.o \
        $(OBJDIR)/test_stdc.o \
        $(OBJDIR)/host_fs_shim.o \
        $(OBJDIR)/host_printf_shim.o

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p ../bin
	$(CC) $(SKY_CFLAGS) -no-pie $(OBJS) -o $@ $(LDLIBS)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# 常规 lib/stdc 源
$(OBJDIR)/%.o: $(LIB)/stdc/%.c | $(OBJDIR)
	$(CC) $(SKY_CFLAGS) -c $< -o $@

# memory.c: 关掉 x86mem 分派 (宿主没有 _init_runtime_and_global_variables 装填
# 函数指针, 走内置 rep movs 实现)
$(OBJDIR)/memory.o: $(LIB)/stdc/memory.c | $(OBJDIR)
	$(CC) $(SKY_CFLAGS) -DNOT_COMPILE_X86MEM=1 -c $< -o $@

# time.c: 用宿主桩替换 syscall 时源
$(OBJDIR)/time.o: $(LIB)/stdc/time.c | $(OBJDIR)
	$(CC) $(SKY_CFLAGS) -DSKY_TIME_SOURCE=host_rtc_seconds \
	      -DSKY_CLOCK_SOURCE=host_monotonic_ms -c $< -o $@

# setjmp/longjmp 汇编实现 (宿主与目标同为 x86_64 SysV)
$(OBJDIR)/setjmp.o: $(LIB)/base/arch/x86_64/setjmp.S | $(OBJDIR)
	$(CC) $(SKY_CFLAGS) -c $< -o $@

$(OBJDIR)/test_stdc.o: test_stdc.c | $(OBJDIR)
	$(CC) $(SKY_CFLAGS) -c $< -o $@

$(OBJDIR)/host_fs_shim.o: host_fs_shim.c | $(OBJDIR)
	$(CC) $(SKY_CFLAGS) -c $< -o $@

$(OBJDIR)/host_printf_shim.o: host_printf_shim.c | $(OBJDIR)
	$(CC) $(SYS_CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
