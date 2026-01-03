
# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:

include ./gdef.mk

# Convenience macro to reliably declare user overridable variables.
override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

# Get Path of build directory
override SS_BUILD_DIR := $(shell pwd)
export SS_BUILD_DIR

# Target architecture to build for. Default to x86_64.
# $(call USER_VARIABLE,KARCH,x86_64)
KARCH := $(BUILD_ARCH)
# Default user QEMU flags. These are appended to the QEMU command calls.
$(call USER_VARIABLE,QEMUFLAGS,-m 2G)

override IMAGE_NAME := SkylineSystem-$(KARCH)

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

.PHONY: run
run: run-$(KARCH)

.PHONY: run-hdd
run-hdd: run-hdd-$(KARCH)

.PHONY: run-x86_64
run-x86_64: edk2-ovmf $(IMAGE_NAME).iso
	qemu-system-$(KARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMUFLAGS)

.PHONY: run-hdd-x86_64
run-hdd-x86_64: edk2-ovmf $(IMAGE_NAME).hdd
	qemu-system-$(KARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-aarch64
run-aarch64: edk2-ovmf $(IMAGE_NAME).iso
	qemu-system-$(KARCH) \
		-M virt \
		-cpu cortex-a72 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-aarch64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMUFLAGS)

.PHONY: run-hdd-aarch64
run-hdd-aarch64: edk2-ovmf $(IMAGE_NAME).hdd
	qemu-system-$(KARCH) \
		-M virt \
		-cpu cortex-a72 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-aarch64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-riscv64
run-riscv64: edk2-ovmf $(IMAGE_NAME).iso
	qemu-system-$(KARCH) \
		-M virt \
		-cpu rv64 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-riscv64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMUFLAGS)

.PHONY: run-hdd-riscv64
run-hdd-riscv64: edk2-ovmf $(IMAGE_NAME).hdd
	qemu-system-$(KARCH) \
		-M virt \
		-cpu rv64 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-riscv64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-loongarch64
run-loongarch64: edk2-ovmf $(IMAGE_NAME).iso
	qemu-system-$(KARCH) \
		-M virt \
		-cpu la464 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-loongarch64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMUFLAGS)

.PHONY: run-hdd-loongarch64
run-hdd-loongarch64: edk2-ovmf $(IMAGE_NAME).hdd
	qemu-system-$(KARCH) \
		-M virt \
		-cpu la464 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-loongarch64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)


.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso
	qemu-system-$(KARCH) \
		-M q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd-bios
run-hdd-bios: $(IMAGE_NAME).hdd
	qemu-system-$(KARCH) \
		-M q35 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine/limine:
	rm -rf limine
	git clone https://github.com/limine-bootloader/limine.git --branch=v10.x-binary --depth=1
	$(MAKE) -C limine
	git clone https://github.com/limine-bootloader/limine-protocol
	cp -f ./limine-protocol/include/limine.h ./kernel/src

kernel-deps:
	chmod +x ./kernel/get-deps
	./kernel/get-deps
	touch kernel-deps

.PHONY: kernel
kernel: kernel-deps
	@echo "Start Building Kernel!"
ifeq ($(NOT_COMPILE_X86MEM),0)
	$(MAKE) -C x86mem
endif
	$(MAKE) -C kernel

$(IMAGE_NAME).iso: limine/limine kernel
	@rm -rf iso_root
	@mkdir -p iso_root/boot
	@cp -v kernel/bin-$(KARCH)/kernel iso_root/boot/
	@mkdir -p iso_root/boot/limine
	@cp -v limine.conf iso_root/boot/limine/
	@mkdir -p iso_root/EFI/BOOT
ifeq ($(KARCH),x86_64)
	@cp -v limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	@cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	@cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	@cp -v programs/programs.saf iso_root/boot/
	@xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
endif
ifeq ($(KARCH),aarch64)
	@cp -v limine/limine-uefi-cd.bin iso_root/boot/limine/
	@cp -v limine/BOOTAA64.EFI iso_root/EFI/BOOT/
	@xorriso -as mkisofs \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
endif
ifeq ($(KARCH),riscv64)
	@cp -v limine/limine-uefi-cd.bin iso_root/boot/limine/
	@cp -v limine/BOOTRISCV64.EFI iso_root/EFI/BOOT/
	@xorriso -as mkisofs \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
endif
ifeq ($(KARCH),loongarch64)
	@cp -v limine/limine-uefi-cd.bin iso_root/boot/limine/
	@cp -v limine/BOOTLOONGARCH64.EFI iso_root/EFI/BOOT/
	@xorriso -as mkisofs \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
endif
	@rm -rf iso_root

$(IMAGE_NAME).hdd: limine/limine kernel
	@rm -f $(IMAGE_NAME).hdd
	@dd if=/dev/zero bs=1M count=0 seek=64 of=$(IMAGE_NAME).hdd
	@sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00
ifeq ($(KARCH),x86_64)
	./limine/limine bios-install $(IMAGE_NAME).hdd
endif
	@mformat -i $(IMAGE_NAME).hdd@@1M
	@mmd -i $(IMAGE_NAME).hdd@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	@mcopy -i $(IMAGE_NAME).hdd@@1M kernel/bin-$(KARCH)/kernel ::/boot
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine.conf ::/boot/limine
ifeq ($(KARCH),x86_64)
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine/limine-bios.sys ::/boot/limine
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTX64.EFI ::/EFI/BOOT
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTIA32.EFI ::/EFI/BOOT
endif
ifeq ($(KARCH),aarch64)
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTAA64.EFI ::/EFI/BOOT
endif
ifeq ($(KARCH),riscv64)
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTRISCV64.EFI ::/EFI/BOOT
endif
ifeq ($(KARCH),loongarch64)
	@mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTLOONGARCH64.EFI ::/EFI/BOOT
endif

.PHONY: clean
clean:
	@$(MAKE) -C x86mem clean
	@$(MAKE) -C kernel clean
	@$(MAKE) -C programs clean
	@$(MAKE) -C saf clean
	rm -rf iso_root *.iso *.hdd

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd kernel-deps limine ovmf
	rm -rf limine-protocol

cm:
	@$(MAKE) clean 
	@$(MAKE) -C saf
	@$(MAKE) -j$(shell nproc)
ifeq ($(KARCH),x86_64)
	@cp -f kernel/bin-$(KARCH)/$(OUTPUT)/kernel kernel
	@qemu-img create $(PROGRAM_IMAGE_NAME).img 1000M -f qcow2
	@qemu-img resize $(PROGRAM_IMAGE_NAME).img 2G
	dd if=/dev/zero of=$(PROGRAM_IMAGE_NAME).img bs=1G count=2
	@mkfs.ext4 \
	-O ^metadata_csum \
	./$(PROGRAM_IMAGE_NAME).img
	@$(MAKE) -C programs
endif

mk:
	$(MAKE) -j$(shell nproc)
	cp -f kernel/bin-$(KARCH)/$(OUTPUT)/kernel kernel
	
cmk:
	@$(MAKE) clean
	@$(MAKE) -j$(shell nproc)

cmr:
	@$(MAKE) cm
	@$(MAKE) run