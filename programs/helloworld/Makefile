OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := HW

include ../../gdef.mk


CFILES   := $(shell find . -type f -name '*.c'   | LC_ALL=C sort | sed 's|^\./||')
ASFILES  := $(shell find . -type f -name '*.S'   | LC_ALL=C sort | sed 's|^\./||')
CPPFILES := $(shell find . -type f -name '*.cpp' | LC_ALL=C sort | sed 's|^\./||')

LD  := ld
CC  := gcc
CXX := g++
AS  := as

LDFLAGS := -entry=_start -static -nostdlib -m elf_x86_64 -z noexecstack 

CFLAGS := -nostdlib -nostdinc \
    -I../../ablib/freestndchdrs/$(BUILD_ARCH)/include \
    -I./include -I. -I../../lib/include/stdc -I../../lib/include -I../../lib/include/base/arch/$(BUILD_ARCH) \
    -ffreestanding -fshort-wchar -mno-red-zone \
    -fno-omit-frame-pointer -fno-exceptions \
    -ffunction-sections -fdata-sections -O0 -m64 -static -g3


CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics 


OBJ := $(addprefix $(OBJDIR)/$(PROJ_NAME)/, \
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o) \
    $(CPPFILES:.cpp=.cpp.o) \
)

.PHONY: all clean
all: $(BINDIR)/hw.elf


 $(BINDIR)/hw.elf: $(OBJ) Makefile
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ) -L../../lib/bin -l:lib.a 

 $(OBJDIR)/$(PROJ_NAME)/%.c.o: %.c Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

 $(OBJDIR)/$(PROJ_NAME)/%.S.o: %.S Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


 $(OBJDIR)/$(PROJ_NAME)/%.s.o: %.s Makefile
	@echo "AS $<"
	@mkdir -p $(dir $@)
	$(AS) -o $@ $<


 $(OBJDIR)/$(PROJ_NAME)/%.cpp.o: %.cpp Makefile
	@echo "C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(BINDIR)