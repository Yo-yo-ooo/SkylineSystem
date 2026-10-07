#!/usr/bin/env bash
# doom-qemu.sh — build SkylineSystem with the DOOM port inside WSL, then
# launch the Windows-side QEMU on the result. Run from the repo root:
#
#   cd /mnt/c/ZSY/SkylineSystem && bash res/scripts/doom-qemu.sh
#
# What it does:
#   1. sanity-check the IWAD (programs/PureDOOM/doom.wad or doom1.wad)
#   2. make cm  (kernel + lib + programs; doom.elf and the IWAD are copied
#      into disk.img by programs/Makefile)
#   3. launch Windows QEMU (qemu-system-x86_64.exe) on the fresh ISO +
#      disk.img, with the QEMU monitor on 127.0.0.1:4444 so screenshots can
#      be taken while the game runs.
set -e

echo "== [1/3] IWAD check =="
WAD=""
for f in programs/PureDOOM/doom.wad programs/PureDOOM/doom1.wad; do
    [ -f "$f" ] && WAD="$f" && break
done
if [ -z "$WAD" ]; then
    echo "ERROR: no IWAD found. Put your doom.wad / doom1.wad at programs/PureDOOM/."
    exit 1
fi
echo "IWAD: $WAD ($(du -h "$WAD" | cut -f1))"

echo "== [2/3] build (make cm) =="
if [ ! -d kernel/deps ]; then
    (cd kernel && ./get-deps)
fi
make limine-binary/limine
make cm

echo "== [3/3] launch Windows QEMU =="
QEMU="/mnt/c/Program Files/qemu/qemu-system-x86_64.exe"
[ -x "$QEMU" ] || QEMU="qemu-system-x86_64.exe"
ISO=$(wslpath -w SkylineSystem-x86_64.iso)
DISK=$(wslpath -w disk.img)
echo "ISO: $ISO"
echo "run from a Windows console for SDL output, or let it open its own window:"
"$QEMU" -machine q35 -cpu max \
    -cdrom "$ISO" -m 2G -smp 4 \
    -serial stdio \
    -netdev user,id=n0 -device e1000,netdev=n0 -device AC97 \
    -drive file="$DISK",if=none,id=drive0,format=raw \
    -device ich9-ahci,id=sata \
    -device ide-hd,drive=drive0,bus=sata.0 \
    -no-reboot --no-shutdown \
    -gdb tcp::26000 -monitor telnet:127.0.0.1:4444,server,nowait
