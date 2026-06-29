include ../gdef.mk

PROGRAM_BUILD_DIR = ./bin

.PHONY: all
all: 
	@echo "Building all programs in $(BUILD_ARCH) architecture"
	@mkdir -p $(PROGRAM_BUILD_DIR)
	$(MAKE) -C desktop
	
	@echo "Copying programs to disk image..."
	../res/saf/saf-make ./bin ./programs.saf -q
	e2cp $(PROGRAM_BUILD_DIR)/desktop.elf ../$(PROGRAM_IMAGE_NAME).img:/
	e2cp ./README.md ../$(PROGRAM_IMAGE_NAME).img:/

clean:
	$(MAKE) -C desktop clean
	rm -rf $(PROGRAM_BUILD_DIR)