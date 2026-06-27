CC_bitop = gcc
AR_bitop = ar
include ../../../../gdef.mk

# 基础编译选项
CFLAGS_bitop_base = -ffreestanding -O3 \
    -isystem ../../../../kernel/freestnd-c-hdrs/x86_64/include \
    -nostdlib -nostdinc \
    -falign-functions=32 -falign-loops=32 -falign-jumps=32 \
    -fpie -fomit-frame-pointer -fno-common -fno-zero-initialized-in-bss \
    -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fno-stack-protector -fno-stack-check -fno-strict-aliasing \
    -fno-merge-all-constants -fno-merge-constants -mno-stack-arg-probe \
    -m64 -mno-red-zone -maccumulate-outgoing-args --std=gnu17 -g3 \
    -Wno-error=deprecated -fmessage-length=0 -ffunction-sections -MMD -MP

# 启用 BMI1 指令集的编译选项
CFLAGS_bitop_bmi = $(CFLAGS_bitop_base) -mbmi

SRCS = bitop.c

.PHONY: all clean

all: bitops.o

bitop_base.o: $(SRCS)
	@echo " -> Compiling bitop [base - no BMI]..."
	$(CC_bitop) $(CFLAGS_bitop_base) -c $< -o $@

bitop_bmi.o: $(SRCS)
	@echo " -> Compiling bitop [bmi - tzcnt/lzcnt]..."
	$(CC_bitop) $(CFLAGS_bitop_bmi) -c $< -o $@

bitops.o: bitop_base.o bitop_bmi.o 
	@echo " => Packing Fat Binary: bitops.o"
	ld -r bitop_base.o bitop_bmi.o -o $@
	@rm -f bitop_base.o bitop_bmi.o 

# 引入自动生成的依赖文件
-include $(wildcard *.d)

clean:
	rm -rf *.d *.o *.a