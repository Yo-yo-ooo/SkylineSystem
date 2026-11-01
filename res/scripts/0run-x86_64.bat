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


rem ---------------------------------------------------------------
rem 加速器: 默认 tcg —— 本机唯一能稳定进桌面的配置。
rem 实测 (QEMU 10.1 / Windows / 该 QEMU 支持 whpx):
rem   accel=whpx  -cpu max                     -> 立刻 "WHPX: Unexpected VP exit code 4"
rem   accel=whpx  -cpu qemu64                  -> 过 APIC, 但 FBBADDR 后 #GP
rem                                                 (TCG 下用 qemu64 也一样 #GP => 内核特性依赖)
rem   accel=whpx  -cpu Skylake-Client          -> 使能中断控制器时 #GP (x2APIC)
rem   accel=whpx  -cpu Skylake-Client,-x2apic  -> 卡在 AHCI 端口探测
rem 结论: whpx 跑不动这个 guest, 且吞吐与 tcg 基本持平, 不值得切。
rem 换到支持 KVM 的 Linux 主机时: set ACCEL=kvm
rem ---------------------------------------------------------------
set ACCEL=tcg

qemu-system-x86_64 -machine q35,accel=%ACCEL% -cpu max ^
-cdrom %~dp0/../../SkylineSystem-x86_64.iso -m 128M -smp 4 ^
-serial stdio ^
-netdev user,id=n0 -device e1000,netdev=n0 -device AC97 ^
-drive file=%SourceFile%,if=none,id=drive0,format=raw ^
-device ich9-ahci,id=sata ^
-device ide-hd,drive=drive0,bus=sata.0 ^
-no-reboot --no-shutdown ^
-monitor telnet:127.0.0.1:4444,server,nowait
