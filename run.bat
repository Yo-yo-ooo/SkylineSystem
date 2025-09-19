@echo off

SET SourceFile=disk.img

if not exist %SourceFile% (
    qemu-img create %SourceFile% 1000M -f qcow2
    qemu-img resize %SourceFile% 1G
    wsl -e mkfs.ext4 %SourceFile%
    wsl -e e2cp -p test %SourceFile%:/
) else (
    echo %SourceFile% is exist, Stop create %SourceFile%
)

qemu-system-%1 -machine q35 -cpu qemu64,+x2apic,+avx ^
-cdrom SkylineSystem-%1.iso -m 2G -smp 4 ^
-serial stdio -net nic -device AC97 ^
-drive file=%SourceFile%,if=none,id=sata1 ^
-device ahci,id=ahci1 ^
-device ide-hd,drive=sata1,bus=ahci1.0 ^
-no-reboot --no-shutdown ^
-gdb tcp::26000 
