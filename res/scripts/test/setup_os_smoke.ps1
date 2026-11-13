#Requires -Version 5.1
# ============================================================================
# res/scripts/test/setup_os_smoke.ps1
#   Headless QEMU gate for the NEW "Setup OS" install path.
#
# WHAT IT PROVES
#   A) The on-demand Installer/Setup can be started from the desktop with
#      Ctrl+Alt+I and really installs a complete OS image into /mp/system:
#        [installer] start: ...
#        [setup] os pkg: ...                       (package info, printed)
#        [setup] os install complete: ... failed=0 (file count parsed)
#        [setup] manifest written: /mp/system/os.manifest
#        [installer] READY: window mounted, waiting Esc/Enter
#        legacy fallback accepted as evidence: [installer] Install complete: 6/6 files (...)
#   B) The installed tree on disk.img is complete and self-consistent:
#        /mp/system/os.manifest  contains "SKYOS1 installed", total_files=,
#                                installed_bytes=
#        /mp/system/boot/kernel        exists, > 1 MiB, starts with ELF magic
#        /mp/system/programs/desktop.elf  exists, > 100 KiB
#        /mp/system/lib/libsky.so         exists, > 100 KiB
#        the "[setup] os install complete" file count == manifest total_files
#   C) A screendump shows the Setup window on the desktop (title bar uses the
#      running window-style title colour, frame differs from the initial frame).
#
# FLOW
#   1) WSL e2tools preprocess disk.img: delete /system recursively (so the
#      install is a fresh one) and, unless -KeepThemeAssets, the theme assets
#      (same list as desktop_features_smoke.ps1).  /mp/os.pkg is NOT touched.
#   2) boot QEMU headless (-m 2G -smp 4 default), wait for the desktop +
#      Settings markers.
#   3) base screendump, then HMP "sendkey ctrl-alt-i" to start the Installer/
#      Setup, wait for the Setup/Installer serial markers (long install window).
#   4) screendump setup_os_installer.ppm, close the Setup window (Esc, Enter
#      fallback), quit QEMU + kill sweep.
#   5) WSL e2tools post-check: copy the installed files out of disk.img to the
#      WSL /tmp (NOT e2cp "-", per the earlier e2tools stdout bug), print sizes,
#      ELF magic and the manifest, then assert everything from PowerShell.
#
# OUTPUT (under <Root> unless overridden)
#   setup_os_serial.log
#   setup_os_base.ppm          initial desktop frame (Settings only)
#   setup_os_installer.ppm     frame with the Setup/Installer window
#
# EXIT CODES: 0 = PASS (WARNs allowed), 1 = assertion/environment failure.
# NOTE: this script only observes/verifies; it builds nothing and never runs
#       make.  It also does not replace /mp/os.pkg (the package source is kept).
# ============================================================================
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [string]$Iso = "",
    [string]$Disk = "",
    [int]$WaitSeconds = 120,
    [int]$ActionWaitSeconds = 30,
    [int]$InstallWaitSeconds = 240,
    [int]$MemMB = 2048,
    [int]$Smp = 4,
    [int]$ScreenshotDelay = 2,
    [int]$Tolerance = 12,
    [int]$MinDiffCells = 12,
    [string]$SerialLog = "",
    [string]$ScreenshotPrefix = "",
    [string]$WslDistro = "Ubuntu-24.04",
    [switch]$KeepThemeAssets,
    [switch]$SkipPreprocess,
    [switch]$SkipDiskVerify,
    [int]$MonitorPort = 0
)

$ErrorActionPreference = "Continue"
$script:FailCount = 0
$script:WarnCount = 0
$script:WslSeq = 0
$script:WslTmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sky_setup_wsl_{0}" -f $PID)

# ----------------------------------------------------------------------------
# Path resolution
# ----------------------------------------------------------------------------
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = if ($Root) { $Root }
        elseif ($env:SKYLINE_ROOT) { $env:SKYLINE_ROOT }
        else { Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $scriptDir)) }
$qemu = if ($Qemu) { $Qemu }
        elseif ($env:SKYLINE_QEMU) { $env:SKYLINE_QEMU }
        else { "C:\Program Files\qemu\qemu-system-x86_64.exe" }
try { $root = [System.IO.Path]::GetFullPath($root) } catch { }
$isoPath = if ($Iso) { $Iso } else { Join-Path $root "SkylineSystem-x86_64.iso" }
$diskPath = if ($Disk) { $Disk } else { Join-Path $root "disk.img" }
$serialPath = if ($SerialLog) { $SerialLog } else { Join-Path $root "setup_os_serial.log" }
$shotPrefix = if ($ScreenshotPrefix) { $ScreenshotPrefix } else { Join-Path $root "setup_os" }
try { $serialPath = [System.IO.Path]::GetFullPath($serialPath) } catch { }
if ($shotPrefix -notmatch '^[A-Za-z]:') { try { $shotPrefix = [System.IO.Path]::GetFullPath($shotPrefix) } catch { } }
$wslTmpHost = "/tmp/sky_setup_smoke"

# ============================================================================
# SHARED HELPERS (pure functions; no side effects beyond stdout)
# ============================================================================
function Write-Step { param([string]$Text) Write-Host ("SETUP OS SMOKE: {0}" -f $Text) }
function Info { param([string]$Text) Write-Host ("INFO: " + $Text) }
function Warn {
    param([string]$Name, [string]$Detail = "")
    Write-Host ("WARN: " + $Name)
    if ($Detail) { Write-Host ("      " + $Detail) }
    $script:WarnCount = $script:WarnCount + 1
}
function Report {
    param([string]$Name, [bool]$Ok, [string]$Detail = "")
    if ($Ok) {
        Write-Host ("PASS: " + $Name)
    } else {
        Write-Host ("FAIL: " + $Name)
        if ($Detail) { Write-Host ("      " + $Detail) }
        $script:FailCount = $script:FailCount + 1
    }
}
function Report-Disk {
    param([string]$Name, [bool]$Ok, [string]$Detail = "")
    if ($SkipDiskVerify) {
        Warn ($Name + " [disk verification skipped]") $Detail
    } else {
        Report $Name $Ok $Detail
    }
}
function Format-Rgb {
    param($Color)
    if ($null -eq $Color) { return "<none>" }
    return ("RGB({0},{1},{2}) #{3:X2}{4:X2}{5:X2}" -f `
        [int]$Color.R, [int]$Color.G, [int]$Color.B, `
        [int]$Color.R, [int]$Color.G, [int]$Color.B)
}
function Format-RgbDelta {
    param($Color, $Expected)
    if ($null -eq $Color -or $null -eq $Expected) { return "d=<n/a>" }
    return ("d=({0},{1},{2})" -f `
        ([int]$Color.R - [int]$Expected.R), `
        ([int]$Color.G - [int]$Expected.G), `
        ([int]$Color.B - [int]$Expected.B))
}
function New-Rgb {
    param([int]$R, [int]$G, [int]$B)
    return [pscustomobject]@{ R = [int]$R; G = [int]$G; B = [int]$B }
}
function Convert-Hex6ToRgb {
    param([string]$Hex)
    if (-not $Hex) { return $null }
    if ($Hex -notmatch '^[0-9A-Fa-f]{6}$') { return $null }
    return (New-Rgb `
        ([Convert]::ToInt32($Hex.Substring(0, 2), 16)) `
        ([Convert]::ToInt32($Hex.Substring(2, 2), 16)) `
        ([Convert]::ToInt32($Hex.Substring(4, 2), 16)))
}
function Get-RgbDiffSum {
    param($A, $B)
    if ($null -eq $A -or $null -eq $B) { return 999999 }
    return ([math]::Abs([int]$A.R - [int]$B.R) +
            [math]::Abs([int]$A.G - [int]$B.G) +
            [math]::Abs([int]$A.B - [int]$B.B))
}
function Test-RgbNear {
    param($A, $B, [int]$Tolerance = 12)
    if ($null -eq $A -or $null -eq $B) { return $false }
    return ((Test-RgbChannelNear ([int]$A.R) ([int]$B.R) $Tolerance) -and
            (Test-RgbChannelNear ([int]$A.G) ([int]$B.G) $Tolerance) -and
            (Test-RgbChannelNear ([int]$A.B) ([int]$B.B) $Tolerance))
}
function Test-RgbChannelNear {
    param([int]$A, [int]$B, [int]$Tolerance)
    return ([math]::Abs($A - $B) -le $Tolerance)
}
function Get-FirstCapture {
    param([string]$Text, [string]$Pattern, [int]$Group = 1)
    if (-not $Text) { return $null }
    $m = [regex]::Match($Text, $Pattern)
    if ($m.Success) { return $m.Groups[$Group].Value }
    return $null
}
function Get-LastMatch {
    param([string]$Text, [string]$Pattern)
    if (-not $Text) { return $null }
    $ms = [regex]::Matches($Text, $Pattern)
    if ($ms.Count -le 0) { return $null }
    return $ms[$ms.Count - 1]
}
function Get-LastLines {
    param([string]$Text, [int]$Count)
    if (-not $Text) { return @() }
    $lines = $Text -split "`n"
    $start = [math]::Max(0, $lines.Count - $Count)
    $out = @()
    for ($i = $start; $i -lt $lines.Count; $i++) { $out += ($lines[$i]).TrimEnd("`r") }
    return $out
}
function Read-TextFileShared {
    param([string]$Path, [int]$Attempts = 1)
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        $fs = $null
        $sr = $null
        try {
            $share = [System.IO.FileShare]([int][System.IO.FileShare]::ReadWrite -bor [int][System.IO.FileShare]::Delete)
            $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
            $sr = New-Object System.IO.StreamReader($fs)
            $text = $sr.ReadToEnd()
            $sr.Close()
            $fs.Close()
            return $text
        } catch {
            if ($sr) { try { $sr.Close() } catch { } }
            if ($fs) { try { $fs.Close() } catch { } }
            if ($attempt -lt $Attempts) { Start-Sleep -Milliseconds 250 }
        }
    }
    return $null
}

# ---------------------------------------------------------------------------
# PPM P6 parser + sampling
# ---------------------------------------------------------------------------
function Skip-PpmSeparators {
    param([byte[]]$Bytes, [hashtable]$State)
    while ($State.Pos -lt $State.Len) {
        $b = [int]$Bytes[$State.Pos]
        if ($b -eq 32 -or $b -eq 9 -or $b -eq 10 -or $b -eq 13) { $State.Pos = $State.Pos + 1; continue }
        if ($b -eq 35) {
            while ($State.Pos -lt $State.Len) {
                $c = [int]$Bytes[$State.Pos]
                $State.Pos = $State.Pos + 1
                if ($c -eq 10 -or $c -eq 13) { break }
            }
            continue
        }
        break
    }
}
function Read-PpmInteger {
    param([byte[]]$Bytes, [hashtable]$State)
    if ($State.Pos -ge $State.Len) { throw "PPM header truncated" }
    $start = [int]$State.Pos
    while ($State.Pos -lt $State.Len) {
        $b = [int]$Bytes[$State.Pos]
        if ($b -ge 48 -and $b -le 57) { $State.Pos = $State.Pos + 1 } else { break }
    }
    if ($State.Pos -eq $start) { throw "PPM header: expected integer" }
    $text = [System.Text.Encoding]::ASCII.GetString($Bytes, $start, $State.Pos - $start)
    return [int]$text
}
function Read-PpmFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw ("PPM file not found: {0}" -f $Path) }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $state = @{ Pos = 0; Len = [int]$bytes.Length }
    Skip-PpmSeparators -Bytes $bytes -State $state
    if (($state.Pos + 1) -ge $state.Len) { throw "PPM too short for a P6 header" }
    $c0 = [char][int]$bytes[$state.Pos]
    $c1 = [char][int]$bytes[$state.Pos + 1]
    if ($c0 -ne 'P' -or $c1 -ne '6') { throw ("unsupported PPM magic '{0}{1}'" -f $c0, $c1) }
    $state.Pos = $state.Pos + 2
    Skip-PpmSeparators -Bytes $bytes -State $state
    $w = Read-PpmInteger -Bytes $bytes -State $state
    Skip-PpmSeparators -Bytes $bytes -State $state
    $h = Read-PpmInteger -Bytes $bytes -State $state
    Skip-PpmSeparators -Bytes $bytes -State $state
    $maxv = Read-PpmInteger -Bytes $bytes -State $state
    if ($w -le 0 -or $h -le 0) { throw ("invalid PPM size {0}x{1}" -f $w, $h) }
    if ($maxv -ne 255) { throw ("unsupported PPM maxval {0}" -f $maxv) }
    if ($state.Pos -ge $state.Len) { throw "PPM header has no raster" }
    $sep = [int]$bytes[$state.Pos]
    if (-not ($sep -eq 32 -or $sep -eq 9 -or $sep -eq 10 -or $sep -eq 13)) { throw "PPM header missing separator" }
    $state.Pos = $state.Pos + 1
    if ($sep -eq 13 -and $state.Pos -lt $state.Len -and [int]$bytes[$state.Pos] -eq 10) { $state.Pos = $state.Pos + 1 }
    $dataStart = [int]$state.Pos
    $need = [int64]$w * [int64]$h * 3
    if (([int64]$dataStart + $need) -gt [int64]$state.Len) { throw "PPM raster truncated" }
    return [pscustomobject]@{
        Width = [int]$w; Height = [int]$h; MaxVal = [int]$maxv
        Bytes = $bytes; DataStart = $dataStart; Path = $Path
    }
}
function Get-PpmPixelRgb {
    param($Ppm, [int]$X, [int]$Y)
    if ($null -eq $Ppm) { return $null }
    if ($X -lt 0 -or $Y -lt 0 -or $X -ge $Ppm.Width -or $Y -ge $Ppm.Height) { return $null }
    $off = [int]$Ppm.DataStart + (($Y * $Ppm.Width) + $X) * 3
    return (New-Rgb ([int]$Ppm.Bytes[$off]) ([int]$Ppm.Bytes[$off + 1]) ([int]$Ppm.Bytes[$off + 2]))
}
function Get-PpmRegionAverage {
    param($Ppm, [int]$X, [int]$Y, [int]$Radius = 2)
    if ($null -eq $Ppm) { return $null }
    $x0 = [math]::Max(0, $X - $Radius)
    $x1 = [math]::Min([int]$Ppm.Width - 1, $X + $Radius)
    $y0 = [math]::Max(0, $Y - $Radius)
    $y1 = [math]::Min([int]$Ppm.Height - 1, $Y + $Radius)
    $sumR = 0; $sumG = 0; $sumB = 0; $count = 0
    for ($yy = $y0; $yy -le $y1; $yy++) {
        for ($xx = $x0; $xx -le $x1; $xx++) {
            $off = [int]$Ppm.DataStart + (($yy * $Ppm.Width) + $xx) * 3
            $sumR = $sumR + [int]$Ppm.Bytes[$off]
            $sumG = $sumG + [int]$Ppm.Bytes[$off + 1]
            $sumB = $sumB + [int]$Ppm.Bytes[$off + 2]
            $count = $count + 1
        }
    }
    if ($count -le 0) { return $null }
    return (New-Rgb ([int][math]::Floor($sumR / $count + 0.5)) ([int][math]::Floor($sumG / $count + 0.5)) ([int][math]::Floor($sumB / $count + 0.5)))
}
function Get-PpmSignature {
    param($Ppm, [int]$Cols = 32, [int]$Rows = 20)
    if ($null -eq $Ppm) { return $null }
    $cells = New-Object System.Collections.ArrayList
    $cellW = [math]::Max(1, [int][math]::Floor($Ppm.Width / $Cols))
    $cellH = [math]::Max(1, [int][math]::Floor($Ppm.Height / $Rows))
    for ($r = 0; $r -lt $Rows; $r++) {
        for ($c = 0; $c -lt $Cols; $c++) {
            $x = [math]::Min([int]$Ppm.Width - 1, $c * $cellW + [int]($cellW / 2))
            $y = [math]::Min([int]$Ppm.Height - 1, $r * $cellH + [int]($cellH / 2))
            $px = Get-PpmPixelRgb -Ppm $Ppm -X $x -Y $y
            [void]$cells.Add(@([int]$px.R, [int]$px.G, [int]$px.B))
        }
    }
    return [pscustomobject]@{ Cells = $cells.ToArray(); Cols = $Cols; Rows = $Rows }
}
function Compare-PpmSignature {
    param($A, $B, [int]$CellThreshold = 12)
    if ($null -eq $A -or $null -eq $B) { return [pscustomobject]@{ DiffCells = 99999; Total = 0 } }
    $n = [math]::Min($A.Cells.Count, $B.Cells.Count)
    $diff = 0
    for ($i = 0; $i -lt $n; $i++) {
        $ca = $A.Cells[$i]; $cb = $B.Cells[$i]
        $d = [math]::Abs($ca[0] - $cb[0]) + [math]::Abs($ca[1] - $cb[1]) + [math]::Abs($ca[2] - $cb[2])
        if ($d -ge $CellThreshold) { $diff = $diff + 1 }
    }
    return [pscustomobject]@{ DiffCells = $diff; Total = $n }
}

# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------
function Get-SerialSnapshot {
    param([string]$Path, [int]$Attempts = 2)
    return (Read-TextFileShared -Path $Path -Attempts $Attempts)
}
function Get-PatternCount {
    param([string]$Text, [string]$Pattern)
    if (-not $Text) { return 0 }
    return ([regex]::Matches($Text, $Pattern)).Count
}
function Wait-SerialAtLeast {
    param([string]$Path, [string]$Pattern, [int]$MinCount = 1,
          [int]$TimeoutSeconds = 30, $Proc = $null)
    $startTime = Get-Date
    $deadline = $startTime.AddSeconds($TimeoutSeconds)
    $text = ""
    while ((Get-Date) -lt $deadline) {
        $snap = Get-SerialSnapshot -Path $Path -Attempts 1
        if ($null -ne $snap) { $text = $snap }
        $count = Get-PatternCount -Text $text -Pattern $Pattern
        if ($count -ge $MinCount) {
            return [pscustomobject]@{ Ok = $true; Count = $count; Text = $text; Elapsed = ((Get-Date) - $startTime).TotalSeconds }
        }
        if ($Proc) { try { if ($Proc.HasExited) { break } } catch { } }
        Start-Sleep -Milliseconds 300
    }
    return [pscustomobject]@{ Ok = $false; Count = (Get-PatternCount -Text $text -Pattern $Pattern); Text = $text; Elapsed = ((Get-Date) - $startTime).TotalSeconds }
}
function Get-StyleValues {
    param([string]$Text)
    $m = Get-LastMatch -Text $Text -Pattern '\[desktop\] lua: style radius=(\d+) title=0x([0-9A-Fa-f]{6}) border=0x([0-9A-Fa-f]{6}) ink=0x([0-9A-Fa-f]{6}) close=0x([0-9A-Fa-f]{6}) paper=0x([0-9A-Fa-f]{6}) shadow=(\d+)'
    if (-not $m) { return $null }
    return [pscustomobject]@{
        Radius = [int]$m.Groups[1].Value
        Title  = (Convert-Hex6ToRgb $m.Groups[2].Value)
        Paper  = (Convert-Hex6ToRgb $m.Groups[6].Value)
    }
}

# ---------------------------------------------------------------------------
# WSL e2tools helpers (disk.img pre/post-processing)
# ---------------------------------------------------------------------------
function ConvertTo-WslPath {
    param([string]$WindowsPath)
    $p = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($p -match '^([A-Za-z]):\\(.*)$') {
        $drive = $Matches[1].ToLowerInvariant()
        $rest = $Matches[2] -replace '\\', '/'
        return ("/mnt/{0}/{1}" -f $drive, $rest)
    }
    throw ("cannot convert to a WSL path: {0}" -f $WindowsPath)
}
function Invoke-WslScript {
    param([string]$Body, [int]$TimeoutMs = 180000)
    New-Item -ItemType Directory -Force -Path $script:WslTmpDir | Out-Null
    $script:WslSeq = $script:WslSeq + 1
    $sh = Join-Path $script:WslTmpDir ("step_{0}.sh" -f $script:WslSeq)
    $outFile = Join-Path $script:WslTmpDir ("step_{0}.out" -f $script:WslSeq)
    $errFile = Join-Path $script:WslTmpDir ("step_{0}.err" -f $script:WslSeq)
    $full = "#!/bin/bash`ntrap 'echo __WSL_RC__=`$?' EXIT`n" + $Body + "`n"
    [System.IO.File]::WriteAllText($sh, ($full -replace "`r`n", "`n"))
    $wslSh = ConvertTo-WslPath $sh
    $wslShArg = $wslSh
    if ($wslShArg -match '\s') { $wslShArg = '"' + $wslShArg + '"' }
    Remove-Item $outFile, $errFile -Force -ErrorAction SilentlyContinue
    $p = $null
    try {
        $p = Start-Process -FilePath 'wsl.exe' `
            -ArgumentList @('-d', $WslDistro, 'bash', $wslShArg) `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    } catch {
        return [pscustomobject]@{ Ok = $false; Code = -1; Output = ""; Error = $_.Exception.Message }
    }
    $null = $p.WaitForExit($TimeoutMs)
    if (-not $p.HasExited) { try { $p.Kill() } catch { } ; return [pscustomobject]@{ Ok = $false; Code = -1; Output = "<wsl timeout>"; Error = "timeout" } }
    $out = ""
    if (Test-Path $outFile) { $out = [System.IO.File]::ReadAllText($outFile) }
    $err = ""
    if (Test-Path $errFile) { $err = [System.IO.File]::ReadAllText($errFile) }
    $rc = 0
    $m = [regex]::Match($out, '(?m)^__WSL_RC__=(-?\d+)\s*$')
    if ($m.Success) {
        $rc = [int]$m.Groups[1].Value
        $out = $out.Substring(0, $m.Index)
    }
    return [pscustomobject]@{ Ok = ($rc -eq 0); Code = $rc; Output = $out; Error = $err }
}
function Test-WslE2Tools {
    param([string]$Distro)
    New-Item -ItemType Directory -Force -Path $script:WslTmpDir | Out-Null
    try {
        $out = & wsl.exe -d $Distro -e bash -c "command -v e2ls && command -v e2rm && command -v e2cp && echo __E2_OK__" 2>$null
        return ((($out | Out-String)) -match '__E2_OK__')
    } catch { return $false }
}
function Get-TokensFromListing {
    param([string]$Listing)
    $tokens = @()
    if (-not $Listing) { return $tokens }
    foreach ($tok in ($Listing -split '\s+')) { if ($tok -and $tok.Trim()) { $tokens += $tok.Trim() } }
    return $tokens
}

# ---------------------------------------------------------------------------
# QEMU process + HMP monitor helpers
# ---------------------------------------------------------------------------
function Get-FreeTcpPort {
    $listener = $null
    try {
        $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
        $listener.Start()
        $port = [int]$listener.LocalEndpoint.Port
        $listener.Stop()
        if ($port -gt 0) { return $port }
    } catch {
        if ($listener) { try { $listener.Stop() } catch { } }
    }
    return 4444
}
function Quote-ProcessArg {
    param([string]$Arg)
    if ($null -eq $Arg) { return '""' }
    if ($Arg -match '\s') { return ('"' + $Arg + '"') }
    return $Arg
}
function Get-QemuProcessIds {
    try { return @(Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.Id }) }
    catch { return @() }
}
function Stop-QemuProcesses {
    param([int[]]$Before, $Proc)
    if ($Proc) {
        try { if (-not $Proc.HasExited) { $Proc.Kill(); [void]$Proc.WaitForExit(3000) } } catch { }
    }
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        $newProcs = @(Get-QemuProcessIds | Where-Object { $Before -notcontains $_ })
        if ($newProcs.Count -eq 0) { return $true }
        foreach ($qpid in $newProcs) {
            try { $qp = Get-Process -Id $qpid -ErrorAction SilentlyContinue; if ($qp) { $qp.Kill() } } catch { }
        }
        if ($Proc) { try { [void]$Proc.WaitForExit(1500) } catch { } }
        Start-Sleep -Milliseconds 500
    }
    $left = @(Get-QemuProcessIds | Where-Object { $Before -notcontains $_ })
    foreach ($qpid in $left) { try { & taskkill /F /PID $qpid 2>$null | Out-Null } catch { } }
    if ($left.Count -gt 0) { Start-Sleep -Milliseconds 600 }
    return ((@(Get-QemuProcessIds | Where-Object { $Before -notcontains $_ })).Count -eq 0)
}
function Format-MonitorText {
    param([string]$Text)
    if (-not $Text) { return "<empty>" }
    $clean = $Text -replace '[^\x20-\x7E\r\n]', ' '
    $clean = $clean -replace '\s+', ' '
    return $clean.Trim()
}
function Read-MonitorUntil {
    param([System.Net.Sockets.NetworkStream]$Stream, [string]$Token, [int]$TimeoutMs)
    $buffer = New-Object byte[] 4096
    $sb = New-Object System.Text.StringBuilder
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $read = -1
        try { if ($Stream.DataAvailable) { $read = [int]$Stream.Read($buffer, 0, $buffer.Length) } } catch { break }
        if ($read -gt 0) {
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            if ($sb.ToString().Contains($Token)) { break }
        } elseif ($read -eq 0) { break } else { Start-Sleep -Milliseconds 80 }
    }
    return $sb.ToString()
}
function Open-MonitorSession {
    param([string]$HostName = '127.0.0.1', [int]$Port, [int]$TimeoutMs = 15000)
    $result = [pscustomobject]@{ Ok = $false; Error = ""; Client = $null; Stream = $null }
    $lastError = ""
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        $candidate = $null
        try {
            $candidate = New-Object System.Net.Sockets.TcpClient
            $iar = $candidate.BeginConnect($HostName, $Port, $null, $null)
            if (-not $iar.AsyncWaitHandle.WaitOne(3000)) { throw "connect timed out" }
            $candidate.EndConnect($iar)
            $stream = $candidate.GetStream()
            $banner = Read-MonitorUntil -Stream $stream -Token '(qemu)' -TimeoutMs $TimeoutMs
            if (-not $banner.Contains('(qemu)')) {
                $lastError = ("no monitor prompt (output: {0})" -f (Format-MonitorText $banner))
                try { $candidate.Close() } catch { }
                Start-Sleep -Milliseconds 600
                continue
            }
            $result.Ok = $true; $result.Client = $candidate; $result.Stream = $stream
            return $result
        } catch {
            $lastError = $_.Exception.Message
            if ($candidate) { try { $candidate.Close() } catch { } }
            Start-Sleep -Milliseconds 600
        }
    }
    $result.Error = ("cannot open HMP monitor on {0}:{1} ({2})" -f $HostName, $Port, $lastError)
    return $result
}
function Invoke-MonitorCommand {
    param($Session, [string]$Command, [int]$TimeoutMs = 8000)
    if ($null -eq $Session -or -not $Session.Ok) { return "" }
    try {
        $payload = [System.Text.Encoding]::ASCII.GetBytes($Command + "`n")
        $Session.Stream.Write($payload, 0, $payload.Length)
        $Session.Stream.Flush()
        return (Read-MonitorUntil -Stream $Session.Stream -Token '(qemu)' -TimeoutMs $TimeoutMs)
    } catch {
        $Session.Ok = $false
        $Session.Error = $_.Exception.Message
        return ""
    }
}
function Send-MonitorKey {
    param($Session, [string]$Keys, [int]$TimeoutMs = 8000)
    $out = Invoke-MonitorCommand -Session $Session -Command ("sendkey " + $Keys) -TimeoutMs $TimeoutMs
    $err = [regex]::Match($out, '(?im)(unknown key[^\r\n]*|invalid[^\r\n]*|error[^\r\n]*|expects[^\r\n]*|unsupported[^\r\n]*)')
    if ($err.Success) { return [pscustomobject]@{ Ok = $false; Output = (Format-MonitorText $out); Error = $err.Value.Trim() } }
    return [pscustomobject]@{ Ok = $true; Output = (Format-MonitorText $out); Error = "" }
}
function Send-MonitorText {
    param($Session, [string]$Command, [int]$TimeoutMs = 8000)
    return (Invoke-MonitorCommand -Session $Session -Command $Command -TimeoutMs $TimeoutMs)
}
function Get-ScreendumpStagingPath {
    param([string]$RequestedPath)
    if ($RequestedPath -and ($RequestedPath -notmatch '\s')) { return $RequestedPath }
    try {
        $tmp = [System.IO.Path]::GetTempPath()
        if ($tmp) { return [System.IO.Path]::Combine($tmp, ("setup_os_shot_{0}.ppm" -f $PID)) }
    } catch { }
    return $null
}
function Save-Screenshot {
    param($Session, [string]$Path, [int]$Retries = 3)
    if ($null -eq $Session -or -not $Session.Ok) {
        return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = "no monitor session" }
    }
    $staging = Get-ScreendumpStagingPath -RequestedPath $Path
    if (-not $staging) { return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = "no staging path" } }
    $lastError = ""
    for ($attempt = 1; $attempt -le $Retries; $attempt++) {
        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Force -ErrorAction SilentlyContinue }
        $hmpPath = $staging.Replace('\', '/')
        $out = Send-MonitorText -Session $Session -Command ("screendump " + $hmpPath) -TimeoutMs 20000
        $fileOk = $false
        for ($wait = 0; $wait -lt 12; $wait++) {
            if (Test-Path -LiteralPath $staging) {
                $len = 0
                try { $len = (Get-Item -LiteralPath $staging).Length } catch { }
                if ($len -gt 100) { $fileOk = $true; break }
            }
            Start-Sleep -Milliseconds 200
        }
        if ($fileOk) {
            if ($staging -ne $Path) {
                try { Copy-Item -LiteralPath $staging -Destination $Path -Force } catch { }
                Remove-Item -LiteralPath $staging -Force -ErrorAction SilentlyContinue
            }
            try {
                $ppm = Read-PpmFile -Path $Path
                return [pscustomobject]@{ Ok = $true; Path = $Path; Ppm = $ppm; Error = "" }
            } catch {
                return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = ("PPM parse failed: " + $_.Exception.Message) }
            }
        }
        $lastError = Format-MonitorText $out
        Start-Sleep -Milliseconds 700
    }
    return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = ("screendump failed: " + $lastError) }
}

# ============================================================================
# END OF SHARED HELPERS
# ============================================================================

function Exit-Environment {
    param([string]$Message)
    Write-Host ("ERROR: " + $Message)
    Write-Host "SETUP OS SMOKE: FAIL (environment)"
    exit 1
}

# ----------------------------------------------------------------------------
# Phase 1: environment
# ----------------------------------------------------------------------------
Write-Host "SETUP OS SMOKE: ======================================================"
Write-Host "SETUP OS SMOKE: Setup OS install gate (disk.img /mp/system verification)"
Write-Host "SETUP OS SMOKE: root = $root"
Write-Host "SETUP OS SMOKE: qemu = $qemu"
Write-Host "SETUP OS SMOKE: iso  = $isoPath"
Write-Host "SETUP OS SMOKE: disk = $diskPath"
Write-Host "SETUP OS SMOKE: serial = $serialPath"
Write-Host "SETUP OS SMOKE: shots  = ${shotPrefix}_*.ppm"

if (-not (Test-Path -LiteralPath $qemu)) { Exit-Environment ("QEMU not found: " + $qemu) }
if (-not (Test-Path -LiteralPath $isoPath)) { Exit-Environment ("ISO not found, build it first: " + $isoPath) }
if (-not (Test-Path -LiteralPath $diskPath)) { Exit-Environment ("disk.img not found: " + $diskPath) }
$existingQemu = @(Get-QemuProcessIds)
if ($existingQemu.Count -gt 0) {
    Exit-Environment ("another QEMU is already running (PID: {0}); stop it first" -f ($existingQemu -join ', '))
}
$needWsl = (-not $SkipPreprocess) -or (-not $SkipDiskVerify)
if ($needWsl) {
    if (-not (Test-WslE2Tools -Distro $WslDistro)) {
        Exit-Environment ("WSL distro '{0}' with e2tools (e2ls/e2rm/e2cp) is required" -f $WslDistro)
    }
    Write-Host "SETUP OS SMOKE: WSL e2tools OK (distro=$WslDistro)"
}
$serialDir = Split-Path -Parent $serialPath
if ($serialDir -and -not (Test-Path -LiteralPath $serialDir)) { New-Item -ItemType Directory -Force -Path $serialDir | Out-Null }
Remove-Item -LiteralPath $serialPath -Force -ErrorAction SilentlyContinue
foreach ($suffix in @('base', 'installer')) {
    Remove-Item -LiteralPath ("{0}_{1}.ppm" -f $shotPrefix, $suffix) -Force -ErrorAction SilentlyContinue
}

$diskWsl = ConvertTo-WslPath $diskPath

# ----------------------------------------------------------------------------
# Phase 2: preprocess disk.img (fresh /system, optional fresh theme assets)
# ----------------------------------------------------------------------------
if (-not $SkipPreprocess) {
    Write-Step "preprocessing disk.img (delete /system recursively)"
    $themeDelete = "for f in themes.index selected_theme.txt theme_aurora.lua theme_ember.lua theme_mint.lua files.index notes.txt; do " +
                   "if e2ls `"`$img:/`" 2>/dev/null | tr -s ' ' '\n' | grep -qx `"`$f`"; then out=`$(e2rm `"`$img:/`$f`" 2>&1); echo `"rm `$f rc=`$?`"; fi; done"
    if ($KeepThemeAssets) { $themeDelete = "echo 'theme assets kept (-KeepThemeAssets)'" }
    $body = @"
img='$diskWsl'
echo '=== ROOT BEFORE ==='
e2ls "`$img:/" 2>&1
if e2ls "`$img:/" 2>/dev/null | tr -s ' ' '\n' | grep -qx 'system'; then
  out=`$(e2rm -r "`$img:/system" 2>&1); rc=`$?
  echo "rmdir system rc=`$rc out=`$out"
else
  echo 'rmdir system absent'
fi
$themeDelete
echo '=== ROOT AFTER ==='
e2ls "`$img:/" 2>&1
echo '=== PREPROCESS DONE ==='
exit 0
"@
    $pre = Invoke-WslScript -Body $body
    if (-not $pre.Ok) {
        Write-Host $pre.Output
        Exit-Environment ("disk.img preprocessing failed (WSL rc={0}): {1}" -f $pre.Code, (Format-MonitorText $pre.Error))
    }
    $afterMatch = [regex]::Match($pre.Output, '(?s)=== ROOT AFTER ===(.*?)=== PREPROCESS DONE ===')
    $afterTokens = @(Get-TokensFromListing -Listing $afterMatch.Groups[1].Value)
    $requiredBase = @('desktop.elf', 'installer.elf', 'settings.elf', 'libsky.so', 'desktop.lua')
    $missingBase = @()
    foreach ($need in $requiredBase) { if ($afterTokens -notcontains $need) { $missingBase += $need } }
    if ($missingBase.Count -gt 0) {
        Write-Host ($pre.Output)
        Exit-Environment ("disk.img is missing required base files: " + ($missingBase -join ', '))
    }
    if ($afterTokens -contains 'system') {
        Warn "could not remove /system recursively (Setup must overwrite the old install)" "root listing still contains 'system'"
    } else {
        Write-Step "disk.img preprocessed: /system absent, base files present"
    }
    if ($pre.Output -match 'rmdir system rc=([1-9])') {
        Warn "e2rm -r /system returned a failure code; Setup is expected to overwrite" "see preprocessing output above"
    }
} else {
    Warn "disk preprocessing skipped (-SkipPreprocess)" "the Setup may report an overwrite install"
}

# ----------------------------------------------------------------------------
# Phase 3: QEMU boot + Ctrl+Alt+I Setup trigger + screendumps
# ----------------------------------------------------------------------------
$monitorPort = if ($MonitorPort -gt 0) { [int]$MonitorPort } else { [int](Get-FreeTcpPort) }
$qemuArgs = @(
    '-M', 'q35', '-cpu', 'max',
    '-m', ("{0}M" -f $MemMB), '-smp', ("{0}" -f $Smp),
    '-cdrom', $isoPath,
    '-serial', ("file:{0}" -f $serialPath),
    '-monitor', ("telnet:127.0.0.1:{0},server,nowait" -f $monitorPort),
    '-display', 'none', '-vga', 'std', '-no-reboot', '-no-shutdown',
    '-drive', ("file={0},if=none,id=drive0,format=raw" -f $diskPath),
    '-device', 'ide-hd,drive=drive0,bus=ide.0'
)
$quotedArgs = @($qemuArgs | ForEach-Object { Quote-ProcessArg $_ })
Write-Step ("starting QEMU (-m {0}M -smp {1}, monitor telnet:127.0.0.1:{2})" -f $MemMB, $Smp, $monitorPort)
Write-Host ("SETUP OS SMOKE: command = {0} {1}" -f $qemu, ($quotedArgs -join ' '))

$qemuBefore = @(Get-QemuProcessIds)
$qmProc = $null
$monitor = $null
$envError = $null
$bootResult = $null
$shots = @{}
$installerStartCount = 0
$setupPkgLine = $null
$setupCompleteLine = $null
$setupManifestLine = $null
$legacyCompleteLine = $null
$installerReadyLine = $null
$installerExitLine = $null
$finalSerialText = ""

try {
    try {
        $qmProc = Start-Process -FilePath $qemu -ArgumentList $quotedArgs -PassThru -WindowStyle Hidden
    } catch {
        $envError = ("failed to start QEMU: " + $_.Exception.Message)
    }
    if ((-not $envError) -and (-not $qmProc)) { $envError = "Start-Process returned no QEMU process object" }
    if ((-not $envError) -and $qmProc) {
        Write-Step ("QEMU started, pid={0}; waiting up to {1}s for desktop markers" -f $qmProc.Id, $WaitSeconds)
        Start-Sleep -Milliseconds 1200
        if ($qmProc.HasExited) {
            $envError = ("QEMU exited immediately (exit code {0}); stale disk lock or bad ISO/args?" -f $qmProc.ExitCode)
            Write-Host ("ERROR: " + $envError)
        } else {
            $bootSpecs = @(
                [pscustomobject]@{ Name = 'desktop start'; Pattern = '\[desktop\] start: fb=' },
                [pscustomobject]@{ Name = 'lua source=file'; Pattern = '\[desktop\] lua: source=file' },
                [pscustomobject]@{ Name = 'lua style'; Pattern = '\[desktop\] lua: style radius=' },
                [pscustomobject]@{ Name = 'Settings spawned'; Pattern = '\[wm\] spawn Settings' },
                [pscustomobject]@{ Name = 'desktop ready'; Pattern = '\[perf\] desktop ready in' }
            )
            $bootStart = Get-Date
            $deadline = $bootStart.AddSeconds($WaitSeconds)
            $bootText = ""
            $missing = @()
            while ((Get-Date) -lt $deadline) {
                $snap = Get-SerialSnapshot -Path $serialPath -Attempts 1
                if ($null -ne $snap) { $bootText = $snap }
                $missing = @()
                foreach ($spec in $bootSpecs) {
                    if ((Get-PatternCount -Text $bootText -Pattern $spec.Pattern) -lt 1) { $missing += $spec.Name }
                }
                if ($missing.Count -eq 0) { break }
                $fatal = Get-LastMatch -Text $bootText -Pattern 'Kernel panic|Kernel crash|General protection|SMASHING|\[desktop\] FATAL|STALL WATCHDOG'
                if ($fatal) { break }
                try { if ($qmProc.HasExited) { break } } catch { }
                Start-Sleep -Milliseconds 400
            }
            $bootResult = [pscustomobject]@{ Ok = ($missing.Count -eq 0); Missing = $missing; Text = $bootText; Elapsed = ((Get-Date) - $bootStart).TotalSeconds }
            if ($bootResult.Ok) {
                Write-Step ("boot markers complete after {0:N1}s" -f $bootResult.Elapsed)
                Start-Sleep -Seconds ([int][math]::Max(1, $ScreenshotDelay))
            } else {
                Write-Host ("SETUP OS SMOKE: boot marker miss: {0}" -f ($missing -join ', '))
            }
            $monitor = Open-MonitorSession -HostName '127.0.0.1' -Port $monitorPort
            if (-not $monitor.Ok) {
                $envError = $monitor.Error
                Write-Host ("ERROR: " + $monitor.Error)
            } else {
                Write-Step ("HMP monitor connected (port {0})" -f $monitorPort)
                $shotBase = Save-Screenshot -Session $monitor -Path ("{0}_base.ppm" -f $shotPrefix)
                $shots['base'] = $shotBase

                if ($bootResult.Ok) {
                    # Ctrl+Alt+I starts the Installer/Setup (on-demand integration)
                    $r = Send-MonitorKey -Session $monitor -Keys 'ctrl-alt-i'
                    if (-not $r.Ok) { Warn "HMP sendkey ctrl-alt-i failed" $r.Error }
                    $startWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] start:' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $qmProc
                    if (-not $startWait.Ok) {
                        Write-Step "Ctrl+Alt+I did not start the Installer; trying Ctrl+Alt+S then I (Settings command)"
                        [void](Send-MonitorKey -Session $monitor -Keys 'ctrl-alt-s')
                        Start-Sleep -Milliseconds 500
                        [void](Send-MonitorKey -Session $monitor -Keys 'i')
                        $startWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] start:' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $qmProc
                    }
                    $installerStartCount = $startWait.Count
                    # Setup OS deployment: package info + install completion + manifest
                    $pkgWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[setup\] os pkg:' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $qmProc
                    $completeWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[setup\] os install complete:' -MinCount 1 -TimeoutSeconds $InstallWaitSeconds -Proc $qmProc
                    $manifestWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[setup\] manifest written: /mp/system/os\.manifest' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $qmProc
                    $readyWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] READY: window mounted' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $qmProc
                    # READY 之后 Installer 才发 LOAD_LUA；等 Aurora 主题真正应用到桌面再取
                    # snapshot 解析 style，避免 C2 偶发采样到开机默认 style。
                    [void](Wait-SerialAtLeast -Path $serialPath -Pattern '\[desktop\] theme: load path=/mp/theme_aurora\.lua' -MinCount 1 -TimeoutSeconds 10 -Proc $qmProc)
                    Start-Sleep -Milliseconds 400
                    $snap = Get-SerialSnapshot -Path $serialPath -Attempts 3
                    $finalSerialText = $snap
                    $m = Get-LastMatch -Text $snap -Pattern '\[setup\] os pkg:[^\r\n]*'
                    if ($m) { $setupPkgLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[setup\] os install complete:[^\r\n]*'
                    if ($m) { $setupCompleteLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[setup\] manifest written:[^\r\n]*'
                    if ($m) { $setupManifestLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[installer\] Install complete:[^\r\n]*'
                    if ($m) { $legacyCompleteLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[installer\] READY:[^\r\n]*'
                    if ($m) { $installerReadyLine = $m.Value.Trim() }

                    Start-Sleep -Milliseconds 1200
                    $shotInst = Save-Screenshot -Session $monitor -Path ("{0}_installer.ppm" -f $shotPrefix)
                    $shots['installer'] = $shotInst
                    $styleVals = Get-StyleValues -Text $snap
                    if ($shotInst.Ok -and $shotBase.Ok -and $styleVals) {
                        $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shotBase.Ppm) -B (Get-PpmSignature -Ppm $shotInst.Ppm)
                        $geom = [pscustomobject]@{
                            TitleX = 404 + [int](560 / 2)
                            TitleY = 244 + 8
                            BodyX = 404; BodyY = 244; BodyW = 560; BodyH = 380
                        }
                        $titlePx = Get-PpmRegionAverage -Ppm $shotInst.Ppm -X $geom.TitleX -Y $geom.TitleY -Radius 2
                        $nonBlack = 0
                        for ($gy = 0; $gy -lt 6; $gy++) {
                            for ($gx = 0; $gx -lt 8; $gx++) {
                                $sx = $geom.BodyX + 20 + [int](($geom.BodyW - 40) * $gx / 7)
                                $sy = $geom.BodyY + 40 + [int](($geom.BodyH - 70) * $gy / 5)
                                $px = Get-PpmPixelRgb -Ppm $shotInst.Ppm -X $sx -Y $sy
                                if ($null -ne $px -and (([int]$px.R + [int]$px.G + [int]$px.B) -ge 30)) { $nonBlack++ }
                            }
                        }
                        Report "C1) screendump changed after the Setup window appeared" `
                            ($diff.DiffCells -ge $MinDiffCells) `
                            ("{0}/{1} grid cells changed (threshold {2})" -f $diff.DiffCells, $diff.Total, $MinDiffCells)
                        Report "C2) Setup window title bar uses the running style title" `
                            (Test-RgbNear $titlePx $styleVals.Title $Tolerance) `
                            ("sampled {0} at ({1},{2}) vs style title {3} ({4})" -f `
                                (Format-Rgb $titlePx), $geom.TitleX, $geom.TitleY, (Format-Rgb $styleVals.Title), (Format-RgbDelta $titlePx $styleVals.Title))
                        Report "C3) Setup window content is not a blank frame" ($nonBlack -ge 6) `
                            ("{0}/48 sampled content points have visible (non-black) pixels" -f $nonBlack)
                    } else {
                        Report "C1) Setup frame captured and parsed" ($null -ne $shotInst -and $shotInst.Ok) `
                            ($(if ($shotInst) { $shotInst.Error } else { "no screenshot" }))
                    }

                    # close the Setup/Installer window: Esc on the fixed kernel, Enter fallback
                    $r = Send-MonitorKey -Session $monitor -Keys 'esc'
                    if (-not $r.Ok) { Warn "HMP sendkey esc failed" $r.Error }
                    $exitWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] exit:' -MinCount 1 -TimeoutSeconds 5 -Proc $qmProc
                    if (-not $exitWait.Ok) {
                        Write-Step "Esc did not close the Setup window; falling back to Enter"
                        [void](Send-MonitorKey -Session $monitor -Keys 'ret')
                        $exitWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] exit:' -MinCount 1 -TimeoutSeconds 8 -Proc $qmProc
                    }
                    $m = Get-LastMatch -Text $exitWait.Text -Pattern '\[installer\] exit:[^\r\n]*'
                    if ($m) { $installerExitLine = $m.Value.Trim() }
                    if (-not $exitWait.Ok) { Warn "could not close the Setup window with Esc/Enter" }
                    # Give the guest filesystem a moment to flush the small
                    # os.manifest writeback before QEMU is killed.
                    Start-Sleep -Milliseconds 2500
                }
            }
        }
    }
} finally {
    if ($monitor -and $monitor.Ok) {
        try { [void](Send-MonitorText -Session $monitor -Command 'quit' -TimeoutMs 3000) } catch { }
    }
    if ($monitor -and $monitor.Client) { try { $monitor.Client.Close() } catch { } }
    $clean = Stop-QemuProcesses -Before $qemuBefore -Proc $qmProc
    if (-not $clean) { Warn "a QEMU process may still be running after the kill sweep" }
    if ($qmProc) { Write-Step "QEMU stopped (monitor quit + kill sweep)" }
}

if ($envError) {
    Write-Host ("ERROR: " + $envError)
    Write-Host "SETUP OS SMOKE: FAIL (environment)"
    exit 1
}

# ----------------------------------------------------------------------------
# Phase 4: final serial evidence
# ----------------------------------------------------------------------------
$finalSerialText = Read-TextFileShared -Path $serialPath -Attempts 10
if ($null -eq $finalSerialText) {
    $finalSerialText = ""
    if ($bootResult) { $finalSerialText = $bootResult.Text }
}
if (-not $setupPkgLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[setup\] os pkg:[^\r\n]*'
    if ($m) { $setupPkgLine = $m.Value.Trim() }
}
if (-not $setupCompleteLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[setup\] os install complete:[^\r\n]*'
    if ($m) { $setupCompleteLine = $m.Value.Trim() }
}
if (-not $setupManifestLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[setup\] manifest written:[^\r\n]*'
    if ($m) { $setupManifestLine = $m.Value.Trim() }
}
if (-not $legacyCompleteLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[installer\] Install complete:[^\r\n]*'
    if ($m) { $legacyCompleteLine = $m.Value.Trim() }
}
if (-not $installerReadyLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[installer\] READY:[^\r\n]*'
    if ($m) { $installerReadyLine = $m.Value.Trim() }
}
if (-not $installerExitLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[installer\] exit:[^\r\n]*'
    if ($m) { $installerExitLine = $m.Value.Trim() }
}
$stallCount = Get-PatternCount -Text $finalSerialText -Pattern 'STALL WATCHDOG'
$crashLine = Get-LastMatch -Text $finalSerialText -Pattern 'Panic|Kernel crash|invalid free|General protection|SMASHING|\[desktop\] FATAL'

Write-Host "---- serial evidence ----"
Write-Host ("  installer start : {0}" -f ((Get-PatternCount -Text $finalSerialText -Pattern '\[installer\] start:')))
Write-Host ("  setup os pkg    : {0}" -f $setupPkgLine)
Write-Host ("  setup complete  : {0}" -f $setupCompleteLine)
Write-Host ("  setup manifest  : {0}" -f $setupManifestLine)
Write-Host ("  legacy complete : {0}" -f $legacyCompleteLine)
Write-Host ("  installer ready : {0}" -f $installerReadyLine)
Write-Host ("  installer exit  : {0}" -f $installerExitLine)

# ----------------------------------------------------------------------------
# Phase 5: post-run disk verification (WSL e2tools; copy to /tmp, not "-")
# ----------------------------------------------------------------------------
$diskFiles = @{}
$manifestText = ""
$kernelMagic = ""
$rootTokens = @()
if (-not $SkipDiskVerify) {
    Write-Step "verifying installed OS tree on disk.img"
    $body = @"
img='$diskWsl'
tmp='$wslTmpHost'
rm -rf "`$tmp"; mkdir -p "`$tmp"
echo '=== ROOT ==='
e2ls "`$img:/" 2>&1
echo '=== TREE SYSTEM ==='
e2ls "`$img:/system" 2>&1
echo '=== COPY ==='
for f in system/os.manifest system/boot/kernel system/programs/desktop.elf system/lib/libsky.so; do
  tgt="`$tmp/`$(echo `$f | tr '/' '_')"
  e2cp "`$img:/`$f" "`$tgt" 2>/dev/null; rc=`$?
  if [ `$rc -eq 0 ]; then sz=`$(stat -c %s "`$tgt" 2>/dev/null); else sz=-1; fi
  echo "FILE `$f rc=`$rc size=`$sz"
done
echo '=== MAGIC ==='
if [ -f "`$tmp/system_boot_kernel" ]; then
  echo "KERNEL_MAGIC `$(od -An -tx1 -N4 "`$tmp/system_boot_kernel" | tr -d ' \n')"
fi
if [ -f "`$tmp/system_programs_desktop.elf" ]; then
  echo "DESKTOP_MAGIC `$(od -An -tx1 -N4 "`$tmp/system_programs_desktop.elf" | tr -d ' \n')"
fi
echo '=== MANIFEST ==='
cat "`$tmp/system_os.manifest" 2>/dev/null
echo ''
echo '=== DONE ==='
exit 0
"@
    $post = Invoke-WslScript -Body $body
    if (-not $post.Ok) {
        Report "E0) disk.img post-run read (WSL e2tools)" $false `
            ("WSL rc={0}: {1}" -f $post.Code, (Format-MonitorText $post.Error))
    } else {
        $rootMatch = [regex]::Match($post.Output, '(?s)=== ROOT ===(.*?)=== TREE SYSTEM ===')
        $rootTokens = @(Get-TokensFromListing -Listing $rootMatch.Groups[1].Value)
        $fileMatches = [regex]::Matches($post.Output, '(?m)^FILE (\S+) rc=(-?\d+) size=(-?\d+)\s*$')
        for ($i = 0; $i -lt $fileMatches.Count; $i++) {
            $m = $fileMatches[$i]
            $diskFiles[$m.Groups[1].Value] = [pscustomobject]@{ Rc = [int]$m.Groups[2].Value; Size = [int64]$m.Groups[3].Value }
        }
        $magicMatch = [regex]::Match($post.Output, '(?m)^KERNEL_MAGIC ([0-9a-fA-F]+)\s*$')
        if ($magicMatch.Success) { $kernelMagic = $magicMatch.Groups[1].Value.ToLowerInvariant() }
        $manMatch = [regex]::Match($post.Output, '(?s)=== MANIFEST ===(.*?)=== DONE ===')
        if ($manMatch.Success) { $manifestText = $manMatch.Groups[1].Value }
    }
} else {
    Warn "disk verification skipped (-SkipDiskVerify)"
}

# ----------------------------------------------------------------------------
# Phase 6: assertions
# ----------------------------------------------------------------------------
# --- (A) Setup/Installer serial evidence ---
Report "A1) Installer/Setup process started ([installer] start:)" `
    ($installerStartCount -ge 1 -or [bool]$setupPkgLine -or [bool]$legacyCompleteLine) `
    ("[installer] start: count={0}; setupPkg={1}; legacyComplete={2}" -f $installerStartCount, [bool]$setupPkgLine, [bool]$legacyCompleteLine)
if ($setupPkgLine) {
    Report "A2) Setup reported the OS package ([setup] os pkg:)" $true $setupPkgLine
} else {
    Warn "A2) [setup] os pkg: marker missing" "old Installer build? (legacy Install complete fallback accepted)"
}
$setupFilesCount = -1
$setupFailedCount = -1
if ($setupCompleteLine) {
    $mCount = [regex]::Match($setupCompleteLine, 'files=(\d+)')
    if (-not $mCount.Success) { $mCount = [regex]::Match($setupCompleteLine, '(\d+)\s*files') }
    if (-not $mCount.Success) { $mCount = [regex]::Match($setupCompleteLine, 'installed[= ]+(\d+)') }
    if ($mCount.Success) { $setupFilesCount = [int]$mCount.Groups[1].Value }
    $mFail = [regex]::Match($setupCompleteLine, 'failed=(\d+)')
    if ($mFail.Success) { $setupFailedCount = [int]$mFail.Groups[1].Value }
    $setupOk = ($setupFailedCount -eq 0)
    Report "A3) Setup OS install complete with failed=0" $setupOk `
        ("files={0} failed={1} line: {2}" -f $setupFilesCount, $setupFailedCount, $setupCompleteLine)
    if ($setupFilesCount -lt 1) { Warn "A3) installed file count not parsed from the setup line" $setupCompleteLine }
} elseif ($legacyCompleteLine) {
    Warn "A3) new [setup] os install complete marker missing" ("legacy installer line found: " + $legacyCompleteLine)
} else {
    Report "A3) Setup OS install complete with failed=0" $false "no [setup] os install complete / [installer] Install complete marker"
}
$manifestWrittenOk = ($null -ne $setupManifestLine) -or ($manifestText -match 'SKYOS1 installed')
Report "A4) Setup wrote /mp/system/os.manifest" $manifestWrittenOk `
    ($(if ($setupManifestLine) { $setupManifestLine } else { "manifest line missing (post-run manifest content: " + (($manifestText -replace '\s+', ' ').Trim()) + ")" }))
Report "A5) Installer handshake READY ([installer] READY:)" `
    ($null -ne $installerReadyLine) $installerReadyLine
if ($installerExitLine) {
    Info ("Setup window closed: " + $installerExitLine)
} else {
    Warn "A6) no [installer] exit: marker" "Esc/Enter close was not confirmed (window may still have been open)"
}
Report "A7) no kernel panic / STALL WATCHDOG during the install" `
    (($stallCount -eq 0) -and ($null -eq $crashLine)) `
    ("stall={0}; crash={1}" -f $stallCount, $(if ($crashLine) { $crashLine.Value.Trim() } else { "none" }))
$shotInst = $shots['installer']
Report "A8) Setup screendump captured (setup_os_installer.ppm)" `
    ($null -ne $shotInst -and $shotInst.Ok) `
    ($(if ($shotInst) { $shotInst.Path } else { "no screenshot" }))

# --- (E) disk tree checks ---
function Get-DiskFileEntry {
    param([hashtable]$Files, [string]$ImagePath)
    if ($Files.ContainsKey($ImagePath)) { return $Files[$ImagePath] }
    return $null
}
$manifestOk = $false
$manifestDetail = "manifest not read"
$manifestTotalFiles = -1
$manifestInstalledBytes = -1
if ($manifestText) {
    $hasBanner = ($manifestText -match 'SKYOS1 installed')
    $mTotal = [regex]::Match($manifestText, 'total_files=(\d+)')
    if ($mTotal.Success) { $manifestTotalFiles = [int]$mTotal.Groups[1].Value }
    $mBytes = [regex]::Match($manifestText, 'installed_bytes=(\d+)')
    if ($mBytes.Success) { $manifestInstalledBytes = [int64]$mBytes.Groups[1].Value }
    $manifestOk = $hasBanner -and ($manifestTotalFiles -ge 0) -and ($manifestInstalledBytes -ge 0)
    $manifestDetail = ("SKYOS1={0} total_files={1} installed_bytes={2}" -f $hasBanner, $manifestTotalFiles, $manifestInstalledBytes)
}
Report-Disk "E1) /mp/system/os.manifest contains SKYOS1 installed + totals" $manifestOk $manifestDetail

$kernelEntry = Get-DiskFileEntry -Files $diskFiles -ImagePath 'system/boot/kernel'
$kernelOk = ($null -ne $kernelEntry) -and ($kernelEntry.Rc -eq 0) -and ($kernelEntry.Size -gt 1048576)
$kernelDetail = ("rc={0} size={1} magic={2}" -f $(if ($kernelEntry) { $kernelEntry.Rc } else { "?" }), $(if ($kernelEntry) { $kernelEntry.Size } else { "?" }), $(if ($kernelMagic) { $kernelMagic } else { "?" }))
Report-Disk "E2) /mp/system/boot/kernel exists, >1MiB and starts with ELF magic" `
    ($kernelOk -and ($kernelMagic -eq '7f454c46')) $kernelDetail

$desktopEntry = Get-DiskFileEntry -Files $diskFiles -ImagePath 'system/programs/desktop.elf'
$desktopOk = ($null -ne $desktopEntry) -and ($desktopEntry.Rc -eq 0) -and ($desktopEntry.Size -gt 102400)
Report-Disk "E3) /mp/system/programs/desktop.elf exists and >100KiB" $desktopOk `
    ("rc={0} size={1}" -f $(if ($desktopEntry) { $desktopEntry.Rc } else { "?" }), $(if ($desktopEntry) { $desktopEntry.Size } else { "?" }))

$libEntry = Get-DiskFileEntry -Files $diskFiles -ImagePath 'system/lib/libsky.so'
$libOk = ($null -ne $libEntry) -and ($libEntry.Rc -eq 0) -and ($libEntry.Size -gt 102400)
Report-Disk "E4) /mp/system/lib/libsky.so exists and >100KiB" $libOk `
    ("rc={0} size={1}" -f $(if ($libEntry) { $libEntry.Rc } else { "?" }), $(if ($libEntry) { $libEntry.Size } else { "?" }))

if (($setupFilesCount -ge 0) -and ($manifestTotalFiles -ge 0)) {
    Report-Disk "E5) Setup file count matches manifest total_files (tolerance 0)" `
        ($setupFilesCount -eq $manifestTotalFiles) `
        ("setup_files={0} manifest_total_files={1}" -f $setupFilesCount, $manifestTotalFiles)
} else {
    Warn "E5) file-count cross-check skipped" ("setup_files={0} manifest_total_files={1}" -f $setupFilesCount, $manifestTotalFiles)
}
$systemTokens = @()
if ($rootTokens -contains 'system') { $systemTokens += 'system' }
Report-Disk "E6) /mp/system directory exists on disk.img" ($systemTokens.Count -ge 1) `
    ("root entries: {0}" -f (($rootTokens | Select-Object -First 20) -join ' '))
if (-not $SkipDiskVerify) {
    if ($rootTokens -contains 'os.pkg') {
        Info "package source /mp/os.pkg is still present on disk.img (kept, not deleted)"
    } else {
        Warn "package source /mp/os.pkg not found on the disk root" "the Setup may stage it from the ISO or another path"
    }
}

# ----------------------------------------------------------------------------
# Verdict
# ----------------------------------------------------------------------------
Write-Host "---- evidence paths ----"
Write-Host ("  serial log : {0}" -f $serialPath)
foreach ($key in @('base', 'installer')) {
    if ($shots.ContainsKey($key) -and $shots[$key]) { Write-Host ("  {0,-10}: {1}" -f $key, $shots[$key].Path) }
}
Write-Host "---- key serial markers ----"
foreach ($line in (Get-LastLines $finalSerialText 500)) {
    if ($line -match '\[installer\]|\[setup\]|\[desktop\] theme: load|\[desktop\] start: fb=') { Write-Host ("  " + $line) }
}

if ($script:FailCount -gt 0) {
    Write-Host "---- serial log tail (last 40 lines) ----"
    foreach ($line in (Get-LastLines $finalSerialText 40)) { Write-Host ("  " + $line) }
    Write-Host ("SETUP OS SMOKE: FAIL ({0} assertion(s) failed, {1} warning(s))" -f $script:FailCount, $script:WarnCount)
    exit 1
}
Write-Host ("SETUP OS SMOKE: PASS ({0} warning(s))" -f $script:WarnCount)
exit 0
