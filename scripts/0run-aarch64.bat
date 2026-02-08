qemu-system-aarch64 ^
-M virt ^
-cpu cortex-a72 ^
-device ramfb ^
-device qemu-xhci ^
-device usb-kbd ^
-device usb-mouse ^
-cdrom %~dp0/../SkylineSystem-aarch64.iso ^
-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-aarch64.fd,readonly=on ^
-m 2G