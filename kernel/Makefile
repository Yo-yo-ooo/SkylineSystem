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

#  修复: 检测 KCXX 而不是 CXX (原版检测错变量, 环境里 CXX=g++ 时
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
# "零警告" 由下方 -Wno-* 支撑。回收 -Wno-* 的路线图:
#   阶段 1: unused-value / unused-function (低风险, 死代码清理后)
#   阶段 2: unused-parameter (需 -Wextra 语义审计)
#   阶段 3: null-* / writable-strings 等 (需 API 语义变更)
#   每阶段独立 PR + 金样门禁, 不做一次性大爆炸。
override KCFLAGS += \
    -Wall -Wextra \
    -Wno-implicit-function-declaration \
    -Wno-c++11-narrowing \
    -Wno-macro-redefined \
    -Wno-missing-field-initializers \
    -Wno-missing-declarations \
    -Wno-unknown-pragmas \
    -Wno-address-of-packed-member \
    -Wno-null-pointer-arithmetic \
    -Wno-null-pointer-subtraction \
    -Wno-null-conversion \
    -Wno-null-arithmetic \
    -Wno-unused-parameter \
    -Wno-unused-value \
    -Wno-unused-function \
    -Wno-incompatible-pointer-types-discards-qualifiers \
    -Wno-writable-strings \
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
    -I../lib/include \
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


# Use "find" to glob all *.c, *.S, *.cpp, *.asm files in the tree and obtain the
# object and header dependency file names.
#
# 目录分层（全模块化重构后）：
#   src/                    Tier0 固定核心（boot/GDT/IDT/中断/页表/调度核心/
#                           syscall 入口/kmod 机制/klib/mem/print/elf/flanterm）
#   modules/builtin/common/ Tier1 内建组件实现（驱动/文件系统/网络/分区/ACPI/
#                           SMBIOS；与原 src 非 arch 目录一一对应，所有架构编译）
#   modules/builtin/<arch>/ Tier1 内建组件的架构相关实现（平台/驱动/syscall 家族）
#   modules/loadable/       可加载 .ko 演示模块（独立 modules 目标编译）
# 内建组件仍静态链接进内核镜像，但源码与生命周期均归属 modules 树，由
# KmodBootComponents() 按 manifest 依赖拓扑初始化（见 docs/kernel-components.md）。
define find_sources
 $(patsubst src/%,%,$(shell find src -type f -name "*.$(1)" -not -path "*/arch/*" | LC_ALL=C sort)) \
 $(patsubst src/%,%,$(shell find src/arch/$(KARCH) -type f -name "*.$(1)" | LC_ALL=C sort))
endef

# 内建组件的源文件（相对 modules/），对象落在 obj-$(KARCH)/modules/<rel>。
define find_module_sources
 $(patsubst modules/%,%,$(shell find modules/builtin/common -type f -name "*.$(1)" 2>/dev/null | LC_ALL=C sort)) \
 $(patsubst modules/%,%,$(shell find modules/builtin/$(KARCH) -type f -name "*.$(1)" 2>/dev/null | LC_ALL=C sort))
endef

CFILES := $(call find_sources,c)
ASFILES := $(call find_sources,S)
CPPFILES := $(call find_sources,cpp)
MOD_CFILES := $(call find_module_sources,c)
MOD_ASFILES := $(call find_module_sources,S)
MOD_CPPFILES := $(call find_module_sources,cpp)

ifeq ($(KARCH),x86_64)
override NASMFILES := $(shell cd src && find -L * -type f -name '*.asm' | LC_ALL=C sort)
override MOD_NASMFILES := $(patsubst modules/%,%,$(shell find modules/builtin -type f -name '*.asm' 2>/dev/null | LC_ALL=C sort))
endif
override OBJ := $(addprefix obj-$(KARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o))
override OBJ += $(addprefix obj-$(KARCH)/modules/,$(MOD_CFILES:.c=.c.o) $(MOD_ASFILES:.S=.S.o) $(MOD_CPPFILES:.cpp=.cpp.o))
ifeq ($(KARCH),x86_64)
override OBJ += $(addprefix obj-$(KARCH)/,$(NASMFILES:.asm=.asm.o))
override OBJ += $(addprefix obj-$(KARCH)/modules/,$(MOD_NASMFILES:.asm=.asm.o))
endif
override HEADER_DEPS := $(addprefix obj-$(KARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))
override HEADER_DEPS += $(addprefix obj-$(KARCH)/modules/,$(MOD_CFILES:.c=.c.d) $(MOD_ASFILES:.S=.S.d))

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

#  KASLR 验证 target: 检查 32S 重定位是否清零
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

# Tier1 内建组件（modules/builtin/**）编译规则：源文件来自 modules 树，
# 对象落在 obj-$(KARCH)/modules/builtin/<rel>，与 Tier0 src 对象分离。
obj-$(KARCH)/modules/%.c.o: modules/%.c Makefile
	@echo [MOD-C] $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

obj-$(KARCH)/modules/%.S.o: modules/%.S Makefile
	@echo [MOD-GAS] $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

obj-$(KARCH)/modules/%.cpp.o: modules/%.cpp Makefile
	@echo [MOD-C++] $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

ifeq ($(KARCH),x86_64)
obj-$(KARCH)/modules/%.asm.o: modules/%.asm Makefile
	@echo [MOD-NASM] $<
	@mkdir -p "$$(dirname $@)"
	@nasm $(KNASMFLAGS) $< -o $@
endif

ifeq ($(KARCH),x86_64)
# Compilation rules for *.asm (nasm) files.
obj-$(KARCH)/%.asm.o: src/%.asm Makefile
	@echo NASM $<
	@mkdir -p "$$(dirname $@)"
	@nasm $(KNASMFLAGS) $< -o $@
endif

# ==================== kmod 可加载模块 (.ko) 构建 ====================
# 独立 PHONY 目标：把 modules/loadable/*.cpp 编译成 bin-$(KARCH)/modules/*.ko，
# 不进入 OBJ / 内核链接（默认 all 目标不含 .ko，Tier1 内建组件另走 modules/builtin）。
#
# 加载器约束（见 kernel/include/kmod/kmod_abi.h 与 src/arch/x86_64/kmod/elf_loader.cpp）：
#   - ET_REL，-fno-pic/-fno-pie -mcmodel=large（alloc 段只允许绝对 64 位重定位）；
#   - .in/.out/.kmod_manifest 段由模块头宏生成，非 const 且 8 字节对齐；
#   - 只允许 R_X86_64_64/PC32/PLT32/32/32S（alloc 目标），用 modules-check 复核。
ifeq ($(KARCH),x86_64)
# 支持直接 `make -C kernel modules`：此时 SS_BUILD_DIR 没有被根 Makefile export，
# 用 CURDIR 反推仓库根，保证 -I 指向真实的 kernel/include。
KMOD_ROOT := $(if $(SS_BUILD_DIR),$(SS_BUILD_DIR),$(abspath $(CURDIR)/..))
KMOD_MODULE_SRCS := $(wildcard modules/loadable/*.cpp)
KMOD_MODULE_OBJS := $(patsubst modules/loadable/%.cpp,obj-$(KARCH)/loadable/%.cxx.o,$(KMOD_MODULE_SRCS))
KMOD_MODULE_KOS := $(patsubst modules/loadable/%.cpp,bin-$(KARCH)/modules/%.ko,$(KMOD_MODULE_SRCS))

# 模块编译 flags 与内核主 flags 刻意分离：内核用 -fPIE/-mcmodel=kernel，
# 模块必须 -fno-pie/-mcmodel=large 才能保持"绝对地址 + 加载期回填"的模型。
KMOD_CXXFLAGS := -std=gnu++17 -O2 -g \
    -target x86_64-unknown-none \
    -fno-pic -fno-pie -mcmodel=large \
    -mno-red-zone -mno-sse -mno-sse2 -mno-80387 -mno-mmx \
    -fno-stack-protector -fno-stack-check -fno-lto \
    -fno-exceptions -fno-rtti -ffreestanding -nostdinc \
    -Wno-c++20-designator \
    -I$(KMOD_ROOT)/kernel/include \
    -isystem $(KMOD_ROOT)/ablib/freestndchdrs/$(KARCH)/include
KMOD_LDFLAGS := -m elf_x86_64 -r

.PHONY: modules
modules: $(KMOD_MODULE_KOS)

# 中间 .cxx.o 保留，避免每次 make modules 都全量重编（.ko 依赖它做增量）。
.SECONDARY: $(KMOD_MODULE_OBJS)

obj-$(KARCH)/loadable/%.cxx.o: modules/loadable/%.cpp
	@echo KMOD-CXX $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KMOD_CXXFLAGS) -c $< -o $@

bin-$(KARCH)/modules/%.ko: obj-$(KARCH)/loadable/%.cxx.o
	@echo KMOD-LD $@
	@mkdir -p "$$(dirname $@)"
	@$(KLD) $(KMOD_LDFLAGS) -o $@ $<

# 构建后校验：.ko 的 alloc 段重定位必须只落在加载器白名单里。
.PHONY: modules-check
modules-check: $(KMOD_MODULE_KOS)
	@echo "== kmod reloc check (allowed: R_X86_64_64/PC32/PLT32/32/32S) =="
	@fail=0; for ko in $(KMOD_MODULE_KOS); do \
	    bad=$$(readelf -rW "$$ko" | grep -oE 'R_X86_64_[A-Z0-9_]+' | \
	           grep -vE '^R_X86_64_(64|PC32|PLT32|32|32S)$$' | sort -u); \
	    if [ -n "$$bad" ]; then echo "[FAIL] $$ko: $$bad"; fail=1; \
	    else echo "[OK]   $$ko"; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "kmod reloc check FAILED"; exit 1; fi; \
	echo "kmod reloc check OK"
else
.PHONY: modules modules-check
modules:
	@echo "modules: skipped (.ko 仅 x86_64，当前 KARCH=$(KARCH))"
modules-check:
	@echo "modules-check: skipped (.ko 仅 x86_64，当前 KARCH=$(KARCH))"
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