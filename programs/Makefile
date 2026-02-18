include ../gdef.mk

PROGRAM_BUILD_DIR = ./bin

.PHONY: all
all: 
	@echo "Building all programs in $(BUILD_ARCH) architecture"
	@mkdir -p $(PROGRAM_BUILD_DIR)
	$(MAKE) -C syscalltest
	$(MAKE) -C iamgod
	
	@echo "Copying programs to disk image..."
	../saf/saf-make ./bin ./programs.saf -q
	e2cp $(PROGRAM_BUILD_DIR)/syscalltest.elf ../$(PROGRAM_IMAGE_NAME).img:/

clean:
	$(MAKE) -C syscalltest clean
	rm -rf $(PROGRAM_BUILD_DIR)