@echo off

cls

SET SourceFile=disk.img

if not exist %SourceFile% (
    qemu-img create %SourceFile% 1000M -f qcow2
    qemu-img resize %SourceFile% 1G
    wsl -e mkfs.ext4 %SourceFile%
    wsl -e e2cp -p test %SourceFile%:/
) else (
    echo %SourceFile% is exist, Stop create %SourceFile%
)


qemu-system-x86_64 -machine q35 -cpu max ^
-cdrom %~dp0/../../SkylineSystem-x86_64.iso -m 2G -smp 4 ^
-serial stdio ^
-netdev user,id=n0 -device e1000,netdev=n0 -device AC97 ^
-drive file=%SourceFile%,if=none,id=drive0,format=raw ^
-device ich9-ahci,id=sata ^
-device ide-hd,drive=drive0,bus=sata.0 ^
-no-reboot --no-shutdown ^
-gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait
