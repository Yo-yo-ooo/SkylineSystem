@echo off

qemu-system-%1 -M q35 ^
-cdrom SkylineSystem-%1.iso -m 2G -smp 4

pause