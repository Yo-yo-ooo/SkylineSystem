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
	@# 动态链接现在是**默认**产物, 所以缺这个库就是硬错误 —— 早点报出来,
	@# 别等到程序链接时才莫名其妙地 "cannot find -l:libsky.so"。
	@if [ -f ../lib/bin/libsky.so ]; then \
	    e2cp ../lib/bin/libsky.so $(DISK_IMG):/ && echo "libsky.so -> image"; \
	else \
	    echo "ERROR: ../lib/bin/libsky.so 不存在 —— 先 'make -C lib' (或直接 'make cm')"; \
	    exit 1; \
	fi

	@# 自检: 镜像里的 /mp/desktop.elf 必须是动态链接版 (NEEDED libsky.so)。
	@# 内核按固定路径加载它, 混淆静态/动态会让"改了半天没生效"这种问题重现。
	@if [ -f $(DISK_IMG) ]; then \
	    if e2cp $(DISK_IMG):/desktop.elf /tmp/_desk_check.elf >/dev/null 2>&1; then \
	        if readelf -d /tmp/_desk_check.elf 2>/dev/null | grep -q 'libsky.so'; then \
	            echo "image desktop.elf: DYNAMIC (NEEDED libsky.so) OK"; \
	        else \
	            echo "warning: 镜像里的 desktop.elf 不是动态链接版"; \
	        fi; \
	    fi; \
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

	@# 字体是**可选资源**: res/font 不入库 (*.ttf 在 .gitignore 里), 由使用者自备。
	@# 拿不到字体时只告警、绝不中断构建 —— 桌面对字体做了判空
	@# (console_font() 可能返回 NULL, 所有调用点都有 `if (f)` 保护, 只是不画
	@# 文字), 所以缺字体不影响启动。以前这里失败会 exit 1, 在离线环境里
	@# `make cm` 必然失败, 而且报错点离真正的原因 (没有网络) 很远。
	@if [ ! -f "$(FONT_FILE)" ]; then \
	    echo "Font not found locally, trying to download..."; \
	    if wget -q https://github.com/Yo-yo-ooo/SkylineSystem/releases/download/font/SourceHanSerifTC_Medium.zip -O /tmp/font.zip 2>/dev/null \
	       && [ -s /tmp/font.zip ]; then \
	        mkdir -p ../res/font; \
	        if unzip -o -q /tmp/font.zip -d ../res/font/ 2>/dev/null; then \
	            echo "font downloaded -> ../res/font"; \
	        else \
	            echo "warning: 字体压缩包解压失败"; \
	        fi; \
	        rm -f /tmp/font.zip; \
	    else \
	        rm -f /tmp/font.zip; \
	        echo "warning: 字体 SourceHanSerifTC_Medium.ttf 缺失且下载失败 (通常是没网)。"; \
	        echo "         界面文字不会渲染, 但桌面已判空, 不影响启动。"; \
	        echo "         需要文字时把该 ttf 放到 res/font/ 后重跑 make -C programs。"; \
	    fi; \
	fi

	@# 镜像里已有就跳过; 本地有就拷进去; 都没有则明确跳过 (不报错)
	@if e2ls $(DISK_IMG) 2>/dev/null | grep -q "SourceHanSerifTC_Medium.ttf"; then \
	    echo "SourceHanSerifTC_Medium.ttf Exists in image"; \
	elif [ -f "$(FONT_FILE)" ]; then \
	    e2cp $(FONT_FILE) $(DISK_IMG):/ && echo "font -> image"; \
	else \
	    echo "(font 既不在镜像里也不在本地 - 跳过, 不视为错误)"; \
	fi

clean:
	$(MAKE) -C desktop clean
	$(MAKE) -C PureDOOM clean
	
	rm -rf $(PROGRAM_BUILD_DIR)