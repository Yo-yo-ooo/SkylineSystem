CC_x86mem = gcc
AR_x86mem = ar
include ../../../../gdef.mk

CFLAGS_x86mem = -ffreestanding -msse4.2 -O3 \
    -isystem ../../../../ablib/freestndchdrs/x86_64/include \
    -nostdlib -nostdinc \
	-falign-functions=32 -falign-loops=32 -falign-jumps=32 \
    -fpie -fomit-frame-pointer -fno-common -fno-zero-initialized-in-bss \
    -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fno-stack-protector -fno-stack-check -fno-strict-aliasing \
    -fno-merge-all-constants -fno-merge-constants -mno-stack-arg-probe \
    -m64 -mno-red-zone -maccumulate-outgoing-args --std=gnu17 -g3 \
    -Wno-error=deprecated -fmessage-length=0 -ffunction-sections -MMD -MP 

# 定义针对不同指令集层级的特定编译器参数和宏
FLAGS_base   := -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512
FLAGS_avx    := -mavx -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512
FLAGS_avx2   := -mavx -mavx2 -DX86MEM_NOT_COMPILE_AVX512
FLAGS_avx512 := -mavx -mavx2 -mavx512f -mprefer-vector-width=512

# 1. 动态评估当前环境支持的指令集，并累加到目标架构列表中
# 基础版本 (SSE4.2) 必选
TARGET_ARCHS := base

ifneq ($(HAS_AVX),0)
ifeq ($(COMPILER_SUPPORT_AVX),yes)
    TARGET_ARCHS += avx
endif
endif

ifneq ($(HAS_AVX2),0)
ifeq ($(COMPILER_SUPPORT_AVX2),yes)
    TARGET_ARCHS += avx2
endif
endif

ifneq ($(HAS_AVX_512),0)
ifeq ($(COMPILER_SUPPORT_AVX512),yes)
    TARGET_ARCHS += avx512
endif
endif

SRCS = memcpy.c memmove.c memset.c memcmp.c
OBJS_TO_LINK = $(foreach arch,$(TARGET_ARCHS),memops_$(arch).o)

.PHONY: all clean

all: memops.o

#  核心魔法：使用模板函数动态生成各指令集版本的编译规则
define COMPILE_TEMPLATE
memops_$(1).o: $(SRCS)
	@echo " -> Compiling x86mem [$(1)] tier..."
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_$(1)) -c memcpy.c -o memcpy_$(1).o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_$(1)) -c memmove.c -o memmove_$(1).o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_$(1)) -c memset.c -o memset_$(1).o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_$(1)) -c memcmp.c -o memcmp_$(1).o
	ld -r memcpy_$(1).o memmove_$(1).o memset_$(1).o memcmp_$(1).o -o $$@
	@rm -f memcpy_$(1).o memmove_$(1).o memset_$(1).o memcmp_$(1).o
endef

# 动态展开生成 base, avx, avx2, avx512 (根据系统支持度) 的具体规则
$(foreach arch,$(TARGET_ARCHS),$(eval $(call COMPILE_TEMPLATE,$(arch))))

# 3. 最终融合：将生成的多个对象文件链接成一个胖二进制文件
memops.o: $(OBJS_TO_LINK)
	@echo " => Packing Fat Binary: memops.o"
	ld -r $(OBJS_TO_LINK) -o $@
	@rm -f $(OBJS_TO_LINK)

# 引入自动生成的依赖文件
-include $(wildcard *.d)

clean:
	rm -rf *.d *.o *.a