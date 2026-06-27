.SUFFIXES:

PREFIX := /usr/local

.PHONY: all
all:

.PHONY: install
install:
	mkdir -p "$(DESTDIR)$(PREFIX)/lib/pkgconfig"
	mkdir -p "$(DESTDIR)$(PREFIX)/share/freestnd-c-hdrs"
	for arch in aarch64 i686 loongarch64 m68k riscv64 x86_64; do \
		cp -r $$arch "$(DESTDIR)$(PREFIX)/share/freestnd-c-hdrs/"; \
		sed "s/@architecture@/$$arch/g;s|@prefix@|$(PREFIX)|g" freestnd-c-hdrs.pc.in > "$(DESTDIR)$(PREFIX)/lib/pkgconfig/freestnd-c-hdrs-$$arch.pc"; \
	done
