OBJDIR = ../object
BINDIR = ../bin

include ../../gdef.mk

CFILES += $(shell  find -L * -type f -name '*.c' | LC_ALL=C sort)
ASFILES += $(shell  find -L * -type f -name '*.S' | LC_ALL=C sort)
CPPFILES += $(shell  find -L * -type f -name '*.cpp' | LC_ALL=C sort)

LD = ld
CC = gcc
CXX = g++
AS = as

LDFLAGS  = --gc-sections  -entry=main -static -nostdlib \
    -m elf_x86_64 -z noexecstack

CFLAGS    = -nostdlib -I../../hllib/include -I. -nostdinc -L../../hllib/bin-$(BUILD_ARCH) -lhllib\
	-I../../hllib/include/arch/$(BUILD_ARCH) \
	-ffreestanding -fshort-wchar -mno-red-zone \
 	-fno-omit-frame-pointer -fno-exceptions -ffunction-sections -fdata-sections -O0 -m64 -static

CPPFLAGS  = $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics

override OBJ := $(addprefix $(OBJDIR)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o))

.PHONY: all
all: $(BINDIR)/desktop.elf

$(BINDIR)/desktop.elf: GNUmakefile $(OBJ)
	mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

# Compilation rules for *.c files.
$(OBJDIR)/%.c.o: ./%.c GNUmakefile
	@echo C $<
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) -c $< -o $@

# Compilation rules for *.S files.
$(OBJDIR)/%.S.o: ./%.S GNUmakefile
	@echo GAS $<
	mkdir -p "$$(dirname $@)"
	$(AS) -o $@ $<

$(OBJDIR)/%.s.o: ./%.s GNUmakefile
	echo GAS $<
	mkdir -p "$$(dirname $@)"
	$(AS) -o $@ $<

$(OBJDIR)/%.cpp.o: ./%.cpp GNUmakefile
	echo C++ $<
	mkdir -p "$$(dirname $@)"
	$(CXX) $(CPPFLAGS)  -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(BINDIR)