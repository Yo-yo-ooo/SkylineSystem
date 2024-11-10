REM Run No pause command
@echo off

qemu-system-%1 -M q35 ^
-cdrom SkylineSystem-%1.iso -m 2G