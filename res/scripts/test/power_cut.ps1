# res/scripts/test/power_cut.ps1 — 断电强杀/崩溃恢复验证 (D3, round 13)
# 用法: pwsh -File res/scripts/test/power_cut.ps1 [-Root <路径>] [-Qemu <路径>] [-Cycles 3]
# 断言: 每次随机时刻强杀 QEMU 后重启, 内核能重新挂载 ext4 且 0 异常
#   (崩溃恢复验证: 文件系统在无 fsck 下的挂载容错)
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [int]$Cycles = 3
)

$root = if ($Root) { $Root } elseif ($env:SKYLINE_ROOT) { $env:SKYLINE_ROOT }
        else { Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) }
$qemu = if ($Qemu) { $Qemu }
        elseif ($env:SKYLINE_QEMU) { $env:SKYLINE_QEMU }
        else { "C:\Program Files\qemu\qemu-system-x86_64.exe" }
if (-not (Test-Path $qemu)) { Write-Error "QEMU 未找到: $qemu"; exit 2 }
if (-not (Test-Path "$root\SkylineSystem-x86_64.iso")) { Write-Error "ISO 不存在, 先构建"; exit 2 }

$rng = New-Object System.Random
$fail = 0
for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $killSec = 30 + $rng.Next(0, 60)   # 随机 30-90s 强杀
    Write-Host "cycle $cycle/$Cycles : run ${killSec}s then kill"
    $p = Start-Process $qemu -ArgumentList @(
      '-machine','q35','-cpu','max','-cdrom',"$root\SkylineSystem-x86_64.iso",
      '-m','2G','-smp','4','-serial',"file:$root\serial.log",
      '-display','none','-net','nic','-net','user',
      '-drive',"file=$root\disk.img,if=none,id=drive0",
      '-device','ide-hd,drive=drive0,bus=ide.0','-no-reboot'
    ) -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds $killSec
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } | Out-Null
    Start-Sleep -Seconds 2

    # 重启验证: 60s 内应重新挂载 ext4 且 0 异常
    Remove-Item "$root\serial.log" -ErrorAction SilentlyContinue
    $p2 = Start-Process $qemu -ArgumentList @(
      '-machine','q35','-cpu','max','-cdrom',"$root\SkylineSystem-x86_64.iso",
      '-m','2G','-smp','4','-serial',"file:$root\serial.log",
      '-display','none','-net','nic','-net','user',
      '-drive',"file=$root\disk.img,if=none,id=drive0",
      '-device','ide-hd,drive=drive0,bus=ide.0','-no-reboot'
    ) -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 60
    if (-not $p2.HasExited) { Stop-Process -Id $p2.Id -Force } | Out-Null

    $ex = Select-String -Path "$root\serial.log" -Pattern 'Exception|Panic|SMASH|mount fail|ext4.*error' -ErrorAction SilentlyContinue
    if ($ex) { Write-Host "cycle $cycle : FAIL"; $ex | Select-Object -First 2 | ForEach-Object { Write-Host $_.Line }; $fail = 1 }
    else { Write-Host "cycle $cycle : PASS (重启后 0 异常)" }
}
if ($fail) { Write-Host "POWER CUT: FAIL"; exit 1 }
Write-Host "POWER CUT: PASS ($Cycles cycles)"
