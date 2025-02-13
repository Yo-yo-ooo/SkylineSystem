@echo off

SET SourceFile=disk.img

if not exist %SourceFile% (
    qemu-img create %SourceFile% 1000M -f qcow2
) else (
    echo %SourceFile% is exist, Stop creating disk.img
)

qemu-system-%1 -machine q35 -cpu qemu64 ^
-cdrom SkylineSystem-%1.iso -m 2G -smp 4 ^
-serial stdio -net nic -device AC97 ^
-drive file=%SourceFile%,if=none,id=sata1 ^
-device ahci,id=ahci1 ^
-device ide-hd,drive=sata1,bus=ahci1.0 ^
-no-reboot --no-shutdown ^
-gdb tcp::26000


pause