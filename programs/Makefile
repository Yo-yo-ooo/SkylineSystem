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

	@# 共享库 (动态链接): 内核从 /mp/libsky.so 加载, 所以必须进盘。
	@# 缺失时不影响现有静态链接的程序, 只提示一句。
	@if [ -f ../lib/bin/libsky.so ]; then \
	    e2cp ../lib/bin/libsky.so $(DISK_IMG):/ && echo "libsky.so -> image"; \
	else \
	    echo "(libsky.so not built yet - run 'make -C lib so' for dynamic linking)"; \
	fi

	@# 动态链接版桌面 (desktop.dyn.elf)。内核目前按固定路径 /mp/desktop.elf
	@# 加载, 所以动态版本要"顶替"这个名字 —— 保留一份静态副本便于回退。
	@# 验收要点: 体积从 ~1.04MB 降到 ~245KB, 且启动日志出现 [dynlink] 行。
	@if [ -f $(PROGRAM_BUILD_DIR)/desktop.dyn.elf ]; then \
	    e2cp $(PROGRAM_BUILD_DIR)/desktop.dyn.elf $(DISK_IMG):/desktop.elf && \
	    echo "desktop.dyn.elf -> disk.img:/desktop.elf (DYNAMIC desktop in image)"; \
	fi

	@# DOOM IWAD —— 不随仓库分发, 使用者自备:
	@#   放在 programs/PureDOOM/doom.wad (完整版) 或 doom1.wad (共享版),
	@#   构建时拷进镜像; 缺失则 doom.elf 仍入盘, 但桌面不会 spawn DOOM 窗口。
	@WAD=""; \
	for f in ./PureDOOM/doom.wad ./PureDOOM/doom1.wad; do \
	    [ -f "$$f" ] && WAD="$$f" && break; \
	done; \
	if [ -n "$$WAD" ]; then \
	    NAME=`basename "$$WAD"`; \
	    if e2ls $(DISK_IMG) 2>/dev/null | grep -q "$$NAME"; then \
	        echo "$$NAME Exists in image"; \
	    else \
	        e2cp "$$WAD" $(DISK_IMG):/; \
	        echo "copied $$NAME into image"; \
	    fi \
	else \
	    echo "no IWAD provided (doom.wad/doom1.wad) - DOOM window will not spawn"; \
	fi
	@# 自检: 打印镜像内 wad 的大小, 和本地对比 (大小不符说明镜像/ext4 有脏数据)
	@e2ls -l $(DISK_IMG):/ 2>/dev/null | grep -i "\.wad" || echo "(no wad in image)"
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