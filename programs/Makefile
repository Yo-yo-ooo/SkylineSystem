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

ifeq ($(shell e2ls disk.img 2>/dev/null | grep -q "README.md" && echo yes),yes)
	$(info "README.md Exists")
else	
	$(shell e2cp ./README.md ../$(PROGRAM_IMAGE_NAME).img:/)
endif

ifeq ($(shell ( ! test -f ../res/font/SourceHanSerifTC_Medium.ttf ); echo $$?),0)
    wget https://github.com/Yo-yo-ooo/SkylineSystem/releases/download/font/SourceHanSerifTC_Medium.zip
	unzip SourceHanSerifTC_Medium.zip
	mv SourceHanSerifTC_Medium.ttf ../res/font
	rm -rf SourceHanSerifTC_Medium.zip
	e2cp ../res/font/SourceHanSerifTC_Medium.ttf ../$(PROGRAM_IMAGE_NAME).img:/
endif
	

clean:
	$(MAKE) -C desktop clean
	rm -rf $(PROGRAM_BUILD_DIR)