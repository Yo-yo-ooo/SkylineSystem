# SkylineSystem

> [!CAUTION]
> Don't run it in real machine because it's now in test

## How to build

> [!IMPORTANT]
> Make sure you have install these software in linux
> * gcc (VER > 10)
> * binutil
> * xorriso
> * make
> * e2cp
>
> Run with this command in the project root dir

## Build Template(Not applicable to x86_64)
> [!CAUTION]
> You must edit the 'BUILD_ARCH' variable in 'gdef.mk' file 
> to set the architecture you want to build for.
```bash
make cm KCC=(XXX arch)-linux-gnu-gcc KCXX=(XXX arch)-linux-gnu-g++ KLD=(XXX arch)-linux-gnu-ld
```
For example, to build aarch64 arch, run:
```bash
make cm KCC=aarch64-linux-gnu-gcc KCXX=aarch64-linux-gnu-g++ KLD=aarch64-linux-gnu-ld
```

> [!TIP]
> **You can build this project(x86_64 arch) with these commands:**
> ```bash
> cd kernel && ./get-deps
> cd .. && make limine/limine
> #If can't run get-deps script, you can run "chmod +x kernel/get-deps"
> make cm
> ```
> **You can run qemu with these commands:**
> ```bash
> # just run x86_64 qemu example command
> qemu-system-x86_64 -machine q35 -cpu qemu64,+x2apic,+avx \
> -cdrom SkylineSystem-x86_64.iso -m 2G -smp 4 \
> -serial stdio -net nic -device AC97 \
> -drive file=disk.img,if=none,id=sata1 \
> -device ahci,id=ahci1 \
> -device ide-hd,drive=sata1,bus=ahci1.0 \
> -no-reboot --no-shutdown \
> -gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait
> ```
> **You can debaug with gdb**
> ```bash
> #first run qemu
> # just run x86_64 qemu example command
> qemu-system-x86_64 -machine q35 -cpu qemu64,+x2apic,+avx \
> -cdrom SkylineSystem-x86_64.iso -m 2G -smp 4 \
> -serial stdio -net nic -device AC97 \
> -drive file=disk.img,if=none,id=sata1 \
> -device ahci,id=ahci1 \
> -device ide-hd,drive=sata1,bus=ahci1.0 \
> -no-reboot --no-shutdown \
> -gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait -S
> #run gdb on kernel folder
> cd kernel && gdb
> ```
> ```bash
> #and enter these command in gdb
> target remote :26000
> file kernel
> #do like you what you want to do than
> ```

## Features(x86_64)

* Devices
  * AHCI
  * IDE
  * ACPI
  * IOAPIC
  * LAPIC
* Basics
  * Syscall
  * Scheduler
  * Output
  * SMP
  * Virtual Memory
  * Process & thread management
* Disk Interfaces
  * Sata
  * Ram

* File Systems Support
  * Fat12
  * Fat16
  * Fat32
  * Exfat
  * ext2/3/4

* Dynamic File System Type Check(get_fs_type!)

Many more in future

## Thanks to

* [MaslOS](https://github.com/marceldobehere/MaslOS)
* [VisualOS](https://github.com/nothotscott/VisualOS)
* [MicroOS](https://github.com/Glowman554/MicroOS)
* [MaslOS-2](https://github.com/marceldobehere/MaslOS-2/)
* [HanOS](https://github.com/jjwang/HanOS/)
* [SAF](https://github.com/chocabloc/saf)

## Unfinished features

* PCI MSI/MSIX
* NVME
* VFS

## Contributors

* [Yo-yo-ooo](https://github.com/Yo-yo-ooo/)
* [marceldobehere](https://github.com/marceldobehere)

[//]: (https://github.com/asterd-og/)

## Connect

**You can conect with e-mail <1218849168@qq.com>** 

