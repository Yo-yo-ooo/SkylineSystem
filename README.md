```text
  /$$$$$$  /$$                 /$$ /$$                      /$$$$$$                        /$$
 /$$__  $$| $$                | $$|__/                     /$$__  $$                      | $$
| $$  \__/| $$   /$$ /$$   /$$| $$ /$$ /$$$$$$$   /$$$$$$ | $$  \__/ /$$   /$$  /$$$$$$$ /$$$$$$    /$$$$$$  /$$$$$$/$$$$
|  $$$$$$ | $$  /$$/| $$  | $$| $$| $$| $$__  $$ /$$__  $$|  $$$$$$ | $$  | $$ /$$_____/|_  $$_/   /$$__  $$| $$_  $$_  $$
 \____  $$| $$$$$$/ | $$  | $$| $$| $$| $$  \ $$| $$$$$$$$ \____  $$| $$  | $$|  $$$$$$   | $$    | $$$$$$$$| $$ \ $$ \ $$
 /$$  \ $$| $$_  $$ | $$  | $$| $$| $$| $$  | $$| $$_____/ /$$  \ $$| $$  | $$ \____  $$  | $$ /$$| $$_____/| $$ | $$ | $$
|  $$$$$$/| $$ \  $$|  $$$$$$$| $$| $$| $$  | $$|  $$$$$$$|  $$$$$$/|  $$$$$$$ /$$$$$$$/  |  $$$$/|  $$$$$$$| $$ | $$ | $$
 \______/ |__/  \__/ \____  $$|__/|__/|__/  |__/ \_______/ \______/  \____  $$|_______/    \___/   \_______/|__/ |__/ |__/
                     /$$  | $$                                       /$$  | $$
                    |  $$$$$$/                                      |  $$$$$$/
                     \______/                                        \______/
```
## License
[![GPL-2.0 Kernel](https://img.shields.io/badge/Kernel-GPLv2--only-red)](./LICENSES/GPL-2.0-only)
[![MIT Userspace](https://img.shields.io/badge/Userspace-MIT-green)](./LICENSES/MIT.txt)
[![License: GPL v3.0 w/RLE](https://img.shields.io/badge/ablib_freestndchdrs-GPLv3%20w%2F%20RLE-blue)](https://www.gnu.org/licenses/gcc-exception-3.1.html)

* **`kernel/`**: GPL-2.0-only
* **`lib/` `programs/` `ablib/atomic/`**: MIT License
Kernel and userspace run in separate address spaces, communicate only via syscall, no GPL copyleft infection.
Full compliance with REUSE standard, see root LICENSES for full statement.
* **`/ablib/freestndchdrs/`**: This directory is licensed under the **GNU General Public License v3.0 (GPLv3)**, supplemented with the **GCC Runtime Library Exception 3.1**. 
    *   *What this means:* You may link this library into your proprietary/closed-source application without being required to release your own source code. 
    *   Please refer to `COPYING.RUNTIME` within that folder for full details.

> [!CAUTION]
> Don't run it in real machine because it's now in test
> If Build Failed, please open an issue or contact me
> The General Build Faild reason is you don't have install the required dependencies <br>
> [OR] You are building for unsupported architecture <br>
## How to build

> [!IMPORTANT]
> Make sure you have install these software in linux
> * gcc (VER > 10)
> * binutil
> * xorriso
> * make
> * e2cp (ext2/3/4 tool)
> Run with this command in the project root dir

**You can build this project(x86_64 arch) with these commands:**
```bash
cd kernel && ./get-deps
cd .. && make limine-binary/limine
#If can't run get-deps script, you can run "chmod +x kernel/get-deps"
make cm
```
## Build Template(Not applicable to x86_64)
> [!CAUTION]
> You must edit the 'BUILD_ARCH' variable in 'gdef.mk' file 
> to set the architecture you want to build for.

For example if you want to build aarch64 OS,In gdef.mk you must change:
```patch
- # BUILD_ARCH = x86_64
+ BUILD_ARCH = aarch64
```
```bash
make cm KCC=(XXX arch)-linux-gnu-gcc KCXX=(XXX arch)-linux-gnu-g++ KLD=(XXX arch)-linux-gnu-ld
```
For example, to build aarch64 arch, run:
```bash
make cm KCC=aarch64-linux-gnu-gcc KCXX=aarch64-linux-gnu-g++ KLD=aarch64-linux-gnu-ld
```

## Run
### In Linux:
```bash
# just run x86_64 qemu example command
qemu-system-x86_64 -machine q35 -cpu max \
-cdrom ./SkylineSystem-x86_64.iso -m 2G -smp 4 \
-serial stdio -net nic -device AC97 \
-drive file=disk.img,if=none,id=drive0 \
-device ide-hd,drive=drive0,bus=ide.0 \
-no-reboot --no-shutdown \
-gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait 
```
### In WSL(Windows Subsystem for Linux):
see ./res/scripts/ folder run the arch you need to run
## Debug
```bash
#first run qemu
# just run x86_64 qemu example command
qemu-system-x86_64 -machine q35 -cpu max \
-cdrom ./SkylineSystem-x86_64.iso -m 2G -smp 4 \
-serial stdio -net nic -device AC97 \
-drive file=disk.img,if=none,id=drive0 \
-device ide-hd,drive=drive0,bus=ide.0 \
-no-reboot --no-shutdown \
-gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait -S
#run gdb on kernel folder
cd kernel && gdb
```
In GDB:
```bash
#and enter these command in gdb
target remote :26000
file kernel
#do like you what you want to do than
```


## Thanks to

* [MaslOS](https://github.com/marceldobehere/MaslOS)
* [VisualOS](https://github.com/nothotscott/VisualOS)
* [MicroOS](https://github.com/Glowman554/MicroOS)
* [MaslOS-2](https://github.com/marceldobehere/MaslOS-2/)
* [HanOS](https://github.com/jjwang/HanOS/)
* [SAF](https://github.com/chocabloc/saf)


## Main Contributors

* [Yo-yo-ooo](https://github.com/Yo-yo-ooo/)
* [marceldobehere](https://github.com/marceldobehere)
* [Arty3](https://github.com/Arty3)
* 人造人(qq:1440332527)


## Connect

**You can connect with e-mail <1218849168@qq.com>** 
