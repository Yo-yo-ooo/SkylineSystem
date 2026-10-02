# tests/net_smoke.ps1 — 网络栈 QEMU 冒烟门禁 (回归门)
# 前置: 内核已构建 (wsl bash _tmp_build.sh)
# 用法: pwsh -File tests/net_smoke.ps1 [-Root <路径>] [-Qemu <路径>] [-WaitSeconds 60]
#   (退出码 0 = 通过)
# 断言: ① 无异常字样; ② 至少 1 次 ping 回复; ③ 统计节拍出现
# P4-88: 参数化 (原硬编码 C:\ZSY\SkylineSystem 与 QEMU 路径)
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [int]$WaitSeconds = 60
)

$root = if ($Root) { $Root } elseif ($env:SKYLINE_ROOT) { $env:SKYLINE_ROOT }
        else { Split-Path -Parent $PSScriptRoot }
$qemu = if ($Qemu) { $Qemu }
        elseif ($env:SKYLINE_QEMU) { $env:SKYLINE_QEMU }
        else { "C:\Program Files\qemu\qemu-system-x86_64.exe" }
if (-not (Test-Path $qemu)) { Write-Error "QEMU 未找到: $qemu"; exit 2 }
if (-not (Test-Path "$root\SkylineSystem-x86_64.iso")) { Write-Error "ISO 不存在, 先构建"; exit 2 }

Remove-Item "$root\serial.log","$root\dbg.log" -ErrorAction SilentlyContinue
$p = Start-Process $qemu -ArgumentList @(
  '-machine','q35','-cpu','max',
  '-cdrom',"$root\SkylineSystem-x86_64.iso",
  '-m','2G','-smp','4',
  '-serial',"file:$root\serial.log",
  '-display','none',
  '-net','nic','-net','user',
  '-drive',"file=$root\disk.img,if=none,id=drive0",
  '-device','ide-hd,drive=drive0,bus=ide.0',
  '-no-reboot'
) -PassThru -WindowStyle Hidden

Start-Sleep -Seconds $WaitSeconds
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } | Out-Null

$log = Get-Content "$root\serial.log" -Raw -ErrorAction SilentlyContinue
$fail = 0

$exceptions = Select-String -Path "$root\serial.log" -Pattern 'Exception|General protection|Kernel crash|Panic|Segfault|SMASHING' -ErrorAction SilentlyContinue
if ($exceptions) { Write-Host "FAIL: 异常输出"; $exceptions | Select-Object -First 3 | ForEach-Object { Write-Host $_.Line }; $fail = 1 }
else { Write-Host "PASS: 无异常输出" }

$pings = Select-String -Path "$root\serial.log" -Pattern 'ping\] reply' -ErrorAction SilentlyContinue
if ($pings) { Write-Host "PASS: ping 回复 x$($pings.Count)" }
else { Write-Host "FAIL: 无 ping 回复 (网络栈未工作)"; $fail = 1 }

$stats = Select-String -Path "$root\serial.log" -Pattern 'lwip\]\[stat\]' -ErrorAction SilentlyContinue
if ($stats) { Write-Host "PASS: 统计节拍 x$($stats.Count)" }
else { Write-Host "FAIL: 无统计节拍 (lwIP 线程未运行)"; $fail = 1 }

if ($fail) { Write-Host "NET SMOKE: FAIL"; exit 1 }
Write-Host "NET SMOKE: PASS"
exit 0
