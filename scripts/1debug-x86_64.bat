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
-cdrom %~dp0/../SkylineSystem-x86_64.iso -m 2G -smp 4 ^
-serial stdio -net nic -device AC97 ^
-drive file=%SourceFile%,if=none,id=drive0 ^
-device ide-hd,drive=drive0,bus=ide.0 ^
-no-reboot --no-shutdown ^
-gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait -S
