include ../gdef.mk

LIB_BUILD_ARCH = $(BUILD_ARCH)

CFILES := $(shell cd src && find . -not -path "./arch/*" -name "*.c" | LC_ALL=C sort)
ASFILES := $(shell cd src && find . -not -path "./arch/*" -name "*.S" | LC_ALL=C sort)
CPPFILES := $(shell cd src && find . -not -path "./arch/*" -name "*.cpp" | LC_ALL=C sort)
CFILES += $(shell cd src && find arch/$(LIB_BUILD_ARCH) -type f -name '*.c' | LC_ALL=C sort)
ASFILES += $(shell cd src && find arch/$(LIB_BUILD_ARCH) -type f -name '*.S' | LC_ALL=C sort)
CPPFILES += $(shell cd src && find arch/$(LIB_BUILD_ARCH) -type f -name '*.cpp' | LC_ALL=C sort)

override OBJ := $(addprefix obj-$(LIB_BUILD_ARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o))
override HEADER_DEPS := $(addprefix obj-$(LIB_BUILD_ARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))
OBJDIR = obj-$(LIB_BUILD_ARCH)
BINDIR = bin-$(LIB_BUILD_ARCH)

CC = gcc
CXX = g++
LD = ld
AS = as
AR = ar

CFLAGS = -ffreestanding -fshort-wchar -mno-red-zone -fno-omit-frame-pointer \
	-fno-exceptions -ffunction-sections -fdata-sections \
	-I . -I./include -I./include/arch/$(LIB_BUILD_ARCH)  -m64 -nostdinc -nostdlib
CPPFLAGS = -ffreestanding -fshort-wchar -mno-red-zone -fno-omit-frame-pointer -I./include/arch/$(LIB_BUILD_ARCH) -I./include \
	-fno-exceptions -nostdinc\
	-fpermissive -Wno-pmf-conversions -ffunction-sections -fdata-sections -I .  -m64 -std=gnu++20 -nostdlib
LDFLAGS = --gc-sections  -static -nostdlib -m elf_x86_64 -unresolved-symbols=ignore-all


.PHONY: all
all: bin-$(LIB_BUILD_ARCH)/hllib.a

# Link rules for the final executable.
bin-$(LIB_BUILD_ARCH)/hllib.a: GNUmakefile $(OBJ)
	@mkdir -p "$$(dirname $@)"
	$(AR) rcs bin-$(LIB_BUILD_ARCH)/hllib.a $(OBJ)

obj-$(LIB_BUILD_ARCH)/%.c.o: src/$(CFILES) GNUmakefile
	@echo C $<
	@mkdir -p "$$(dirname $@)"
	@$(CC) $(CFLAGS)  -c $< -o $@

obj-$(LIB_BUILD_ARCH)/%.S.o: src/$(ASFILES) GNUmakefile
	@echo GAS $<
	@mkdir -p "$$(dirname $@)"
	@$(AS) $(CFLAGS)  -c $< -o $@
	
obj-$(LIB_BUILD_ARCH)/%.cpp.o: src/$(CPPFILES) GNUmakefile
	@echo C++ $<
	@mkdir -p "$$(dirname $@)"
	@$(CXX) $(CPPFLAGS)  -c $< -o $@


clean:
	rm -rf obj-$(LIB_BUILD_ARCH)
	rm -rf bin-$(LIB_BUILD_ARCH)