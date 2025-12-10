
.PHONY: all
all: 
	gcc -ffreestanding -march=x86-64 -mavx2 -O3 \
	-isystem ../kernel/freestnd-c-hdrs/x86_64/include \
	-nostdlib -nostdinc \
	-fpie -fomit-frame-pointer -fno-common -fno-zero-initialized-in-bss \
	-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-stack-protector -fno-stack-check -fno-strict-aliasing \
	-fno-merge-all-constants -fno-merge-constants -mno-stack-arg-probe \
	-m64 -mno-red-zone -maccumulate-outgoing-args --std=gnu17 -g3 \
	-Wno-error=deprecated -fmessage-length=0 -ffunction-sections -MMD -MP \
	memcmp.c memcpy.c memmove.c memset.c -o memops.o

	ar rcs memops.a memops.o

clean:
	rm -rf memops.d memops.o memops.a