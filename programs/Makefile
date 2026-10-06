include ../gdef.mk

PROGRAM_BUILD_DIR = ./bin
DISK_IMG = ../$(PROGRAM_IMAGE_NAME).img
FONT_FILE = ../res/font/SourceHanSerifTC_Medium.ttf

.PHONY: all clean

all: 
	@echo "Building all programs in $(BUILD_ARCH) architecture"
	@mkdir -p $(PROGRAM_BUILD_DIR)
	$(MAKE) -C desktop
	$(MAKE) -C helloworld
	$(MAKE) -C helloworld2
	$(MAKE) -C notepad
	$(MAKE) -C PureDOOM
    
	@echo "Copying programs to disk image..."
	../res/saf/saf-make ./bin ./programs.saf -q
	e2cp $(PROGRAM_BUILD_DIR)/desktop.elf $(DISK_IMG):/
	e2cp $(PROGRAM_BUILD_DIR)/hw.elf $(DISK_IMG):/
	e2cp $(PROGRAM_BUILD_DIR)/hw2.elf $(DISK_IMG):/
	e2cp $(PROGRAM_BUILD_DIR)/notepad.elf $(DISK_IMG):/
	e2cp $(PROGRAM_BUILD_DIR)/doom.elf $(DISK_IMG):/

	@# DOOM IWAD (shareware doom1.wad, ~4 MB) —— 不随仓库分发, 使用者自备:
	@#   放在 programs/PureDOOM/doom1.wad, 构建时拷进镜像; 缺失则
	@#   doom.elf 仍会入盘, 但桌面不会spawn DOOM 窗口。
	@if [ -f "./PureDOOM/doom1.wad" ]; then \
	    if e2ls $(DISK_IMG) 2>/dev/null | grep -q "doom1.wad"; then \
	        echo "doom1.wad Exists in image"; \
	    else \
	        e2cp ./PureDOOM/doom1.wad $(DISK_IMG):/; \
	    fi \
	else \
	    echo "doom1.wad not provided - DOOM window will not spawn"; \
	fi

	@if e2ls $(DISK_IMG) 2>/dev/null | grep -q "README.md"; then \
	    echo "README.md Exists"; \
	else \
	    e2cp ./README.md $(DISK_IMG):/; \
	fi

	@if [ ! -f "$(FONT_FILE)" ]; then \
	    echo "Font not found locally, downloading..."; \
        wget -q https://github.com/Yo-yo-ooo/SkylineSystem/releases/download/font/SourceHanSerifTC_Medium.zip -O /tmp/font.zip; \
        if [ $$? -ne 0 ]; then echo "Failed to download font!"; exit 1; fi; \
        mkdir -p ../res/font; \
        unzip -o -q /tmp/font.zip -d ../res/font/; \
        rm -f /tmp/font.zip; \
	fi

	@# 检查镜像中是否已有字体，没有则拷贝
	@if e2ls $(DISK_IMG) 2>/dev/null | grep -q "SourceHanSerifTC_Medium.ttf"; then \
        echo "SourceHanSerifTC_Medium.ttf Exists in image"; \
	else \
	    e2cp $(FONT_FILE) $(DISK_IMG):/; \
	fi

clean:
	$(MAKE) -C desktop clean
	$(MAKE) -C PureDOOM clean
	
	rm -rf $(PROGRAM_BUILD_DIR)