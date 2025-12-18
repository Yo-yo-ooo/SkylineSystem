CC_x86mem = gcc
AR_x86mem = ar
CFLAGS_x86mem = -ffreestanding -march=x86-64 -mavx2 -O3 \
	-isystem ../kernel/freestnd-c-hdrs/x86_64/include \
	-nostdlib -nostdinc \
	-fpie -fomit-frame-pointer -fno-common -fno-zero-initialized-in-bss \
	-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-stack-protector -fno-stack-check -fno-strict-aliasing \
	-fno-merge-all-constants -fno-merge-constants -mno-stack-arg-probe \
	-m64 -mno-red-zone -maccumulate-outgoing-args --std=gnu17 -g3 \
	-Wno-error=deprecated -fmessage-length=0 -ffunction-sections -MMD -MP 

.PHONY: all
all: 
	$(CC_x86mem) $(CFLAGS_x86mem) -c memcpy.c -o memcpy.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memmove.c -o memmove.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memset.c -o memset.o
	$(CC_x86mem) $(CFLAGS_x86mem) -c memcmp.c -o memcmp.o
#$(AR_x86mem) rcs memops.a memcpy.o memmove.o memset.o memcmp.o

clean:
	rm -rf *.d *.o *.a