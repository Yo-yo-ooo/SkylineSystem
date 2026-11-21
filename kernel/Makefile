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

# ==================== 组件实现文件 -> .ko 迁移清单 ====================
# modules/mcr/<component>.sources（MCR = module compile rules）里的路径
# （相对 modules/）会：
#   1. 从内核 OBJ 的 find_module_sources 结果中排除；
#   2. 用 KMOD_CXXFLAGS 编译成额外模块对象，链接进对应组件 .ko。
KMOD_MCR_DIR := modules/mcr
KMOD_MCR_FILES := $(wildcard $(KMOD_MCR_DIR)/*.sources)
KMOD_MOVED_SRCS := $(foreach f,$(KMOD_MCR_FILES),$(addprefix modules/,$(shell sed -e 's/#.*//' -e '/^[[:space:]]*$$/d' $(f))))

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
#   modules/common/         DynModule 共享实现库（驱动/文件系统/网络/分区/
#                           ACPI/SMBIOS；编入 kernel，导出 kernel.component.v1）
#   modules/<arch>/         架构实现 + components/（组件壳 -> 独立 .ko）
#   modules/loadable/       demo/扩展 .ko（独立 modules 目标编译）
# 组件壳不再编进 kernel；38 组件 .ko + 17 demo .ko 由 modules-saf 打进
# bin-$(KARCH)/modules.saf，Limine 只加载这一个文件（见 docs/kernel-dynlink.md）。
define find_sources
 $(patsubst src/%,%,$(shell find src -type f -name "*.$(1)" -not -path "*/arch/*" | LC_ALL=C sort)) \
 $(patsubst src/%,%,$(shell find src/arch/$(KARCH) -type f -name "*.$(1)" | LC_ALL=C sort))
endef

# 共享实现库源文件（相对 modules/，排除 components/ 与 dosvm/），
# 对象落在 obj-$(KARCH)/modules/<rel>。
# 组件壳（components/ 与 dosvm/）不再编进内核镜像：它们各自编译成独立 .ko，
# 由 modules.saf → KmodSafIngest → KmodBootLoad 动态加载。其余实现代码
# （驱动/FS/网络/平台/syscall 家族）仍作为内核共享实现库编进镜像，供 .ko
# 通过 kernel.component.v1 的生成导出表解析。
define find_module_sources
 $(patsubst modules/%,%,$(filter-out $(KMOD_MOVED_SRCS),$(shell find modules/common -type f -name "*.$(1)" -not -path "*/components/*" 2>/dev/null | LC_ALL=C sort))) \
 $(patsubst modules/%,%,$(filter-out $(KMOD_MOVED_SRCS),$(shell find modules/$(KARCH) -type f -name "*.$(1)" -not -path "*/components/*" -not -path "*/dosvm/*" 2>/dev/null | LC_ALL=C sort)))
endef

CFILES := $(call find_sources,c)
ASFILES := $(call find_sources,S)
CPPFILES := $(call find_sources,cpp)
MOD_CFILES := $(call find_module_sources,c)
MOD_ASFILES := $(call find_module_sources,S)
MOD_CPPFILES := $(call find_module_sources,cpp)

ifeq ($(KARCH),x86_64)
override NASMFILES := $(shell cd src && find -L * -type f -name '*.asm' | LC_ALL=C sort)
override MOD_NASMFILES := $(patsubst modules/%,%,$(shell find modules/common modules/$(KARCH) -type f -name '*.asm' 2>/dev/null | LC_ALL=C sort))
endif
override OBJ := $(addprefix obj-$(KARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(CPPFILES:.cpp=.cpp.o))
override OBJ += $(addprefix obj-$(KARCH)/modules/,$(MOD_CFILES:.c=.c.o) $(MOD_ASFILES:.S=.S.o) $(MOD_CPPFILES:.cpp=.cpp.o))
ifeq ($(KARCH),x86_64)
override OBJ += $(addprefix obj-$(KARCH)/,$(NASMFILES:.asm=.asm.o))
override OBJ += $(addprefix obj-$(KARCH)/modules/,$(MOD_NASMFILES:.asm=.asm.o))
endif
override HEADER_DEPS := $(addprefix obj-$(KARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))
override HEADER_DEPS += $(addprefix obj-$(KARCH)/modules/,$(MOD_CFILES:.c=.c.d) $(MOD_ASFILES:.S=.S.d))

# ==================== .kout 导出表：构建期生成，不写死源码 ====================
# 组件 .ko 的未定义符号 = 内核必须通过 kernel.component.v1 导出的符号集合。
# 以前这份表是仓库里提交的 component_exports.cpp（197 行手写 extern +
# KMOD_KEXPORT_STR，靠 Python 脚本重新生成后提交）；现在改为构建时生成：
#   1. nm 扫描 bin-$(KARCH)/components/**/*.ko 的未定义符号（已由其它 .ko
#      提供的跨模块符号会跳过）；
#   2. 生成 obj-$(KARCH)/generated/component_exports.S，每条 .kout 记录写
#      ".quad <symbol>" —— 地址是**链接期重定位**，符号名来自编译产物，
#      源码树里不再有符号清单；
#   3. 汇编成 .o 并链接进内核（导出项格式与 KMOD_KEXPORT_STR 完全一致）。
# 注意：顶层 `make kernel` 会先构建组件 .ko 再调这里；单独 `make -C kernel`
# 时如果组件还没建，导出表为空（内核能链接，但组件会在启动时 UNRESOLVED）。
# 该自动导出目前只接 x86_64（组件 .ko 本身也只在该架构构建）。
ifeq ($(KARCH),x86_64)
KO_KOUT_DIR := obj-$(KARCH)/generated
KO_KOUT_SRC := $(KO_KOUT_DIR)/component_exports.S
KO_KOUT_OBJ := $(KO_KOUT_SRC).o
KO_KOUT_SCRIPT := $(if $(SS_BUILD_DIR),$(SS_BUILD_DIR),$(abspath $(CURDIR)/..))/res/scripts/gen_component_exports.py
override OBJ += $(KO_KOUT_OBJ)
endif

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

# 共享实现库（modules/common/** 与 modules/<arch>/**，排除组件壳）编译规则：
# 源文件来自 modules 树，对象落在 obj-$(KARCH)/modules/<rel>，与 Tier0 src 对象分离。
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

# --- 构建期生成的 .kout 导出表（见 OBJ 上方的说明） ---
# 生成规则每次都跑（脚本内容不变时不重写文件），确保组件 .ko 增删后导出表
# 立即跟上；只有内容变化才会触发下面的重新汇编/重链接。
ifeq ($(KARCH),x86_64)
.PHONY: kout-force
kout-force:
$(KO_KOUT_SRC): kout-force
	@mkdir -p "$$(dirname $@)"
	python3 "$(KO_KOUT_SCRIPT)" --ko-root bin-$(KARCH)/components --out $@

$(KO_KOUT_OBJ): $(KO_KOUT_SRC) Makefile
	@echo [KOUT] $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@
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
# 不进入 OBJ / 内核链接（默认 all 目标不含 .ko，Tier1 内建组件另走 modules/<arch>）。
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
# 原 Tier1 组件壳（components/ + dosvm/）现在也编译成独立 .ko：
#   - 用 -DKMOD_COMPONENT_AS_KO=1 把 KCOMPONENT_BEGIN/END 映射成 .kmod_manifest/.in/.out；
#   - 组件实现函数仍在内核镜像内，.ko 通过 kernel.component.v1 生成导出表解析；
#   - 所有组件 .ko 与 demo .ko 一起打进一个 modules.saf，由 Limine 单文件加载。
KMOD_COMPONENT_SRCS := $(shell find modules/$(KARCH)/components -type f -name "*.cpp" 2>/dev/null | LC_ALL=C sort) \
                       $(wildcard modules/$(KARCH)/dosvm/*.cpp)
KMOD_COMPONENT_OBJS := $(patsubst modules/%.cpp,obj-$(KARCH)/component-ko/%.cxx.o,$(KMOD_COMPONENT_SRCS))
KMOD_COMPONENT_KOS := $(patsubst modules/%.cpp,bin-$(KARCH)/components/%.ko,$(KMOD_COMPONENT_SRCS))
# 已迁移到 .ko 的实现对象（由 modules/mcr/*.sources 声明）
KMOD_MOVED_C_SRCS := $(filter %.c,$(KMOD_MOVED_SRCS))
KMOD_MOVED_CXX_SRCS := $(filter %.cpp,$(KMOD_MOVED_SRCS))
KMOD_MCR_OBJS := \
    $(patsubst modules/%.cpp,obj-$(KARCH)/component-ko/extra/%.cxx.o,$(KMOD_MOVED_CXX_SRCS)) \
    $(patsubst modules/%.c,obj-$(KARCH)/component-ko/extra/%.c.o,$(KMOD_MOVED_C_SRCS))
define mcr_srcs
$(shell sed -e 's/#.*//' -e '/^[[:space:]]*$$/d' $(KMOD_MCR_DIR)/$(1).sources 2>/dev/null)
endef
# 某个组件 .ko 额外链接的实现对象
define mcr_objs
$(patsubst modules/%.cpp,obj-$(KARCH)/component-ko/extra/%.cxx.o,$(filter %.cpp,$(addprefix modules/,$(call mcr_srcs,$(1))))) \
$(patsubst modules/%.c,obj-$(KARCH)/component-ko/extra/%.c.o,$(filter %.c,$(addprefix modules/,$(call mcr_srcs,$(1)))))
endef
# .ko 对象必须依赖契约头：只改 kmod_abi.h（tier/flags/结构）时若 .ko 不重编，
# 会把旧线格式静默带进镜像。这里把全部模块 ABI 头纳入依赖。
KMOD_HEADERS := $(wildcard include/kmod/*.h) $(wildcard include/kmod/contracts/*.h)

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
    -I$(KMOD_ROOT)/kernel/src \
    -I$(KMOD_ROOT)/lib/include \
    -I$(KMOD_ROOT)/ablib \
    -isystem $(KMOD_ROOT)/ablib/freestndchdrs/$(KARCH)/include
KMOD_CFLAGS := $(filter-out -std=gnu++17,$(KMOD_CXXFLAGS)) -std=gnu17 \
    -I$(KMOD_ROOT)/kernel/fakestdlib \
    -Wno-error=implicit-function-declaration -Wno-implicit-function-declaration
KMOD_LDFLAGS := -m elf_x86_64 -r

.PHONY: modules components
modules: $(KMOD_MODULE_KOS) $(KMOD_COMPONENT_KOS) $(KMOD_MCR_OBJS)
components: $(KMOD_COMPONENT_KOS) $(KMOD_MCR_OBJS)

# 中间 .cxx.o 保留，避免每次 make modules 都全量重编（.ko 依赖它做增量）。
.SECONDARY: $(KMOD_MODULE_OBJS) $(KMOD_COMPONENT_OBJS) $(KMOD_MCR_OBJS)

obj-$(KARCH)/loadable/%.cxx.o: modules/loadable/%.cpp $(KMOD_HEADERS)
	@echo KMOD-CXX $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KMOD_CXXFLAGS) -c $< -o $@

bin-$(KARCH)/modules/%.ko: obj-$(KARCH)/loadable/%.cxx.o
	@echo KMOD-LD $@
	@mkdir -p "$$(dirname $@)"
	@$(KLD) $(KMOD_LDFLAGS) -o $@ $<

# 组件壳 .ko：同一份源码，KMOD_COMPONENT_AS_KO 把 KCOMPONENT_* 映射到模块段。
obj-$(KARCH)/component-ko/%.cxx.o: modules/%.cpp $(KMOD_HEADERS)
	@echo KMOD-COMP-CXX $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KMOD_CXXFLAGS) -DKMOD_COMPONENT_AS_KO=1 -c $< -o $@

# 迁移到组件的实现对象
obj-$(KARCH)/component-ko/extra/%.cxx.o: modules/%.cpp $(KMOD_HEADERS)
	@echo KMOD-COMP-EXTRA-CXX $<
	@mkdir -p "$$(dirname $@)"
	@$(KCXX) $(KMOD_CXXFLAGS) -DKMOD_COMPONENT_AS_KO=1 -c $< -o $@

obj-$(KARCH)/component-ko/extra/%.c.o: modules/%.c $(KMOD_HEADERS)
	@echo KMOD-COMP-EXTRA-C $<
	@mkdir -p "$$(dirname $@)"
	@$(KCC) $(KMOD_CFLAGS) -DKMOD_COMPONENT_AS_KO=1 -c $< -o $@

.SECONDEXPANSION:
bin-$(KARCH)/components/%.ko: obj-$(KARCH)/component-ko/%.cxx.o $(KMOD_MCR_FILES) $$(call mcr_objs,$$(notdir $$(basename $$@)))
	@echo KMOD-COMP-LD $@
	@mkdir -p "$$(dirname $@)"
	@extra="$(call mcr_objs,$(notdir $(basename $@)))"; \
	$(KLD) $(KMOD_LDFLAGS) -o $@ $< $$extra

# 单一内核模块归档：所有 .ko 打成一个 .saf，Limine 只声明它。
.PHONY: modules-saf
modules-saf: modules
	@echo SAF-PACK bin-$(KARCH)/modules.saf
	@rm -rf obj-$(KARCH)/saf-stage
	@mkdir -p obj-$(KARCH)/saf-stage/components
	@cp -r bin-$(KARCH)/modules/*.ko obj-$(KARCH)/saf-stage/
	@cp -r bin-$(KARCH)/components/. obj-$(KARCH)/saf-stage/components/
	@python3 $(KMOD_ROOT)/res/scripts/make_modules_saf.py obj-$(KARCH)/saf-stage bin-$(KARCH)/modules.saf

# 构建后校验：.ko 的 alloc 段重定位必须只落在加载器白名单里。
.PHONY: modules-check
modules-check: $(KMOD_MODULE_KOS) $(KMOD_COMPONENT_KOS)
	@echo "== kmod reloc check (allowed: R_X86_64_64/PC32/PLT32/32/32S) =="
	@fail=0; for ko in $(KMOD_MODULE_KOS) $(KMOD_COMPONENT_KOS); do \
	    bad=$$(readelf -rW "$$ko" | grep -oE 'R_X86_64_[A-Z0-9_]+' | \
	           grep -vE '^R_X86_64_(64|PC32|PLT32|32|32S)$$' | sort -u); \
	    if [ -n "$$bad" ]; then echo "[FAIL] $$ko: $$bad"; fail=1; \
	    else echo "[OK]   $$ko"; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "kmod reloc check FAILED"; exit 1; fi; \
	echo "kmod reloc check OK"
else
.PHONY: modules components modules-check modules-saf
modules components:
	@echo "modules/components: skipped (.ko 仅 x86_64，当前 KARCH=$(KARCH))"
modules-check:
	@echo "modules-check: skipped (.ko 仅 x86_64，当前 KARCH=$(KARCH))"
modules-saf:
	@echo "modules-saf: skipped (.ko 仅 x86_64，当前 KARCH=$(KARCH))"
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