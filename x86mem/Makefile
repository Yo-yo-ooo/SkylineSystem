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

ifneq ($(HAS_AVX),0)
ifeq ($(COMPILER_SUPPORT_AVX),yes)
$(info CFLAGS_x86mem += -mavx)
CFLAGS_x86mem += -mavx
endif
endif

ifneq ($(HAS_AVX2),0)
ifeq ($(COMPILER_SUPPORT_AVX2),yes)
$(info CFLAGS_x86mem += -mavx2)
CFLAGS_x86mem += -mavx2
endif
endif

ifneq ($(HAS_AVX_512),0)
ifeq ($(COMPILER_SUPPORT_AVX512),yes)
$(info CFLAGS_x86mem += -mavx512f)
CFLAGS_x86mem += -mavx512f
endif
endif

.PHONY: all
all: 
	$(CC_x86mem) $(CFLAGS_x86mem) -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memcmp.c -o memcmp.o

clean:
	rm -rf *.d *.o *.a