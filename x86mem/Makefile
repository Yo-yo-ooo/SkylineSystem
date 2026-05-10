CC_x86mem = gcc
AR_x86mem = ar
include ../gdef.mk
CFLAGS_x86mem = -ffreestanding -msse4.2 -O3 \
	-isystem ../kernel/freestnd-c-hdrs/x86_64/include \
	-nostdlib -nostdinc \
	-fpie -fomit-frame-pointer -fno-common -fno-zero-initialized-in-bss \
	-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-stack-protector -fno-stack-check -fno-strict-aliasing \
	-fno-merge-all-constants -fno-merge-constants -mno-stack-arg-probe \
	-m64 -mno-red-zone -maccumulate-outgoing-args --std=gnu17 -g3 \
	-Wno-error=deprecated -fmessage-length=0 -ffunction-sections -MMD -MP 

FLAGS_FOR_AVX512 := -mavx512f -mprefer-vector-width=512
FLAGS_FOR_AVX2 := -mavx2
FLAGS_FOR_AVX := -mavx

.PHONY: all
all: 
ifneq ($(HAS_AVX_512),0)
ifeq ($(COMPILER_SUPPORT_AVX512),yes)
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) $(FLAGS_FOR_AVX512) -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) $(FLAGS_FOR_AVX512) -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) $(FLAGS_FOR_AVX512) -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) $(FLAGS_FOR_AVX512) -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_avx512.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_avx2.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_avx.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	$(CC_x86mem) $(CFLAGS_x86mem) -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_base.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	ld -r memops_base.o memops_avx.o memops_avx2.o memops_avx512.o -o memops.o
	rm -rf memops_base.o memops_avx.o memops_avx2.o memops_avx512.o
	@true
endif
endif

ifneq ($(HAS_AVX2),0)
ifeq ($(COMPILER_SUPPORT_AVX2),yes)
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -DX86MEM_NOT_COMPILE_AVX512 -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -DX86MEM_NOT_COMPILE_AVX512 -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -DX86MEM_NOT_COMPILE_AVX512 -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) $(FLAGS_FOR_AVX2) -DX86MEM_NOT_COMPILE_AVX512 -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_avx2.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX512 -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX512 -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX512 -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX512 -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_avx.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX512 -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX512 -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX512 -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX512 -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_base.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	ld -r memops_base.o memops_avx.o memops_avx2.o -o memops.o
	rm -rf memops_base.o memops_avx.o memops_avx2.o 
	@true
endif
endif

ifneq ($(HAS_AVX),0)
ifeq ($(COMPILER_SUPPORT_AVX),yes)
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) $(FLAGS_FOR_AVX) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_avx.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_base.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	ld -r memops_base.o memops_avx.o -o memops.o
	rm -rf memops_base.o memops_avx.o  
	@true
endif
endif
# Compile The Base Version of x86mem lib
# Min Request:SSE4.2!
ifneq ($(HAS_SSE_4_2),0)
ifeq ($(COMPILER_SUPPORT_SSE_4_2),yes)
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) -DX86MEM_NOT_COMPILE_AVX -DX86MEM_NOT_COMPILE_AVX2 -DX86MEM_NOT_COMPILE_AVX512 -c memcmp.c -o memcmp.o
	ld -r memcpy.o memmove.o memset.o memcmp.o -o memops_base.o
	rm -rf memcpy.o memmove.o memset.o memcmp.o *.d *.a
	ld -r memops_base.o -o memops.o
	rm -rf memops_base.o 
	@true
endif
endif

clean:
	rm -rf *.d *.o *.a