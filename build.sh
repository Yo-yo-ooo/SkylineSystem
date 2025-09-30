#qemu-img create disk.img 1000M -f qcow2
#qemu-img resize disk.img 1G

#mkfs.ext4 -O ^has_journal,extent,huge_file,flex_bg,64bit,dir_nlink,extra_isize disk.img

gcc -ffreestanding -fshort-wchar -mno-red-zone \
 -fno-omit-frame-pointer -fno-exceptions -ffunction-sections -fdata-sections -O0 -c test.c -m64 -o test.o

ld test.o --gc-sections  -entry=main -static -nostdlib\
    -m elf_x86_64 -unresolved-symbols=ignore-all -o test

e2cp test disk.img:/