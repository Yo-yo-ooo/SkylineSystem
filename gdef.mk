# Architecture to build for SkylineSystem
# You must set in thest architecture: x86_64, aarch64, riscv64, loongarch64

BUILD_ARCH = aarch64
PROGRAM_IMAGE_NAME = disk

# 1:TRUE
# 0:FALSE
ifeq ($(BUILD_ARCH),x86_64)
# You can change it but can't change in else
NOT_COMPILE_X86MEM = 1
else
NOT_COMPILE_X86MEM = 1
endif