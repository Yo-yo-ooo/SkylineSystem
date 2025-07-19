# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:

# This is the name that our final executable will have.
# Change as needed.
override OUTPUT := kernel

# Convenience macro to reliably declare user overridable variables.
override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

# Target architecture to build for. Default to x86_64.
$(call USER_VARIABLE,KARCH,x86_64)

# Destination directory on install (should always be empty by default).
$(call USER_VARIABLE,DESTDIR,)

# Install prefix; /usr/local is a good, standard default pick.
$(call USER_VARIABLE,PREFIX,/usr/local)

# Check if the architecture is supported.
ifeq ($(filter $(KARCH),aarch64 loongarch64 riscv64 x86_64),)
    $(error Architecture $(KARCH) not supported)
endif

# User controllable C++ compiler command.
$(call USER_VARIABLE,KCXX,g++)

# User controllable C compiler command.
$(call USER_VARIABLE,KCC,cc)

# User controllable linker command.
$(call USER_VARIABLE,KLD,ld)

# User controllable C flags.
$(call USER_VARIABLE,KCFLAGS,-g -O2 -pipe)

# User controllable C preprocessor flags. We set none by default.
$(call USER_VARIABLE,KCPPFLAGS,)

ifeq ($(KARCH),x86_64)
    # User controllable nasm flags.
    $(call USER_VARIABLE,KNASMFLAGS,-F dwarf -g)
endif

# User controllable linker flags. We set none by default.
$(call USER_VARIABLE,KLDFLAGS,)

# Ensure the dependencies have been obtained.
ifneq ($(shell ( test '$(MAKECMDGOALS)' = clean || test '$(MAKECMDGOALS)' = distclean ); echo $$?),0)
    ifeq ($(shell ( ! test -d freestnd-c-hdrs || ! test -d cc-runtime || ! test -f src/limine.h || ! test -d freestnd-cxx-hdrs); echo $$?),0)
        $(error Please run the ./get-deps script first)
    endif
endif

# Check if KCC is Clang.
override KCC_IS_CLANG := $(shell ! $(KCC) --version 2>/dev/null | grep 'clang' >/dev/null 2>&1; echo $$?)

# Check if CXX is Clang.
override KCXX_IS_CLANG := $(shell ! $(CXX) --version 2>/dev/null | grep 'clang' >/dev/null 2>&1; echo $$?)

# Internal C flags that should not be changed by the user.
override KCFLAGS += \
	-w \
	-I$(SS_BUILD_DIR)/kernel/include \
	-Icc-runtime -Lcc-runtime \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
	-fpermissive \
	-Wno-error=deprecated \

# Internal C preprocessor flags that should not be changed by the user.
override KCPPFLAGS := \
    -I src \
	-isystem freestnd-c-hdrs/$(KARCH)/include \
	-I$(SS_BUILD_DIR)/kernel/include \
	-Icc-runtime -Lcc-runtime \
    $(KCPPFLAGS) \
    -MMD \
    -MP

KCC += -std=gnu17
KCXX += -isystem freestnd-cxx-hdrs/$(KARCH)/include -std=gnu++17

ifeq ($(KARCH),x86_64)
    # Internal nasm flags that should not be changed by the user.
    override KNASMFLAGS += \
        -Wall
endif

# Architecture specific internal flags.
ifeq ($(KARCH),x86_64)
    ifeq ($(KCC_IS_CLANG),1)
        override KCC += \
            -target x86_64-unknown-none
    endif
	ifeq ($(KCXX_IS_CLANG),1)
		override KCXX += \
			-target x86_64-unknown-none
	endif
    override KCFLAGS += \
        -m64 \
        -march=x86-64 \
        -mno-80387 \
		-mno-mmx \
		-mno-red-zone \
        -mcmodel=kernel \
		-mno-sse \
        -mno-sse2 
		
    override KLDFLAGS += \
        -m elf_x86_64
    override KNASMFLAGS += \
        -f elf64
endif
ifeq ($(KARCH),aarch64)
    ifeq ($(KCC_IS_CLANG),1)
        override KCC += \
            -target aarch64-unknown-none
    endif
	ifeq ($(KCXX_IS_CLANG),1)
		override KCXX += \
			-target aarch64-unknown-none
	endif
    override KCFLAGS += \
        -mgeneral-regs-only
    override KLDFLAGS += \
        -m aarch64elf
endif
ifeq ($(KARCH),riscv64)
    ifeq ($(KCC_IS_CLANG),1)
        override KCC += \
            -target riscv64-unknown-none
        override KCFLAGS += \
            -march=rv64imac
    else
        override KCFLAGS += \
            -march=rv64imac_zicsr_zifencei
    endif
	ifeq ($(KCXX_IS_CLANG),1)
        override KCXX += \
            -target riscv64-unknown-none
        override KCPPFLAGS += \
            -march=rv64imac
    else
        override KCPPFLAGS += \
            -march=rv64imac_zicsr_zifencei
    endif
    override KCFLAGS += \
        -mabi=lp64 \
        -mno-relax
    override KLDFLAGS += \
        -m elf64lriscv \
        --no-relax
endif
ifeq ($(KARCH),loongarch64)
    ifeq ($(KCC_IS_CLANG),1)
        override KCC += \
            -target loongarch64-unknown-none
    endif
	ifeq ($(KCXX_IS_CLANG),1)
		override KCXX += \
			-target loongarch64-unknown-none
	endif
    override KCFLAGS += \
        -march=loongarch64 \
        -mabi=lp64s
    override KLDFLAGS += \
        -m elf64loongarch \
        --no-relax
endif

# Internal linker flags that should not be changed by the user.
override KLDFLAGS += \
	-unresolved-symbols=ignore-all \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    -gc-sections \
	-Bsymbolic \
	--gc-sections   \
    -T linker-$(KARCH).ld

# Use "find" to glob all *.c, *.S, and *.asm files in the tree and obtain the
# object and header dependency file names.
override CFILES := $(shell cd src && find -L * -type f -path './arch' -prune -o -name '*.c' -print | LC_ALL=C sort)
override ASFILES := $(shell cd src && find -L * -type f -path './arch' -prune -o -name '*.S' | LC_ALL=C sort)
override CPPFILES := $(shell cd src && find -L * -type f -path './arch' -prune -o -name '*.cpp' | LC_ALL=C sort)
CFILES += $(shell cd src && find -L arch/$(KARCH) -type f -name '*.c' | LC_ALL=C sort)
ASFILES += $(shell cd src && find -L arch/$(KARCH) -type f -name '*.S' | LC_ALL=C sort)
CPPFILES += $(shell cd src && find -L arch/$(KARCH) -type f -name '*.cpp' | LC_ALL=C sort)

ifeq ($(KARCH),x86_64)
override NASMFILES := $(shell cd src && find -L * -type f -name '*.asm' | LC_ALL=C sort)
endif
override OBJ := $(addprefix obj-$(KARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o))
ifeq ($(KARCH),x86_64)
override OBJ += $(addprefix obj-$(KARCH)/,$(NASMFILES:.asm=.asm.o))
endif
override HEADER_DEPS := $(addprefix obj-$(KARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

# Default target.
.PHONY: all
all: bin-$(KARCH)/$(OUTPUT)

# Link rules for the final executable.
bin-$(KARCH)/$(OUTPUT): GNUmakefile linker-$(KARCH).ld $(OBJ)
	@mkdir -p "$$(dirname $@)"
	@$(KLD) $(OBJ) $(KLDFLAGS) -o $@

# Include header dependencies.
-include $(HEADER_DEPS)

# Compilation rules for *.c files.
obj-$(KARCH)/%.c.o: src/%.c GNUmakefile
	@echo C $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj-$(KARCH)/%.S.o: src/%.S GNUmakefile
	@echo GAS $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

obj-$(KARCH)/%.cpp.o: src/%.cpp GNUmakefile
	@echo C++ $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

ifeq ($(KARCH),x86_64)
# Compilation rules for *.asm (nasm) files.
obj-$(KARCH)/%.asm.o: src/%.asm GNUmakefile
	@mkdir -p "$$(dirname $@)"
	@nasm $(KNASMFLAGS) $< -o $@
endif

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf bin-$(KARCH) obj-$(KARCH)

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean:
	rm -rf bin-* obj-* freestnd-c-hdrs freestnd-cxx-hdrs src/limine.h

# Install the final built executable to its final on-root location.
.PHONY: install
install: all
	install -d "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	install -m 644 bin-$(KARCH)/$(OUTPUT) "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(KARCH)"

# Try to undo whatever the "install" target did.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(KARCH)"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
