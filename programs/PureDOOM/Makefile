OBJDIR := ../object
BINDIR := ../bin
PROJ_NAME := DOOM

include ../../gdef.mk

# Deliberately NOT a `find`: src/DOOM/ holds the upstream reference sources
# which are not part of the build (the game lives in PureDOOM.h).
CFILES   := skyline/doom_main.c
ASFILES  :=
CPPFILES :=

LD  := ld
CC  := gcc
CXX := g++
AS  := as

# NOTE: -T ./link/console_x86_64.ld is mandatory, not cosmetic. It reserves a
# WRITABLE .prepad page at 0x400000 — that page IS the WM protocol page
# (SKYWIN_PROTO_PAGE_VA, see graphic/winstyle.h); the WM shares it into its
# own address space before the client starts. Without it the ELF's own
# read-only header lands at 0x400000 and the first OUT_SEQ bump faults with
# "write to RO" (addr=0x400048).
LDFLAGS := -entry=_start -static -nostdlib -m elf_x86_64 -z noexecstack \
           -T ./link/console_x86_64.ld

# DOOM's 1993 code is not warning-clean and predates C23: `enum { false, true }`
# collides with the C23 keywords, so this project is pinned to gnu17.
# -fno-strict-aliasing: the original engine aliases freely through byte*.
CFLAGS := -nostdlib -nostdinc \
    -I../../ablib/freestndchdrs/$(BUILD_ARCH)/include \
    -I. -I../../lib/include/stdc -I../../lib/include -I../../lib/include/base/arch/$(BUILD_ARCH) \
    -ffreestanding -fshort-wchar -mno-red-zone \
    -fno-omit-frame-pointer -fno-exceptions \
    -ffunction-sections -fdata-sections -O2 -m64 -static -g3 \
    -std=gnu17 -fno-strict-aliasing \
    -Wall -Wextra \
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses \
    -Wno-enum-compare -Wno-unknown-pragmas -Wno-sign-compare \
    -Wno-implicit-fallthrough -Wno-maybe-uninitialized

CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics

OBJ := $(addprefix $(OBJDIR)/$(PROJ_NAME)/, \
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o) \
    $(CPPFILES:.cpp=.cpp.o) \
)

.PHONY: all clean
all: $(BINDIR)/doom.elf

$(BINDIR)/doom.elf: $(OBJ) Makefile
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ) -L../../lib/bin -l:lib.a

$(OBJDIR)/$(PROJ_NAME)/%.c.o: %.c Makefile PureDOOM.h
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/$(PROJ_NAME)/%.S.o: %.S Makefile
	@echo "CC $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/$(PROJ_NAME)/%.cpp.o: %.cpp Makefile
	@echo "C++ $<"
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR)/$(PROJ_NAME) $(BINDIR)/doom.elf
