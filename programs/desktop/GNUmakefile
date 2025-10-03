include ../def.mk


CFILES += $(shell  find -L * -type f -name '*.c' | LC_ALL=C sort)
ASFILES += $(shell  find -L * -type f -name '*.S' | LC_ALL=C sort)
CPPFILES += $(shell  find -L * -type f -name '*.cpp' | LC_ALL=C sort)


LDFLAGS   = --gc-sections  -entry=main -static -nostdlib \
    -m elf_x86_64 -unresolved-symbols=ignore-all 

CFLAGS    = -I../../hllib/include \
	-ffreestanding -fshort-wchar -mno-red-zone \
 	-fno-omit-frame-pointer -fno-exceptions -ffunction-sections -fdata-sections -O2 -m64

CPPFLAGS  = $(CFLAGS) -std=gnu++20 -fno-threadsafe-statics

override OBJ := $(addprefix $(OBJDIR)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o))

.PHONY: all
all: $(BINDIR)/desktop.elf

$(BINDIR)/desktop.elf: GNUmakefile $(OBJ)
	@mkdir -p "$$(dirname $@)"
	@$(LD) $(LDFLAGS) -o $@ $(OBJ)

# Compilation rules for *.c files.
$(OBJDIR)/%.c.o: ./%.c GNUmakefile
	@echo C $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(CFLAGS) -c $< -o $@

# Compilation rules for *.S files.
$(OBJDIR)/%.S.o: ./%.S GNUmakefile
	@echo GAS $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(ASFILES)  -c $< -o $@

$(OBJDIR)/%.cpp.o: ./%.cpp GNUmakefile
	@echo C++ $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(CPPFLAGS)  -c $< -o $@

