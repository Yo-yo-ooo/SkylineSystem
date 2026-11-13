#Requires -Version 5.1
# ============================================================================
# res/scripts/test/doom_compact_smoke.ps1
#   Headless QEMU gate for DOOM on the DESKTOP-ON-DEMAND path and for the new
#   DOOM_TINY / compact mode.
#
# WHAT IT PROVES
#   A) The desktop no longer auto-spawns DOOM; it prints
#        [desktop] DOOM on demand (Ctrl+Alt+D, mem=<n> MiB[ , low-memory])
#      and Ctrl+Alt+D really spawns the game ([wm] spawn DOOM).
#   B) DOOM starts in compact mode when DOOM_TINY is enabled:
#        [doom] compact=1        (WARN if the running build has no such marker)
#        [doom] zone_mb=<n>      (zone tier chosen from machine memory)
#        [doom] init done
#        [doom] f=<frames> ms=<ms> seq=<seq>   (at least 2 heartbeats)
#   C) No kernel panic / Kernel crash / STALL WATCHDOG / invalid free.
#   D) A screendump proves the DOOM window is on screen: its title bar uses the
#      running window-style title colour, the frame changes compared with the
#      pre-DOOM baseline, and the content area has visible (non-black) pixels.
#
# 64 MiB MODE (-MemMB 64)
#   The same flow with longer waits (>= 80 s for init) and the OOM line count is
#   recorded as INFO (OOM is expected on the 64 MiB target, it must NOT fail the
#   gate); panic / stall / missing heartbeats still fail.
#
# FLOW
#   1) WSL e2tools preprocess: verify /mp/doom.elf and /mp/doom.wad exist on
#      disk.img (keep them).  With -TrimmedWad <file> the host file replaces
#      /mp/doom.wad on the image (trimmed WAD experiment); -KeepWad skips that.
#   2) boot QEMU headless, wait for the desktop ready markers.
#   3) base screendump, HMP "sendkey ctrl-alt-d", wait for the DOOM serial
#      markers / heartbeats.
#   4) screendump doom_compact.ppm, pixel assertions, quit + kill sweep.
#
# OUTPUT (under <Root> unless overridden)
#   doom_compact_serial.log
#   doom_compact_base.ppm      desktop before Ctrl+Alt+D
#   doom_compact.ppm           frame with the DOOM window
#
# EXIT CODES: 0 = PASS (WARNs allowed), 1 = assertion/environment failure.
# NOTE: builds nothing; it only boots the already-built ISO/disk.img.
# ============================================================================
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [string]$Iso = "",
    [string]$Disk = "",
    [int]$MemMB = 2048,
    [int]$WaitSeconds = 120,
    [int]$ActionWaitSeconds = 30,
    [int]$InitWaitSeconds = 90,
    [int]$HeartbeatWaitSeconds = 60,
    [int]$Smp = 4,
    [int]$ScreenshotDelay = 1,
    [int]$Tolerance = 12,
    [int]$MinDiffCells = 12,
    [string]$SerialLog = "",
    [string]$ScreenshotPrefix = "",
    [string]$WslDistro = "Ubuntu-24.04",
    [string]$TrimmedWad = "",
    [switch]$KeepWad,
    [switch]$SkipPreprocess,
    [int]$MonitorPort = 0
)

$ErrorActionPreference = "Continue"
$script:FailCount = 0
$script:WarnCount = 0
$script:WslSeq = 0
$script:WslTmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sky_doom_wsl_{0}" -f $PID)

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
$serialPath = if ($SerialLog) { $SerialLog } else { Join-Path $root "doom_compact_serial.log" }
$shotPrefix = if ($ScreenshotPrefix) { $ScreenshotPrefix } else { Join-Path $root "doom_compact" }
try { $serialPath = [System.IO.Path]::GetFullPath($serialPath) } catch { }
if ($shotPrefix -notmatch '^[A-Za-z]:') { try { $shotPrefix = [System.IO.Path]::GetFullPath($shotPrefix) } catch { } }
$wslTmpHost = "/tmp/sky_doom_smoke"

# 64 MiB target: the desktop/DOOM need much longer to come up and to init.
$lowMem = ($MemMB -le 96)
if ($lowMem) {
    if ($WaitSeconds -lt 180) { $WaitSeconds = 180 }
    if ($InitWaitSeconds -lt 180) { $InitWaitSeconds = 180 }
    if ($HeartbeatWaitSeconds -lt 120) { $HeartbeatWaitSeconds = 120 }
}

# ============================================================================
# SHARED HELPERS (pure functions; no side effects beyond stdout)
# ============================================================================
function Write-Step { param([string]$Text) Write-Host ("DOOM COMPACT SMOKE: {0}" -f $Text) }
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
function Format-Rgb {
    param($Color)
    if ($null -eq $Color) { return "<none>" }
    return ("RGB({0},{1},{2}) #{3:X2}{4:X2}{5:X2}" -f `
        [int]$Color.R, [int]$Color.G, [int]$Color.B, [int]$Color.R, [int]$Color.G, [int]$Color.B)
}
function Format-RgbDelta {
    param($Color, $Expected)
    if ($null -eq $Color -or $null -eq $Expected) { return "d=<n/a>" }
    return ("d=({0},{1},{2})" -f ([int]$Color.R - [int]$Expected.R), ([int]$Color.G - [int]$Expected.G), ([int]$Color.B - [int]$Expected.B))
}
function New-Rgb {
    param([int]$R, [int]$G, [int]$B)
    return [pscustomobject]@{ R = [int]$R; G = [int]$G; B = [int]$B }
}
function Convert-Hex6ToRgb {
    param([string]$Hex)
    if (-not $Hex) { return $null }
    if ($Hex -notmatch '^[0-9A-Fa-f]{6}$') { return $null }
    return (New-Rgb ([Convert]::ToInt32($Hex.Substring(0, 2), 16)) ([Convert]::ToInt32($Hex.Substring(2, 2), 16)) ([Convert]::ToInt32($Hex.Substring(4, 2), 16)))
}
function Test-RgbNear {
    param($A, $B, [int]$Tolerance = 12)
    if ($null -eq $A -or $null -eq $B) { return $false }
    return (([math]::Abs([int]$A.R - [int]$B.R) -le $Tolerance) -and
            ([math]::Abs([int]$A.G - [int]$B.G) -le $Tolerance) -and
            ([math]::Abs([int]$A.B - [int]$B.B) -le $Tolerance))
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
    return [int]([System.Text.Encoding]::ASCII.GetString($Bytes, $start, $State.Pos - $start))
}
function Read-PpmFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw ("PPM file not found: {0}" -f $Path) }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $state = @{ Pos = 0; Len = [int]$bytes.Length }
    Skip-PpmSeparators -Bytes $bytes -State $state
    if (($state.Pos + 1) -ge $state.Len) { throw "PPM too short" }
    $c0 = [char][int]$bytes[$state.Pos]; $c1 = [char][int]$bytes[$state.Pos + 1]
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
    return [pscustomobject]@{ Width = [int]$w; Height = [int]$h; MaxVal = [int]$maxv; Bytes = $bytes; DataStart = $dataStart; Path = $Path }
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
    $x0 = [math]::Max(0, $X - $Radius); $x1 = [math]::Min([int]$Ppm.Width - 1, $X + $Radius)
    $y0 = [math]::Max(0, $Y - $Radius); $y1 = [math]::Min([int]$Ppm.Height - 1, $Y + $Radius)
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
function Get-SerialSnapshot { param([string]$Path, [int]$Attempts = 2) return (Read-TextFileShared -Path $Path -Attempts $Attempts) }
function Get-PatternCount {
    param([string]$Text, [string]$Pattern)
    if (-not $Text) { return 0 }
    return ([regex]::Matches($Text, $Pattern)).Count
}
function Wait-SerialAtLeast {
    param([string]$Path, [string]$Pattern, [int]$MinCount = 1, [int]$TimeoutSeconds = 30, $Proc = $null)
    $startTime = Get-Date
    $deadline = $startTime.AddSeconds($TimeoutSeconds)
    $text = ""
    while ((Get-Date) -lt $deadline) {
        $snap = Get-SerialSnapshot -Path $Path -Attempts 1
        if ($null -ne $snap) { $text = $snap }
        $count = Get-PatternCount -Text $text -Pattern $Pattern
        if ($count -ge $MinCount) { return [pscustomobject]@{ Ok = $true; Count = $count; Text = $text; Elapsed = ((Get-Date) - $startTime).TotalSeconds } }
        if ($Proc) { try { if ($Proc.HasExited) { break } } catch { } }
        Start-Sleep -Milliseconds 300
    }
    return [pscustomobject]@{ Ok = $false; Count = (Get-PatternCount -Text $text -Pattern $Pattern); Text = $text; Elapsed = ((Get-Date) - $startTime).TotalSeconds }
}
function Get-StyleValues {
    param([string]$Text)
    $m = Get-LastMatch -Text $Text -Pattern '\[desktop\] lua: style radius=(\d+) title=0x([0-9A-Fa-f]{6}) border=0x([0-9A-Fa-f]{6}) ink=0x([0-9A-Fa-f]{6}) close=0x([0-9A-Fa-f]{6}) paper=0x([0-9A-Fa-f]{6}) shadow=(\d+)'
    if (-not $m) { return $null }
    return [pscustomobject]@{ Radius = [int]$m.Groups[1].Value; Title = (Convert-Hex6ToRgb $m.Groups[2].Value); Paper = (Convert-Hex6ToRgb $m.Groups[6].Value) }
}

# ---------------------------------------------------------------------------
# WSL e2tools helpers
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
        $p = Start-Process -FilePath 'wsl.exe' -ArgumentList @('-d', $WslDistro, 'bash', $wslShArg) `
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
    if ($m.Success) { $rc = [int]$m.Groups[1].Value; $out = $out.Substring(0, $m.Index) }
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
    } catch { if ($listener) { try { $listener.Stop() } catch { } } }
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
    if ($Proc) { try { if (-not $Proc.HasExited) { $Proc.Kill(); [void]$Proc.WaitForExit(3000) } } catch { } }
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        $newProcs = @(Get-QemuProcessIds | Where-Object { $Before -notcontains $_ })
        if ($newProcs.Count -eq 0) { return $true }
        foreach ($qpid in $newProcs) { try { $qp = Get-Process -Id $qpid -ErrorAction SilentlyContinue; if ($qp) { $qp.Kill() } } catch { } }
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
function Send-MonitorText { param($Session, [string]$Command, [int]$TimeoutMs = 8000) return (Invoke-MonitorCommand -Session $Session -Command $Command -TimeoutMs $TimeoutMs) }
function Get-ScreendumpStagingPath {
    param([string]$RequestedPath)
    if ($RequestedPath -and ($RequestedPath -notmatch '\s')) { return $RequestedPath }
    try {
        $tmp = [System.IO.Path]::GetTempPath()
        if ($tmp) { return [System.IO.Path]::Combine($tmp, ("doom_compact_shot_{0}.ppm" -f $PID)) }
    } catch { }
    return $null
}
function Save-Screenshot {
    param($Session, [string]$Path, [int]$Retries = 3)
    if ($null -eq $Session -or -not $Session.Ok) { return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = "no monitor session" } }
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
            try { return [pscustomobject]@{ Ok = $true; Path = $Path; Ppm = (Read-PpmFile -Path $Path); Error = "" } }
            catch { return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = ("PPM parse failed: " + $_.Exception.Message) } }
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
    Write-Host "DOOM COMPACT SMOKE: FAIL (environment)"
    exit 1
}

# ----------------------------------------------------------------------------
# Phase 1: environment
# ----------------------------------------------------------------------------
Write-Host "DOOM COMPACT SMOKE: ==================================================="
Write-Host ("DOOM COMPACT SMOKE: DOOM on-demand / DOOM_TINY compact gate (mem={0}MiB{1})" -f $MemMB, $(if ($lowMem) { ", low-memory mode" } else { "" }))
Write-Host "DOOM COMPACT SMOKE: root = $root"
Write-Host "DOOM COMPACT SMOKE: qemu = $qemu"
Write-Host "DOOM COMPACT SMOKE: iso  = $isoPath"
Write-Host "DOOM COMPACT SMOKE: disk = $diskPath"
Write-Host "DOOM COMPACT SMOKE: serial = $serialPath"
Write-Host "DOOM COMPACT SMOKE: shots  = ${shotPrefix}_*.ppm"

if (-not (Test-Path -LiteralPath $qemu)) { Exit-Environment ("QEMU not found: " + $qemu) }
if (-not (Test-Path -LiteralPath $isoPath)) { Exit-Environment ("ISO not found, build it first: " + $isoPath) }
if (-not (Test-Path -LiteralPath $diskPath)) { Exit-Environment ("disk.img not found: " + $diskPath) }
$existingQemu = @(Get-QemuProcessIds)
if ($existingQemu.Count -gt 0) { Exit-Environment ("another QEMU is already running (PID: {0}); stop it first" -f ($existingQemu -join ', ')) }
if (-not $SkipPreprocess) {
    if (-not (Test-WslE2Tools -Distro $WslDistro)) { Exit-Environment ("WSL distro '{0}' with e2tools (e2ls/e2rm/e2cp) is required" -f $WslDistro) }
    Write-Host "DOOM COMPACT SMOKE: WSL e2tools OK (distro=$WslDistro)"
}
$serialDir = Split-Path -Parent $serialPath
if ($serialDir -and -not (Test-Path -LiteralPath $serialDir)) { New-Item -ItemType Directory -Force -Path $serialDir | Out-Null }
Remove-Item -LiteralPath $serialPath -Force -ErrorAction SilentlyContinue
foreach ($suffix in @('base', '')) {
    $name = if ($suffix) { "{0}_{1}.ppm" -f $shotPrefix, $suffix } else { "{0}.ppm" -f $shotPrefix }
    Remove-Item -LiteralPath $name -Force -ErrorAction SilentlyContinue
}
$diskWsl = ConvertTo-WslPath $diskPath

# ----------------------------------------------------------------------------
# Phase 2: preprocess disk.img (keep /mp/doom.elf + /mp/doom.wad)
# ----------------------------------------------------------------------------
$wadSizeBytes = -1
$doomElfPresent = $false
$wadPresent = $false
if (-not $SkipPreprocess) {
    Write-Step "checking disk.img assets (keep /mp/doom.elf and /mp/doom.wad)"
    $body = @"
img='$diskWsl'
tmp='$wslTmpHost'
rm -rf "`$tmp"; mkdir -p "`$tmp"
echo '=== ROOT ==='
e2ls "`$img:/" 2>&1
for f in doom.elf doom.wad; do
  tgt="`$tmp/`$f"
  e2cp "`$img:/`$f" "`$tgt" 2>/dev/null; rc=`$?
  if [ `$rc -eq 0 ]; then sz=`$(stat -c %s "`$tgt" 2>/dev/null); else sz=-1; fi
  echo "ASSET `$f rc=`$rc size=`$sz"
done
echo '=== DONE ==='
exit 0
"@
    $pre = Invoke-WslScript -Body $body
    if (-not $pre.Ok) {
        Write-Host $pre.Output
        Exit-Environment ("disk.img asset check failed (WSL rc={0}): {1}" -f $pre.Code, (Format-MonitorText $pre.Error))
    }
    $assetMatches = [regex]::Matches($pre.Output, '(?m)^ASSET (\S+) rc=(-?\d+) size=(-?\d+)\s*$')
    for ($i = 0; $i -lt $assetMatches.Count; $i++) {
        $m = $assetMatches[$i]
        if ($m.Groups[1].Value -eq 'doom.elf' -and [int]$m.Groups[2].Value -eq 0 -and [int64]$m.Groups[3].Value -gt 0) { $doomElfPresent = $true }
        if ($m.Groups[1].Value -eq 'doom.wad' -and [int]$m.Groups[2].Value -eq 0 -and [int64]$m.Groups[3].Value -gt 0) {
            $wadPresent = $true
            $wadSizeBytes = [int64]$m.Groups[3].Value
        }
    }
    $rootMatch = [regex]::Match($pre.Output, '(?s)=== ROOT ===(.*?)ASSET ')
    if (-not $rootMatch.Success) { $rootMatch = [regex]::Match($pre.Output, '(?s)=== ROOT ===(.*?)=== DONE ===') }
    Write-Step ("disk assets: doom.elf={0} doom.wad={1} ({2} bytes)" -f $doomElfPresent, $wadPresent, $wadSizeBytes)
    if (-not $doomElfPresent -or -not $wadPresent) {
        Write-Host $pre.Output
        Exit-Environment ("disk.img is missing the DOOM assets (need /mp/doom.elf and /mp/doom.wad)")
    }
    if ($TrimmedWad -and -not $KeepWad) {
        if (-not (Test-Path -LiteralPath $TrimmedWad)) { Exit-Environment ("-TrimmedWad file not found: " + $TrimmedWad) }
        Write-Step ("replacing /mp/doom.wad with the trimmed WAD: {0}" -f $TrimmedWad)
        $trimWsl = ConvertTo-WslPath $TrimmedWad
        $body2 = @"
img='$diskWsl'
tmp='$wslTmpHost'
rm -f "`$tmp/trimmed.wad"
e2cp '$trimWsl' "`$tmp/trimmed.wad"; rc=`$?
if [ `$rc -ne 0 ]; then echo "COPYIN rc=`$rc"; exit 1; fi
sz=`$(stat -c %s "`$tmp/trimmed.wad")
e2rm "`$img:/doom.wad" 2>/dev/null
e2cp "`$tmp/trimmed.wad" "`$img:/doom.wad"; rc=`$?
echo "REPLACE rc=`$rc size=`$sz"
exit `$rc
"@
        $rep = Invoke-WslScript -Body $body2
        if (-not $rep.Ok) {
            Write-Host $rep.Output
            Exit-Environment ("failed to install the trimmed WAD into disk.img (WSL rc={0})" -f $rep.Code)
        }
        $repMatch = [regex]::Match($rep.Output, 'REPLACE rc=(\d+) size=(\d+)')
        if ($repMatch.Success) { $wadSizeBytes = [int64]$repMatch.Groups[2].Value }
        Write-Step ("trimmed WAD installed: {0} bytes" -f $wadSizeBytes)
    } elseif ($TrimmedWad -and $KeepWad) {
        Warn "trimmed WAD ignored (-KeepWad)" $TrimmedWad
    }
} else {
    Warn "disk asset check skipped (-SkipPreprocess)" "assuming /mp/doom.elf and /mp/doom.wad exist"
    $doomElfPresent = $true
    $wadPresent = $true
}

# ----------------------------------------------------------------------------
# Phase 3: QEMU boot + Ctrl+Alt+D + DOOM markers/screenshot
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
Write-Host ("DOOM COMPACT SMOKE: command = {0} {1}" -f $qemu, ($quotedArgs -join ' '))

$qemuBefore = @(Get-QemuProcessIds)
$qmProc = $null
$monitor = $null
$envError = $null
$bootResult = $null
$shots = @{}
$finalSerialText = ""
$onDemandLine = $null
$compactLine = $null
$zoneLine = $null
$memLine = $null
$initLine = $null
$mirrorSize = -1
$heartbeatCount = 0
$oomCount = 0
$spawnDoomCount = 0

try {
    try { $qmProc = Start-Process -FilePath $qemu -ArgumentList $quotedArgs -PassThru -WindowStyle Hidden }
    catch { $envError = ("failed to start QEMU: " + $_.Exception.Message) }
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
                foreach ($spec in $bootSpecs) { if ((Get-PatternCount -Text $bootText -Pattern $spec.Pattern) -lt 1) { $missing += $spec.Name } }
                if ($missing.Count -eq 0) { break }
                $fatal = Get-LastMatch -Text $bootText -Pattern 'Kernel panic|Kernel crash|General protection|SMASHING|STALL WATCHDOG'
                if ($fatal) { break }
                try { if ($qmProc.HasExited) { break } } catch { }
                Start-Sleep -Milliseconds 400
            }
            $bootResult = [pscustomobject]@{ Ok = ($missing.Count -eq 0); Missing = $missing; Text = $bootText; Elapsed = ((Get-Date) - $bootStart).TotalSeconds }
            if ($bootResult.Ok) {
                Write-Step ("boot markers complete after {0:N1}s" -f $bootResult.Elapsed)
                Start-Sleep -Seconds ([int][math]::Max(1, $ScreenshotDelay))
            } else {
                Write-Host ("DOOM COMPACT SMOKE: boot marker miss: {0}" -f ($missing -join ', '))
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
                    $onDemandCheck = Wait-SerialAtLeast -Path $serialPath -Pattern '\[desktop\] DOOM on demand' -MinCount 1 -TimeoutSeconds 10 -Proc $qmProc
                    $snapNow = Get-SerialSnapshot -Path $serialPath -Attempts 3
                    $m = Get-LastMatch -Text $snapNow -Pattern '\[desktop\] DOOM on demand[^\r\n]*'
                    if ($m) { $onDemandLine = $m.Value.Trim() }
                    $hints = Get-PatternCount -Text $snapNow -Pattern 'DOOM skipped|DOOM on demand'
                    Info ("desktop DOOM policy: on-demand marker={0}; related lines={1}" -f ($null -ne $onDemandLine), $hints)

                    $r = Send-MonitorKey -Session $monitor -Keys 'ctrl-alt-d'
                    if (-not $r.Ok) { Warn "HMP sendkey ctrl-alt-d failed" $r.Error }
                    $clientWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[doom\] client start' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $qmProc
                    $zoneWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[doom\] zone_mb=' -MinCount 1 -TimeoutSeconds $InitWaitSeconds -Proc $qmProc
                    $compactWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[doom\] compact=1' -MinCount 1 -TimeoutSeconds $InitWaitSeconds -Proc $qmProc
                    $initWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[doom\] init done' -MinCount 1 -TimeoutSeconds $InitWaitSeconds -Proc $qmProc
                    $hbWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[doom\] f=\d+' -MinCount 2 -TimeoutSeconds $HeartbeatWaitSeconds -Proc $qmProc
                    $snap = Get-SerialSnapshot -Path $serialPath -Attempts 3
                    $finalSerialText = $snap
                    $m = Get-LastMatch -Text $snap -Pattern '\[doom\] compact=1[^\r\n]*'
                    if ($m) { $compactLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[doom\] zone_mb=[^\r\n]*'
                    if ($m) { $zoneLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[doom\] mem_total_mb=[^\r\n]*'
                    if ($m) { $memLine = $m.Value.Trim() }
                    $m = Get-LastMatch -Text $snap -Pattern '\[doom\] init done[^\r\n]*'
                    if ($m) { $initLine = $m.Value.Trim() }
                    $mirrorMatch = [regex]::Match($snap, '\[doom\] mirror=(\d+)')
                    if ($mirrorMatch.Success) { $mirrorSize = [int64]$mirrorMatch.Groups[1].Value }
                    $heartbeatCount = Get-PatternCount -Text $snap -Pattern '\[doom\] f=\d+'
                    $oomCount = Get-PatternCount -Text $snap -Pattern 'OOM'
                    $spawnDoomCount = Get-PatternCount -Text $snap -Pattern '\[wm\] spawn DOOM'
                    Info ("doom: client_start={0} zone_mb_seen={1} init_done={2} heartbeats={3} OOM_lines={4} spawn_lines={5}" -f `
                        $clientWait.Ok, $zoneWait.Ok, $initWait.Ok, $heartbeatCount, $oomCount, $spawnDoomCount)

                    if ($hbWait.Ok) { Start-Sleep -Milliseconds 1200 }
                    $shotDoom = Save-Screenshot -Session $monitor -Path ("{0}.ppm" -f $shotPrefix)
                    $shots['doom'] = $shotDoom
                    $styleVals = Get-StyleValues -Text $snap
                    if ($shotDoom.Ok -and $shotBase.Ok -and $styleVals) {
                        $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shotBase.Ppm) -B (Get-PpmSignature -Ppm $shotDoom.Ppm)
                        # DOOM body 640x440 spawned at dx=120 dy=60 (centered surface)
                        $bodyX = [int]((1280 - (640 + 52)) / 2) + 120
                        $bodyY = [int]((800 - (440 + 52)) / 2) + 60
                        $titleX = $bodyX + [int](640 / 2)
                        $titleY = $bodyY + 8
                        $titlePx = Get-PpmRegionAverage -Ppm $shotDoom.Ppm -X $titleX -Y $titleY -Radius 2
                        $colored = 0
                        $total = 0
                        for ($gy = 0; $gy -lt 6; $gy++) {
                            for ($gx = 0; $gx -lt 8; $gx++) {
                                $sx = $bodyX + 20 + [int]((640 - 40) * $gx / 7)
                                $sy = $bodyY + 40 + [int]((440 - 70) * $gy / 5)
                                $px = Get-PpmPixelRgb -Ppm $shotDoom.Ppm -X $sx -Y $sy
                                $total++
                                if ($null -ne $px -and (([int]$px.R + [int]$px.G + [int]$px.B) -ge 30)) { $colored++ }
                            }
                        }
                        Report "E1) DOOM window appeared (frame changed after Ctrl+Alt+D)" `
                            ($diff.DiffCells -ge $MinDiffCells) `
                            ("{0}/{1} grid cells changed (threshold {2}); [wm] spawn DOOM lines={3}" -f $diff.DiffCells, $diff.Total, $MinDiffCells, $spawnDoomCount)
                        Report "E2) DOOM title bar uses the running style title" `
                            (Test-RgbNear $titlePx $styleVals.Title $Tolerance) `
                            ("sampled {0} at ({1},{2}) vs style title {3} ({4})" -f (Format-Rgb $titlePx), $titleX, $titleY, (Format-Rgb $styleVals.Title), (Format-RgbDelta $titlePx $styleVals.Title))
                        Report "E3) DOOM content area shows visible (non-black) pixels" ($colored -ge 6) `
                            ("{0}/{1} sampled content points have non-black pixels (DOOM title screen / status bar)" -f $colored, $total)
                    } else {
                        Report "E1) DOOM frame captured and parsed" ($null -ne $shotDoom -and $shotDoom.Ok) `
                            ($(if ($shotDoom) { $shotDoom.Error } else { "no screenshot" }))
                    }
                    if (-not $compactWait.Ok) {
                        Warn "A2) [doom] compact=1 marker missing" "DOOM_TINY not enabled in this build? (WARN only per task)"
                    }
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
    Write-Host "DOOM COMPACT SMOKE: FAIL (environment)"
    exit 1
}

# ----------------------------------------------------------------------------
# Phase 4: final serial evidence + assertions
# ----------------------------------------------------------------------------
$finalSerialText = Read-TextFileShared -Path $serialPath -Attempts 10
if ($null -eq $finalSerialText) {
    $finalSerialText = ""
    if ($bootResult) { $finalSerialText = $bootResult.Text }
}
if (-not $onDemandLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[desktop\] DOOM on demand[^\r\n]*'
    if ($m) { $onDemandLine = $m.Value.Trim() }
}
if (-not $compactLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[doom\] compact=1[^\r\n]*'
    if ($m) { $compactLine = $m.Value.Trim() }
}
if (-not $zoneLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[doom\] zone_mb=[^\r\n]*'
    if ($m) { $zoneLine = $m.Value.Trim() }
}
if (-not $memLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[doom\] mem_total_mb=[^\r\n]*'
    if ($m) { $memLine = $m.Value.Trim() }
}
if (-not $initLine) {
    $m = Get-LastMatch -Text $finalSerialText -Pattern '\[doom\] init done[^\r\n]*'
    if ($m) { $initLine = $m.Value.Trim() }
}
if ($heartbeatCount -lt 2) { $heartbeatCount = (Get-PatternCount -Text $finalSerialText -Pattern '\[doom\] f=\d+') }
if ($mirrorSize -lt 0) {
    $mm = [regex]::Match($finalSerialText, '\[doom\] mirror=(\d+)')
    if ($mm.Success) { $mirrorSize = [int64]$mm.Groups[1].Value }
}
$oomCount = (Get-PatternCount -Text $finalSerialText -Pattern 'OOM')
$crashLine = Get-LastMatch -Text $finalSerialText -Pattern 'Panic|Kernel crash|STALL WATCHDOG|invalid free|General protection|SMASHING|\[desktop\] FATAL'
$zoneValue = -1
if ($zoneLine) {
    $zm = [regex]::Match($zoneLine, 'zone_mb=(\d+)')
    if ($zm.Success) { $zoneValue = [int]$zm.Groups[1].Value }
}

Write-Host "---- serial evidence ----"
Write-Host ("  doom on-demand  : {0}" -f $onDemandLine)
Write-Host ("  compact marker  : {0}" -f $compactLine)
Write-Host ("  zone marker     : {0}" -f $zoneLine)
Write-Host ("  mem marker      : {0}" -f $memLine)
Write-Host ("  init done       : {0}" -f $initLine)
Write-Host ("  heartbeats      : {0}" -f $heartbeatCount)
Write-Host ("  wad mirror      : {0} bytes (expected disk WAD: {1})" -f $mirrorSize, $wadSizeBytes)
Write-Host ("  OOM lines       : {0}{1}" -f $oomCount, $(if ($lowMem) { " (recorded, not a failure in 64MiB mode)" } else { "" }))

Report "A1) desktop advertises DOOM on demand (Ctrl+Alt+D)" ($null -ne $onDemandLine) $onDemandLine
Report "A2) Ctrl+Alt+D spawned the DOOM window ([wm] spawn DOOM)" `
    ((Get-PatternCount -Text $finalSerialText -Pattern '\[wm\] spawn DOOM') -ge 1 -or
     (Get-PatternCount -Text $finalSerialText -Pattern '\[doom\] client start') -ge 1) `
    ("[wm] spawn DOOM count={0}; [doom] client start={1}" -f (Get-PatternCount -Text $finalSerialText -Pattern '\[wm\] spawn DOOM'), (Get-PatternCount -Text $finalSerialText -Pattern '\[doom\] client start'))

if ($null -ne $compactLine) {
    Report "B1) DOOM runs in compact mode ([doom] compact=1)" $true $compactLine
} else {
    Warn "B1) [doom] compact=1 marker missing" "DOOM_TINY is not enabled in the running build (WARN per task; other DOOM checks still apply)"
}
Report "B2) DOOM reported its zone tier ([doom] zone_mb=)" `
    (($null -ne $zoneLine) -and ($zoneValue -ge 2) -and ($zoneValue -le 64)) `
    ("{0} (parsed={1}; expected tier from -m {2}M)" -f $zoneLine, $zoneValue, $MemMB)
Report "B3) DOOM init completed ([doom] init done)" ($null -ne $initLine) $initLine
Report "B4) at least 2 DOOM frame heartbeats ([doom] f=)" ($heartbeatCount -ge 2) `
    ("heartbeat count={0}" -f $heartbeatCount)
if ($mirrorSize -ge 0 -and $wadSizeBytes -gt 0) {
    Report "B5) DOOM mirrored the WAD from disk (mirror bytes == disk WAD size)" `
        ($mirrorSize -eq $wadSizeBytes) `
        ("mirror={0} disk_wad={1}" -f $mirrorSize, $wadSizeBytes)
} else {
    Warn "B5) WAD mirror-size cross-check skipped" ("mirror={0} disk_wad={1}" -f $mirrorSize, $wadSizeBytes)
}
Report "B6) no kernel panic / STALL WATCHDOG / invalid free" ($null -eq $crashLine) `
    ($(if ($crashLine) { $crashLine.Value.Trim() } else { "none" }))
$shotDoom = $shots['doom']
Report "B7) DOOM screendump captured (doom_compact.ppm)" `
    ($null -ne $shotDoom -and $shotDoom.Ok) `
    ($(if ($shotDoom) { $shotDoom.Path } else { "no screenshot" }))
Info ("OOM lines: {0} (informational only{1})" -f $oomCount, $(if ($lowMem) { "; 64MiB target" } else { "" }))

# ----------------------------------------------------------------------------
# Verdict
# ----------------------------------------------------------------------------
Write-Host "---- evidence paths ----"
Write-Host ("  serial log : {0}" -f $serialPath)
foreach ($key in @('base', 'doom')) {
    if ($shots.ContainsKey($key) -and $shots[$key]) { Write-Host ("  {0,-6}: {1}" -f $key, $shots[$key].Path) }
}
Write-Host "---- key serial markers ----"
foreach ($line in (Get-LastLines $finalSerialText 500)) {
    if ($line -match '\[doom\]|\[desktop\] DOOM|\[desktop\] start: fb=') { Write-Host ("  " + $line) }
}

if ($script:FailCount -gt 0) {
    Write-Host "---- serial log tail (last 40 lines) ----"
    foreach ($line in (Get-LastLines $finalSerialText 40)) { Write-Host ("  " + $line) }
    Write-Host ("DOOM COMPACT SMOKE: FAIL ({0} assertion(s) failed, {1} warning(s))" -f $script:FailCount, $script:WarnCount)
    exit 1
}
Write-Host ("DOOM COMPACT SMOKE: PASS ({0} warning(s))" -f $script:WarnCount)
exit 0
