@echo off
REM Run No pause command

qemu-system-%1 -M q35 ^
-cdrom SkylineSystem-%1.iso -m 2G