# Architecture to build for SkylineSystem
# You must set in thest architecture: x86_64, aarch64, riscv64, loongarch64

BUILD_ARCH = x86_64
PROGRAM_IMAGE_NAME = disk
HAS_SSE_4_2 := $(shell grep -o 'sse4_2' /proc/cpuinfo | wc -l)
HAS_AVX := $(shell grep -o 'avx' /proc/cpuinfo | wc -l)
HAS_AVX2 := $(shell grep -o 'avx2' /proc/cpuinfo | wc -l)
HAS_AVX_512 := $(shell grep -o 'avx512f' /proc/cpuinfo | wc -l)

COMPILER_SUPPORT_SSE_4_2 := $(shell gcc -msse4.2 -o /dev/null $(SS_BUILD_DIR)/scripts/cputest/x86_64/sse4_2.c > /dev/null 2>&1 && echo "yes" || echo "no";)
COMPILER_SUPPORT_AVX := $(shell gcc -mavx -o /dev/null $(SS_BUILD_DIR)/scripts/cputest/x86_64/avx.c > /dev/null 2>&1 && echo "yes" || echo "no";)
COMPILER_SUPPORT_AVX2 := $(shell gcc -mavx -o /dev/null $(SS_BUILD_DIR)/scripts/cputest/x86_64/avx2.c > /dev/null 2>&1 && echo "yes" || echo "no";)
COMPILER_SUPPORT_AVX512 := $(shell gcc -mavx -o /dev/null $(SS_BUILD_DIR)/scripts/cputest/x86_64/avx512f.c > /dev/null 2>&1 && echo "yes" || echo "no";)

# 1:TRUE
# 0:FALSE
NOT_COMPILE_X86MEM := 1
ifeq ($(BUILD_ARCH),x86_64)
ifneq ($(HAS_SSE_4_2),0)
ifeq ($(COMPILER_SUPPORT_SSE_4_2),yes)
$(info This Env Can Compile x86mem library)
NOT_COMPILE_X86MEM := 0
endif
endif
endif

$(info NOT_COMPILE_X86MEM = $(NOT_COMPILE_X86MEM))