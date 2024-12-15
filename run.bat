@echo off

SET SourceFile=disk.img

if not exist %SourceFile% (
    qemu-img create disk.img 1M
) else (
    echo %SourceFile% is exist, Stop creating disk.img
)

qemu-system-%1 -M q35 -cpu qemu64 ^
-cdrom SkylineSystem-%1.iso -m 2G -smp 4 ^
-serial stdio -net nic -device AC97

pause