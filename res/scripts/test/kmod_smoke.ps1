# res/scripts/test/kmod_smoke.ps1 - QEMU headless serial smoke gate for kmod /
# componentised kernel.
# ============================================================================
# Preconditions: ISO and kernel already built by the Lead. This script only
# consumes SkylineSystem-x86_64.iso + disk.img, boots QEMU headless, waits, then
# asserts the serial log. All keyword matching is ASCII-only on purpose (the
# serial log may contain ANSI/UTF-8 payloads; matching Chinese text would be
# brittle under Windows PowerShell 5.1's ANSI code-page decoding).
#
# Usage:
#   powershell -File res/scripts/test/kmod_smoke.ps1 [-Root <repo>]
#              [-Qemu <qemu-system-x86_64.exe>] [-WaitSeconds 90] [-MinBuiltins 1]
#              [-AllowBuiltinRollback]
# Exit codes: 0 = PASS, 1 = assertion failed, 2 = environment missing.
# Note: on a loaded host the full boot + KMOD_SELFTEST can take >120s; the
# independent acceptance run used -WaitSeconds 200. If (a)/(c) fail with the
# log ending mid-selftest, increase WaitSeconds rather than treating it as a bug.
#
# Assertions:
#   (a) serial.log exists and contains "KMOD_SELFTEST PASS"
#   (b) "[kmod] builtin ready:" lines >= -MinBuiltins
#   (c) "[kmod] boot load: 2 requested, 2 ready, 0 failed"
#       OR "[demo.greeter] hello_fn -> 1001"
#       (proves .ko parsing / relocation / private-contract import binding ran)
#   (d) no Exception|Panic|Kernel crash|General protection|SMASHING
#   (e) contains "desktop PROCESS" (existing boot behaviour preserved)
# Failure prints the last match of each failed keyword plus serial tail.
# ============================================================================
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [int]$WaitSeconds = 90,
    [int]$MinBuiltins = 1,
    [switch]$AllowBuiltinRollback
)

$ErrorActionPreference = "Continue"

# ---- Path resolution: explicit args > env vars > repo root from script path ----
$root = if ($Root) { $Root }
        elseif ($env:SKYLINE_ROOT) { $env:SKYLINE_ROOT }
        else { Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) }
$qemu = if ($Qemu) { $Qemu }
        elseif ($env:SKYLINE_QEMU) { $env:SKYLINE_QEMU }
        else { "C:\Program Files\qemu\qemu-system-x86_64.exe" }

$iso = Join-Path $root "SkylineSystem-x86_64.iso"
$disk = Join-Path $root "disk.img"
$serial = Join-Path $root "serial.log"
$dbg = Join-Path $root "dbg.log"
$fail = 0

if (-not (Test-Path $qemu)) { Write-Error "QEMU not found: $qemu"; exit 2 }
if (-not (Test-Path $iso)) { Write-Error "ISO not found, build it first: $iso"; exit 2 }
if (-not (Test-Path $disk)) { Write-Error "disk.img not found: $disk"; exit 2 }

Write-Host "KMOD SMOKE: root=$root"
Write-Host "KMOD SMOKE: qemu=$qemu wait=${WaitSeconds}s min-builtins=$MinBuiltins"

Remove-Item $serial, $dbg -ErrorAction SilentlyContinue

# ---- Boot QEMU headless, serial to file, fixed wait then terminate ----
$qemuArgs = @(
    '-M', 'q35', '-cpu', 'max',
    '-cdrom', $iso,
    '-m', '2G', '-smp', '4',
    '-serial', "file:$serial",
    '-display', 'none',
    '-drive', "file=$disk,if=none,id=d0",
    '-device', 'ide-hd,drive=d0,bus=ide.0',
    '-no-reboot'
)
# Snapshot existing QEMU PIDs so cleanup only touches processes we started.
$qemuBefore = @(Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue |
                ForEach-Object { $_.Id })

$p = Start-Process $qemu -ArgumentList $qemuArgs -PassThru -WindowStyle Hidden
if (-not $p) { Write-Error "failed to start QEMU"; exit 2 }

Start-Sleep -Seconds $WaitSeconds
if (-not $p.HasExited) {
    # Kill must be reliable: if it silently fails the serial file stays locked and
    # the subsequent read looks empty. Kill the Start-Process handle first, then
    # sweep any QEMU process that appeared during this run (handles launcher /
    # PID-mapping corner cases), retrying a few times.
    try { $p.Kill() } catch { }
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        $newProcs = @(Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue |
                      Where-Object { $qemuBefore -notcontains $_.Id })
        if ($newProcs.Count -eq 0) { break }
        foreach ($qp in $newProcs) {
            try { $qp.Kill() } catch { }
        }
        $p.WaitForExit(2000) | Out-Null
        Start-Sleep -Milliseconds 500
    }
    $left = @(Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue |
              Where-Object { $qemuBefore -notcontains $_.Id })
    if ($left.Count -gt 0) {
        Write-Host "WARN: force-killing $($left.Count) leftover QEMU process(es)"
        foreach ($qp in $left) { & taskkill /F /PID $qp.Id 2>$null | Out-Null }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Milliseconds 300
} else {
    Write-Host "KMOD SMOKE: QEMU exited by itself (exit=$($p.ExitCode))"
}

# ---- Serial log basic checks ----
if (-not (Test-Path $serial)) {
    Write-Host "FAIL: serial.log not produced (QEMU gave no serial output)"
    Write-Host "KMOD SMOKE: FAIL"
    exit 1
}
$raw = $null
for ($attempt = 0; $attempt -lt 10 -and -not $raw; $attempt++) {
    try {
        # FileShare.ReadWrite: QEMU may still hold the serial file open for writing
        # (e.g. kill just failed); opening with default share mode would throw.
        $fs = [System.IO.File]::Open($serial, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        $sr = New-Object System.IO.StreamReader($fs)
        $raw = $sr.ReadToEnd()
        $sr.Close()
        $fs.Close()
    } catch {
        Start-Sleep -Milliseconds 300
    }
}
if (-not $raw) {
    Write-Host "FAIL: serial.log is empty"
    Write-Host "KMOD SMOKE: FAIL"
    exit 1
}
$lineCount = ($raw -split "`n" | Measure-Object -Line).Lines
Write-Host "KMOD SMOKE: serial.log $lineCount lines"

function Last-Match([string]$pattern, [bool]$simple) {
    if ($simple) {
        $m = Select-String -Path $serial -Pattern $pattern -SimpleMatch -ErrorAction SilentlyContinue
    } else {
        $m = Select-String -Path $serial -Pattern $pattern -ErrorAction SilentlyContinue
    }
    if ($m) { return ($m | Select-Object -Last 1).Line }
    return $null
}

function Report([string]$name, [bool]$ok, [string]$pattern, [bool]$simple) {
    if ($ok) {
        Write-Host "PASS: $name"
    } else {
        Write-Host "FAIL: $name"
        $last = Last-Match $pattern $simple
        if ($last) { Write-Host "  last match: $last" }
        $script:fail = 1
    }
}

# ---- (a) kernel selftest passed ----
$selftest = Select-String -Path $serial -Pattern 'KMOD_SELFTEST PASS' -SimpleMatch -ErrorAction SilentlyContinue
Report "(a) KMOD_SELFTEST PASS" ([bool]$selftest) 'KMOD_SELFTEST' $true

# If it explicitly failed, print the LAST selftest line for diagnosis
$selftestFail = Select-String -Path $serial -Pattern 'KMOD_SELFTEST FAIL' -SimpleMatch -ErrorAction SilentlyContinue
if ($selftestFail) {
    $line = ($selftestFail | Select-Object -Last 1).Line
    Write-Host "  selftest: $line"
}

# ---- (b) builtin component graph established ----
$builtins = Select-String -Path $serial -Pattern '[kmod] builtin ready:' -SimpleMatch -ErrorAction SilentlyContinue
$nBuiltin = if ($builtins) { ($builtins | Measure-Object).Count } else { 0 }
$bOk = ($nBuiltin -ge $MinBuiltins)
if (-not $bOk -and $AllowBuiltinRollback) {
    # During the B1/B3/B4 integration window the builtin graph may legitimately
    # roll back (e.g. missing provider -> UNRESOLVED). Accept that as a warning.
    $rollback = Select-String -Path $serial -Pattern 'builtin.*(UNRESOLVED|rollback|FAILED)' -ErrorAction SilentlyContinue
    if ($rollback) {
        Write-Host "WARN: (b) builtin graph failed/rolled back; allowed by -AllowBuiltinRollback"
        $line = ($rollback | Select-Object -Last 1).Line
        Write-Host "  warning: $line"
        $bOk = $true
    }
}
Report "(b) '[kmod] builtin ready:' x$nBuiltin (>= $MinBuiltins)" $bOk '\[kmod\] builtin' $false

# ---- (c) .ko loader + private-contract binding proof ----
$bootLoad = Select-String -Path $serial -Pattern '[kmod] boot load: 2 requested, 2 ready, 0 failed' -SimpleMatch -ErrorAction SilentlyContinue
$greeter = Select-String -Path $serial -Pattern '[demo.greeter] hello_fn -> 1001' -SimpleMatch -ErrorAction SilentlyContinue
$cOk = ([bool]$bootLoad) -or ([bool]$greeter)
if ($cOk) {
    Write-Host "PASS: (c) .ko loader + private-contract binding proof"
} else {
    Write-Host "FAIL: (c) no '[kmod] boot load: 2 requested, 2 ready, 0 failed' and no '[demo.greeter] hello_fn -> 1001'"
    $last = Last-Match 'boot load|demo.greeter' $false
    if ($last) { Write-Host "  last match: $last" }
    $fail = 1
}

# ---- (d) no crash / panic keywords ----
$bad = Select-String -Path $serial -Pattern 'Exception|Panic|Kernel crash|General protection|SMASHING' -ErrorAction SilentlyContinue
if ($bad) {
    Write-Host "FAIL: (d) crash/panic keyword found in serial.log"
    $bad | Select-Object -First 3 | ForEach-Object { Write-Host "  $($_.Line)" }
    $fail = 1
} else {
    Write-Host "PASS: (d) no crash/panic keywords"
}

# ---- (e) existing boot behaviour preserved ----
$desktop = Select-String -Path $serial -Pattern 'desktop PROCESS' -SimpleMatch -ErrorAction SilentlyContinue
Report "(e) desktop PROCESS" ([bool]$desktop) 'desktop' $true

# ---- On failure print serial tail for quick triage ----
if ($fail) {
    Write-Host "---- serial.log tail ----"
    Get-Content $serial -Tail 25 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }
    Write-Host "KMOD SMOKE: FAIL"
    exit 1
}

Write-Host "KMOD SMOKE: PASS"
exit 0
