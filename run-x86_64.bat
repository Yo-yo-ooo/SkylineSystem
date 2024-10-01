qemu-system-x86_64 -M q35 -drive ^
if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-x86_64.fd,readonly=on ^
-drive if=pflash,unit=1,format=raw,file=ovmf/ovmf-vars-x86_64.fd  ^
-cdrom template-x86_64.iso -m 2G
pause