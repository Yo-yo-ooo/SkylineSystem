# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:

# This is the name that our final executable will have.
# Change as needed.
override OUTPUT := kernel

# Convenience macro to reliably declare user overridable variables.
override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

# Target architecture to build for. Default to x86_64.
include ../gdef.mk
KARCH = $(BUILD_ARCH)
override X86MEM_PATH = $(SS_BUILD_DIR)/ablib/arch/x86_64/x86mem
# Destination directory on install (should always be empty by default).
 $(call USER_VARIABLE,DESTDIR,)

# Install prefix; /usr/local is a good, standard default pick.
 $(call USER_VARIABLE,PREFIX,/usr/local)

# Check if the architecture is supported.
ifeq ($(filter $(KARCH),aarch64 loongarch64 riscv64 x86_64),)
    $(error Architecture $(KARCH) not supported)
endif

# User controllable C++ compiler command.
 $(call USER_VARIABLE,KCXX,clang++)

# User controllable C compiler command.
 $(call USER_VARIABLE,KCC,clang)

# User controllable linker command.
 $(call USER_VARIABLE,KLD,ld.lld)

# User controllable C flags.
 $(call USER_VARIABLE,KCFLAGS,-g -pipe)

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
    ifeq ($(shell ( ! test -f src/limine.h ); echo $$?),0)
        $(error Please run the ./get-deps script first)
    endif
endif

# Check if KCC is Clang.
override KCC_IS_CLANG := $(shell ! $(KCC) --version 2>/dev/null | grep 'clang' >/dev/null 2>&1; echo $$?)

# ★ 修复: 检测 KCXX 而不是 CXX (原版检测错变量, 环境里 CXX=g++ 时
#   Clang 分支永远不进)
override KCXX_IS_CLANG := $(shell ! $(KCXX) --version 2>/dev/null | grep 'clang' >/dev/null 2>&1; echo $$?)

ifndef NOStackCanary
    KCFLAGS +=  -fstack-protector-strong \
    -mstack-protector-guard=global 
else
    KCFLAGS +=  -fno-stack-protector 
endif

# ==================== KASLR / DEBUG 控制 ====================
# 用法:
#   make                    → KASLR 构建 (-fPIE -pie, Limine KASLR=yes 生效)
#   make DEBUG=1            → 调试构建 (无 KASLR, GDB 地址=链接地址)
#
# 依据: Clang 支持 -fPIE + -mcmodel=kernel 编译 (GCC 拒绝);
#       -Bsymbolic + 内核在 2GB 窗口内 → lld 可将 32S 重定位 relax 成
#       RELATIVE。链接后必须验证: readelf -r | grep 32S 为空。
ifeq ($(KARCH),x86_64)
    ifndef DEBUG
        # Release: PIE + KASLR
        KCFLAGS += -fPIE
        KLDFLAGS += -pie
        $(info ============================================)
        $(info  BUILD: Release (KASLR enabled))
        $(info  Flags: -fPIE -pie -mcmodel=kernel)
        $(info  NOTE:  verify with 'readelf -r bin-$(KARCH)/$(OUTPUT) | grep 32S')
        $(info        (empty output = OK; nonempty = add -fno-plt))
        $(info ============================================)
    else
        $(info ============================================)
        $(info  BUILD: DEBUG (no KASLR))
        $(info  Flags: -fno-PIC -mcmodel=kernel)
        $(info  GDB:   symbols = link addresses, direct use)
        $(info ============================================)
    endif
endif

# Internal C flags that should not be changed by the user.
override KCFLAGS += \
    -Wno-implicit-function-declaration \
    -Wno-c++11-narrowing \
    -w \
    -Wno-everything \
    -I$(SS_BUILD_DIR)/kernel/include \
    -I$(SS_BUILD_DIR)/ablib \
    -nostdinc \
    -ffreestanding \
    -fno-stack-check \
    -fno-lto \
    -ffunction-sections \
    -fdata-sections \
    -O2 \
    -g3 \
    -DLIMINE_API_REVISION=5 \
    -fpermissive \
    -Wno-error=deprecated \

# Internal C preprocessor flags that should not be changed by the user.
override KCPPFLAGS := \
    -I src \
    -isystem ../ablib/freestndchdrs/$(KARCH)/include \
    -I$(SS_BUILD_DIR)/kernel/include \
    -Ifakestdlib \
    -DLIMINE_API_REVISION=6 \
    $(KCPPFLAGS) \
    -MMD \
    -MP \
    -fno-rtti \
    -fno-exceptions

KCC += -std=gnu17
KCXX += -isystem  -std=gnu++17

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
        -mno-sse2 \
        -DNOT_COMPILE_X86MEM=$(NOT_COMPILE_X86MEM) \
        -DCOMPILER_SUPPORT_AVX512=$(if $(filter yes,$(COMPILER_SUPPORT_AVX512)),1,0) \
        -DCOMPILER_SUPPORT_AVX=$(if $(filter yes,$(COMPILER_SUPPORT_AVX)),1,0) \
        -DCOMPILER_SUPPORT_AVX2=$(if $(filter yes,$(COMPILER_SUPPORT_AVX2)),1,0) \
        -DCOMPILER_SUPPORT_SSE_4_2=$(if $(filter yes,$(COMPILER_SUPPORT_SSE_4_2)),1,0) \
        -DUSE_HOST_CPU_EXTENSIONS=$(USE_HOST_CPU_EXTENSIONS)
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
        -mcpu=generic \
        -march=armv8-a+nofp+nosimd \
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
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    -gc-sections \
    -Bsymbolic \
    -T linker-$(KARCH).ld


# Use "find" to glob all *.c, *.S, and *.asm files in the tree and obtain the
# object and header dependency file names.
define find_sources
 $(patsubst src/%,%,$(shell find src -type f -name "*.$(1)" -not -path "*/arch/*" | LC_ALL=C sort)) \
 $(patsubst src/%,%,$(shell find src/arch/$(KARCH) -type f -name "*.$(1)" | LC_ALL=C sort))
endef

CFILES := $(call find_sources,c)
ASFILES := $(call find_sources,S)
CPPFILES := $(call find_sources,cpp)

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
bin-$(KARCH)/$(OUTPUT): Makefile linker-$(KARCH).ld $(OBJ)
	@mkdir -p "$$(dirname $@)"
	@echo LD kernel
ifeq ($(NOT_COMPILE_X86MEM),0)
	@$(KLD) $(OBJ) $(X86MEM_PATH)/memops.o \
	            $(KLDFLAGS) -o $@
else
	@$(KLD) $(OBJ) $(KLDFLAGS) -o $@
endif

# ★ KASLR 验证 target: 检查 32S 重定位是否清零
.PHONY: kaslr-check
kaslr-check: bin-$(KARCH)/$(OUTPUT)
	@echo "== KASLR relocation check =="
	@count=$$(readelf -r bin-$(KARCH)/$(OUTPUT) 2>/dev/null | grep -c 'R_X86_64_32' || true); \
	if [ "$$count" = "0" ]; then \
	    echo "[OK] No R_X86_64_32/32S relocations — KASLR safe"; \
	else \
	    echo "[FAIL] $$count 32/32S relocations remain:"; \
	    readelf -r bin-$(KARCH)/$(OUTPUT) | grep 'R_X86_64_32' | head -20; \
	    echo "Fix: add -fno-plt to KCFLAGS/KCPPFLAGS and rebuild"; \
	    exit 1; \
    fi
	@type=$$(readelf -h bin-$(KARCH)/$(OUTPUT) | grep 'Type:'); \
    echo "ELF $$type (need DYN for KASLR)"

# Include header dependencies.
-include $(HEADER_DEPS)

# 4. 针对这些特定目标，在 aarch64 架构下移除 Flag
ifeq ($(KARCH), aarch64)
obj-$(KARCH)/klib/0aarch64mem.cpp.o: KCFLAGS := -march=armv8-a+fp+simd $(filter-out -mgeneral-regs-only -march=armv8-a+nofp+nosimd, $(KCFLAGS))
endif

# Compilation rules for *.c files.
obj-$(KARCH)/%.c.o: src/%.c Makefile
	@echo C $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj-$(KARCH)/%.S.o: src/%.S Makefile
	@echo GAS $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

obj-$(KARCH)/%.cpp.o: src/%.cpp Makefile
	@echo C++ $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

ifeq ($(KARCH),x86_64)
# Compilation rules for *.asm (nasm) files.
obj-$(KARCH)/%.asm.o: src/%.asm Makefile
	@echo NASM $<
	@mkdir -p "$$(dirname $@)"
	@nasm $(KNASMFLAGS) $< -o $@
endif

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf bin-* obj-*

.PHONY: distclean
distclean:
	rm -rf bin-* obj-* src/limine.h

.PHONY: install
install: all
	install -d "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	install -m 644 bin-$(KARCH)/$(OUTPUT) "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(KARCH)"

.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(KARCH)"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"