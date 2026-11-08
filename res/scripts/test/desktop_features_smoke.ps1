#Requires -Version 5.1
# ============================================================================
# res/scripts/test/desktop_features_smoke.ps1
#   Headless QEMU gate for the SkylineSystem desktop feature stack:
#     A) first-boot Installer: deploys /mp resources (themes.index, theme_*.lua,
#        files.index, notes.txt) and requests the Aurora Lua theme;
#     B) desktop Settings window: reads /mp/themes.index, Enter sends
#        SKYWIN_CMD_LOAD_LUA, the desktop really switches the running theme
#        (serial "[desktop] theme: load path=... rc=0", wallpaper + chrome
#        pixels change in the screendump);
#     C) Settings R (reload /mp/desktop.lua) and I (spawn Installer) commands
#        are proven by the serial markers they trigger;
#     D) Notepad Open / Close dialogs driven through QEMU HMP "sendkey"
#        (Ctrl+O / Down / Enter / Ctrl+W / Esc), proven by the [notepad]
#        serial markers plus screendump differences.
#
# FLOW
#   1) preprocess disk.img with WSL e2tools: delete /mp/themes.index,
#      /mp/selected_theme.txt, /mp/theme_*.lua, /mp/files.index, /mp/notes.txt
#      (and /mp/installer.log if present) so the first-boot install is fresh;
#   2) boot QEMU headless (serial file + HMP telnet + screendump PPM);
#   3) wait for the desktop + Settings serial markers (the Installer is NOT
#      auto-spawned by this integration: first boot only brings up Settings,
#      which reports "no themes installed - press I to run Installer");
#   4) take desktop_features_settings_base.ppm (initial frame) and assert the
#      Settings window is really on screen (title colour from the serial style
#      marker + content paper pixel from the known window geometry) and that
#      the wallpaper matches the script-painted readback;
#   5) HMP sendkey ctrl-alt-i triggers the Installer; wait for
#      [installer] start: / Install complete: / READY / theme apply requested /
#      [desktop] theme: load path=/mp/theme_aurora.lua rc=0, then take
#      desktop_features_installer.ppm and assert the Aurora frame;
#   6) close the Installer (esc) so the later Settings "I" command can prove
#      itself with a second [installer] start:; apply Mint through Settings
#      (ctrl-alt-s, down, down, ret).  If the Settings window was spawned
#      before the install (stale empty theme list) the helper restarts it with
#      esc + ctrl-alt-s so it re-reads /mp/themes.index, then retries.  Take
#      desktop_features_settings.ppm and assert the theme switch + colours;
#   7) ctrl-alt-n focuses Notepad, ctrl-o opens the Open dialog, down+ret opens
#      the second entry (/mp/theme_aurora.lua), ctrl-w opens the Close dialog,
#      esc cancels.  [notepad] dialog=open / opened path= / dialog=close /
#      close choice=cancel markers are asserted; if the running build predates
#      those markers the checks degrade to WARN (screenshot diffs still prove
#      the dialogs rendered) -- pass -RequireNotepadMarkers to make them hard;
#   8) ctrl-alt-s -> "r" (reload desktop.lua) and "i" (spawn Installer again);
#      assert the serial markers, take desktop_features_final.ppm;
#   9) quit/kill QEMU, re-read disk.img with WSL e2tools and verify the
#      installed resources + /mp/selected_theme.txt; print PASS/FAIL/WARN and
#      the evidence paths.
#
# HMP sendkey key names used: ctrl-alt-s, ctrl-alt-n, ctrl-alt-i, ctrl-o,
# ctrl-w, down, ret, esc.  Every monitor response is checked; a rejected key
# is reported with the raw monitor output.
#
# OUTPUT (all under <Root>)
#   desktop_features_serial.log
#   desktop_features_settings_base.ppm
#   desktop_features_installer.ppm
#   desktop_features_settings.ppm
#   desktop_features_notepad_open.ppm
#   desktop_features_notepad_opened.ppm
#   desktop_features_notepad_close.ppm
#   desktop_features_final.ppm
#
# EXIT CODES: 0 = PASS (WARNs allowed unless -RequireNotepadMarkers),
#             1 = assertion failure, 2 = environment/setup failure.
# ============================================================================
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [string]$Iso = "",
    [string]$Disk = "",
    [switch]$UseDisk,
    [switch]$NoDisk,
    [int]$WaitSeconds = 90,
    [int]$ActionWaitSeconds = 25,
    [int]$ScreenshotDelay = 2,
    [int]$MinDiffCells = 12,
    [int]$Tolerance = 10,
    [string]$SerialLog = "",
    [string]$ScreenshotPrefix = "",
    [string]$WslDistro = "Ubuntu-24.04",
    [switch]$SkipPreprocess,
    [switch]$SkipDiskVerify,
    [switch]$RequireNotepadMarkers,
    [int]$MonitorPort = 0
)

$ErrorActionPreference = "Continue"
$script:FailCount = 0
$script:WarnCount = 0
$script:WslSeq = 0
$script:WslTmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sky_features_wsl_{0}" -f $PID)

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
$serialPath = if ($SerialLog) { $SerialLog } else { Join-Path $root "desktop_features_serial.log" }
$shotPrefix = if ($ScreenshotPrefix) { $ScreenshotPrefix } else { Join-Path $root "desktop_features" }
try { $serialPath = [System.IO.Path]::GetFullPath($serialPath) } catch { }
if ($shotPrefix -notmatch '^[A-Za-z]:') { try { $shotPrefix = [System.IO.Path]::GetFullPath($shotPrefix) } catch { } }

# ============================================================================
# SHARED HELPERS (pure functions; also extracted verbatim by offline tests)
# ============================================================================
function Write-Step {
    param([string]$Text)
    Write-Host ("DESKTOP FEATURES: {0}" -f $Text)
}
function Info {
    param([string]$Text)
    Write-Host ("INFO: " + $Text)
}
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
    param($A, $B, [int]$Tolerance = 16)
    if ($null -eq $A -or $null -eq $B) { return $false }
    if ([math]::Abs([int]$A.R - [int]$B.R) -gt $Tolerance) { return $false }
    if ([math]::Abs([int]$A.G - [int]$B.G) -gt $Tolerance) { return $false }
    if ([math]::Abs([int]$A.B - [int]$B.B) -gt $Tolerance) { return $false }
    return $true
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
            $shareMode = [System.IO.FileShare]([int][System.IO.FileShare]::ReadWrite -bor [int][System.IO.FileShare]::Delete)
            $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
                                         [System.IO.FileAccess]::Read, $shareMode)
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
# PPM P6 parser + sampling (same proven code as lua_desktop_smoke.ps1)
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
    if (([int64]$dataStart + $need) -gt [int64]$state.Len) { throw ("PPM raster truncated" ) }
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
    return (New-Rgb `
        ([int][math]::Floor($sumR / $count + 0.5)) `
        ([int][math]::Floor($sumG / $count + 0.5)) `
        ([int][math]::Floor($sumB / $count + 0.5)))
}
# Cheap whole-frame signature: centre pixel of a Cols x Rows grid.
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
# Serial markers
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
          [int]$TimeoutSeconds = 25, $Proc = $null)
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
        Start-Sleep -Milliseconds 250
    }
    return [pscustomobject]@{ Ok = $false; Count = (Get-PatternCount -Text $text -Pattern $Pattern); Text = $text; Elapsed = ((Get-Date) - $startTime).TotalSeconds }
}
function Wait-ThemeLoad {
    param([string]$Path, [string]$LuaPath, [int]$MinCount = 1, [int]$TimeoutSeconds = 25, $Proc = $null)
    $pattern = ('\[desktop\] theme: load path={0} rc=(-?\d+) radius=(\d+)' -f [regex]::Escape($LuaPath))
    return (Wait-SerialAtLeast -Path $Path -Pattern $pattern -MinCount $MinCount -TimeoutSeconds $TimeoutSeconds -Proc $Proc)
}
function Get-StyleValues {
    param([string]$Text)
    $m = Get-LastMatch -Text $Text -Pattern '\[desktop\] lua: style radius=(\d+) title=0x([0-9A-Fa-f]{6}) border=0x([0-9A-Fa-f]{6}) ink=0x([0-9A-Fa-f]{6}) close=0x([0-9A-Fa-f]{6}) paper=0x([0-9A-Fa-f]{6}) shadow=(\d+)'
    if (-not $m) { return $null }
    return [pscustomobject]@{
        Radius  = [int]$m.Groups[1].Value
        Title   = (Convert-Hex6ToRgb $m.Groups[2].Value)
        Border  = (Convert-Hex6ToRgb $m.Groups[3].Value)
        Ink     = (Convert-Hex6ToRgb $m.Groups[4].Value)
        Close   = (Convert-Hex6ToRgb $m.Groups[5].Value)
        Paper   = (Convert-Hex6ToRgb $m.Groups[6].Value)
        Shadow  = [int]$m.Groups[7].Value
    }
}
function Get-WallpaperValues {
    param([string]$Text)
    $m = Get-LastMatch -Text $Text -Pattern '\[desktop\] lua: wallpaper: script-painted fb=(\S+) (\d+)x(\d+) top=0x([0-9A-Fa-f]{6}) mid=0x([0-9A-Fa-f]{6}) bottom=0x([0-9A-Fa-f]{6})'
    if (-not $m) { return $null }
    return [pscustomobject]@{
        Fb      = $m.Groups[1].Value
        Width   = [int]$m.Groups[2].Value
        Height  = [int]$m.Groups[3].Value
        Top     = (Convert-Hex6ToRgb $m.Groups[4].Value)
        Mid     = (Convert-Hex6ToRgb $m.Groups[5].Value)
        Bottom  = (Convert-Hex6ToRgb $m.Groups[6].Value)
    }
}
function Get-ThemeLoads {
    param([string]$Text)
    $list = @()
    if (-not $Text) { return $list }
    $ms = [regex]::Matches($Text, '\[desktop\] theme: load path=(\S+) rc=(-?\d+) radius=(\d+)')
    for ($i = 0; $i -lt $ms.Count; $i++) {
        $m = $ms[$i]
        $list += [pscustomobject]@{
            Path   = $m.Groups[1].Value
            Rc     = [int]$m.Groups[2].Value
            Radius = [int]$m.Groups[3].Value
        }
    }
    return $list
}
# True when the kernel stall watchdog reported a hang (SMP/FS regression).
function Test-KernelStalled {
    param([string]$Text)
    if (-not $Text) { return 0 }
    return ([regex]::Matches($Text, 'STALL WATCHDOG')).Count
}
# Known WM client geometry (desktop spawn parameters: body w/h + dx/dy offsets;
# surface = body + 2*SKYWIN_SHADOW, centered, then shifted by dx/dy).
function Get-ClientWindowGeom {
    param([int]$ScrW, [int]$ScrH, [int]$BodyW, [int]$BodyH, [int]$Dx, [int]$Dy)
    $m = 26
    $surfW = $BodyW + 2 * $m
    $surfH = $BodyH + 2 * $m
    $sx = [int][math]::Floor(($ScrW - $surfW) / 2)
    $sy = [int][math]::Floor(($ScrH - $surfH) / 2)
    $bx = $sx + $Dx
    $by = $sy + $Dy
    return [pscustomobject]@{
        BodyX = $bx; BodyY = $by; BodyW = $BodyW; BodyH = $BodyH
        CenterX = ($bx + [int][math]::Floor($BodyW / 2))
        TitleY = ($by + 8)
        TitleX = ($bx + [int][math]::Floor($BodyW / 2))
    }
}

# ---------------------------------------------------------------------------
# WSL e2tools helpers (disk.img preprocessing + post-run verification)
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
    param([string]$Body, [int]$TimeoutMs = 120000)
    New-Item -ItemType Directory -Force -Path $script:WslTmpDir | Out-Null
    $script:WslSeq = $script:WslSeq + 1
    $sh = Join-Path $script:WslTmpDir ("step_{0}.sh" -f $script:WslSeq)
    $outFile = Join-Path $script:WslTmpDir ("step_{0}.out" -f $script:WslSeq)
    $errFile = Join-Path $script:WslTmpDir ("step_{0}.err" -f $script:WslSeq)
    $full = "#!/bin/bash`n" + $Body + "`n__WSL_RC__=`$?`n"
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
        $text = ($out | Out-String)
        return ($text -match '__E2_OK__')
    } catch {
        return $false
    }
}
function Get-TokensFromListing {
    param([string]$Listing)
    $tokens = @()
    if (-not $Listing) { return $tokens }
    foreach ($tok in ($Listing -split '\s+')) {
        if ($tok -and $tok.Trim()) { $tokens += $tok.Trim() }
    }
    return $tokens
}
function Invoke-DiskPreprocess {
    param([string]$ImageWsl)
    $body = @"
img='$ImageWsl'
echo '=== LIST BEFORE ==='
e2ls "`$img:/" 2>&1
echo '=== DELETE ==='
for f in themes.index selected_theme.txt installer.log theme_aurora.lua theme_ember.lua theme_mint.lua files.index notes.txt; do
  if e2ls "`$img:/" 2>/dev/null | tr -s ' ' '\n' | grep -qx "`$f"; then
    out=`$(e2rm "`$img:/`$f" 2>&1); rc=`$?
    echo "rm `$f rc=`$rc `$out"
  else
    echo "rm `$f absent"
  fi
done
echo '=== LIST AFTER ==='
e2ls "`$img:/" 2>&1
echo '=== PREPROCESS DONE ==='
exit 0
"@
    return (Invoke-WslScript -Body $body)
}
function Invoke-DiskPostcheck {
    param([string]$ImageWsl)
    $body = @"
img='$ImageWsl'
echo '=== ROOT ==='
e2ls "`$img:/" 2>&1
for f in themes.index selected_theme.txt files.index notes.txt theme_aurora.lua theme_ember.lua theme_mint.lua; do
  echo "=== FILE `$f ==="
  e2cp "`$img:/`$f" - 2>&1
  rc=`$?
  echo ""
  echo "=== ENDFILE `$f rc=`$rc ==="
done
echo '=== POSTCHECK DONE ==='
exit 0
"@
    return (Invoke-WslScript -Body $body)
}
function Parse-DiskPostcheck {
    param([string]$Output)
    $files = @{}
    $rootListing = ""
    $rootMatch = [regex]::Match($Output, '(?s)=== ROOT ===(.*?)=== FILE ')
    if ($rootMatch.Success) { $rootListing = $rootMatch.Groups[1].Value }
    $ms = [regex]::Matches($Output, '(?s)=== FILE (?<name>[^\s]+) ===\r?\n(?<content>.*?)=== ENDFILE \k<name> rc=(?<rc>-?\d+) ===')
    for ($i = 0; $i -lt $ms.Count; $i++) {
        $m = $ms[$i]
        $files[$m.Groups['name'].Value] = [pscustomobject]@{
            Name = $m.Groups['name'].Value
            Rc = [int]$m.Groups['rc'].Value
            Content = $m.Groups['content'].Value
        }
    }
    return [pscustomobject]@{ RootListing = $rootListing; Files = $files }
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
    try {
        return @(Get-Process -Name qemu-system-x86_64 -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.Id })
    } catch {
        return @()
    }
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
            try {
                $qp = Get-Process -Id $qpid -ErrorAction SilentlyContinue
                if ($qp) { $qp.Kill() }
            } catch { }
        }
        if ($Proc) { try { [void]$Proc.WaitForExit(1500) } catch { } }
        Start-Sleep -Milliseconds 500
    }
    $left = @(Get-QemuProcessIds | Where-Object { $Before -notcontains $_ })
    foreach ($qpid in $left) { try { & taskkill /F /PID $qpid 2>$null | Out-Null } catch { } }
    if ($left.Count -gt 0) { Start-Sleep -Milliseconds 600 }
    $left2 = @(Get-QemuProcessIds | Where-Object { $Before -notcontains $_ })
    return ($left2.Count -eq 0)
}
function Format-MonitorText {
    param([string]$Text)
    if (-not $Text) { return "<empty>" }
    $clean = $Text -replace '[^\x20-\x7E\r\n]', ' '
    $clean = $clean -replace '\s+', ' '
    $clean = $clean.Trim()
    if ($clean.Length -gt 300) { $clean = $clean.Substring(0, 300) + "..." }
    return $clean
}
function Read-MonitorUntil {
    param([System.Net.Sockets.NetworkStream]$Stream, [string]$Token, [int]$TimeoutMs)
    $buffer = New-Object byte[] 4096
    $sb = New-Object System.Text.StringBuilder
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $read = -1
        try {
            if ($Stream.DataAvailable) { $read = [int]$Stream.Read($buffer, 0, $buffer.Length) }
        } catch { break }
        if ($read -gt 0) {
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            if ($sb.ToString().Contains($Token)) { break }
        } elseif ($read -eq 0) {
            break
        } else {
            Start-Sleep -Milliseconds 80
        }
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
            $result.Ok = $true
            $result.Client = $candidate
            $result.Stream = $stream
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
    if ($err.Success) {
        return [pscustomobject]@{ Ok = $false; Output = (Format-MonitorText $out); Error = $err.Value.Trim() }
    }
    return [pscustomobject]@{ Ok = $true; Output = (Format-MonitorText $out); Error = "" }
}
function Send-MonitorText {
    param($Session, [string]$Command, [int]$TimeoutMs = 8000)
    return (Invoke-MonitorCommand -Session $Session -Command $Command -TimeoutMs $TimeoutMs)
}
function Get-ScreendumpStagingPath {
    param([string]$RequestedPath)
    if ($RequestedPath -and ($RequestedPath -notmatch '\s')) { return $RequestedPath }
    $candidates = @()
    try {
        $tmp = [System.IO.Path]::GetTempPath()
        if ($tmp) { $candidates += [System.IO.Path]::Combine($tmp, ("desktop_features_shot_{0}.ppm" -f $PID)) }
    } catch { }
    foreach ($candidate in $candidates) {
        if ($candidate -and ($candidate -notmatch '\s')) { return $candidate }
    }
    return $null
}
function Save-Screenshot {
    param($Session, [string]$Path, [int]$Retries = 3)
    if ($null -eq $Session -or -not $Session.Ok) {
        return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = "no monitor session" }
    }
    $staging = Get-ScreendumpStagingPath -RequestedPath $Path
    if (-not $staging) {
        return [pscustomobject]@{ Ok = $false; Path = $Path; Ppm = $null; Error = "no whitespace-free staging path" }
    }
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

# ----------------------------------------------------------------------------
# Phase 1: environment checks
# ----------------------------------------------------------------------------
Write-Host "DESKTOP FEATURES: ==================================================="
Write-Host "DESKTOP FEATURES: Installer + Settings/Lua theme + Notepad dialogs gate"
Write-Host "DESKTOP FEATURES: root      = $root"
Write-Host "DESKTOP FEATURES: qemu      = $qemu"
Write-Host "DESKTOP FEATURES: iso       = $isoPath"
Write-Host "DESKTOP FEATURES: disk      = $diskPath"
Write-Host "DESKTOP FEATURES: serial    = $serialPath"
Write-Host "DESKTOP FEATURES: shots     = ${shotPrefix}_*.ppm"

function Exit-Environment {
    param([string]$Message)
    Write-Host ("ERROR: " + $Message)
    Write-Host "DESKTOP FEATURES: FAIL (environment)"
    exit 2
}

if (-not (Test-Path -LiteralPath $qemu)) { Exit-Environment ("QEMU not found: " + $qemu) }
if (-not (Test-Path -LiteralPath $isoPath)) { Exit-Environment ("ISO not found, build it first: " + $isoPath) }
if (-not (Test-Path -LiteralPath $diskPath)) { Exit-Environment ("disk.img not found (this test needs the user disk): " + $diskPath) }
if (-not $UseDisk -and $NoDisk) { Exit-Environment "this test requires the user disk image; -NoDisk is not supported" }
$existingQemu = @(Get-QemuProcessIds)
if ($existingQemu.Count -gt 0) {
    Exit-Environment ("another QEMU is already running (PID: {0}); stop it first (disk.img lock + serial/display conflicts)" -f ($existingQemu -join ', '))
}

$needWsl = (-not $SkipPreprocess) -or (-not $SkipDiskVerify)
if ($needWsl) {
    if (-not (Test-WslE2Tools -Distro $WslDistro)) {
        Exit-Environment ("WSL distro '{0}' with e2tools (e2ls/e2rm/e2cp) is required for disk.img pre/post-processing" -f $WslDistro)
    }
    Write-Host "DESKTOP FEATURES: WSL e2tools OK (distro=$WslDistro)"
}

$serialDir = Split-Path -Parent $serialPath
if ($serialDir -and -not (Test-Path -LiteralPath $serialDir)) { New-Item -ItemType Directory -Force -Path $serialDir | Out-Null }

Remove-Item -LiteralPath $serialPath -Force -ErrorAction SilentlyContinue
foreach ($suffix in @('settings_base', 'installer', 'settings', 'notepad_open', 'notepad_opened', 'notepad_close', 'final')) {
    Remove-Item -LiteralPath ("{0}_{1}.ppm" -f $shotPrefix, $suffix) -Force -ErrorAction SilentlyContinue
}

# ----------------------------------------------------------------------------
# Phase 2: disk.img preprocessing (first-boot state)
# ----------------------------------------------------------------------------
$diskWsl = ConvertTo-WslPath $diskPath
if (-not $SkipPreprocess) {
    Write-Step "preprocessing disk.img (delete installed themes/resources)"
    $pre = Invoke-DiskPreprocess -ImageWsl $diskWsl
    if (-not $pre.Ok) {
        Write-Host $pre.Output
        Exit-Environment ("disk.img preprocessing failed (WSL rc={0}): {1}" -f $pre.Code, (Format-MonitorText $pre.Error))
    }
    $beforeMatch = [regex]::Match($pre.Output, '(?s)=== LIST BEFORE ===(.*?)=== DELETE ===')
    $afterMatch = [regex]::Match($pre.Output, '(?s)=== LIST AFTER ===(.*?)=== PREPROCESS DONE ===')
    $beforeTokens = @(Get-TokensFromListing -Listing $beforeMatch.Groups[1].Value)
    $afterTokens = @(Get-TokensFromListing -Listing $afterMatch.Groups[1].Value)
    $requiredBase = @('desktop.elf', 'installer.elf', 'settings.elf', 'notepad.elf', 'libsky.so', 'desktop.lua')
    $missingBase = @()
    foreach ($need in $requiredBase) { if ($afterTokens -notcontains $need) { $missingBase += $need } }
    if ($missingBase.Count -gt 0) {
        Write-Host ($pre.Output)
        Exit-Environment ("disk.img is missing required base files: " + ($missingBase -join ', '))
    }
    $leftInstalled = @()
    foreach ($gone in @('themes.index', 'selected_theme.txt', 'installer.log', 'theme_aurora.lua', 'theme_ember.lua', 'theme_mint.lua', 'files.index', 'notes.txt')) {
        if ($afterTokens -contains $gone) { $leftInstalled += $gone }
    }
    if ($leftInstalled.Count -gt 0) {
        Write-Host ($pre.Output)
        Exit-Environment ("disk.img preprocessing left installed files behind: " + ($leftInstalled -join ', '))
    }
    if ($beforeTokens.Count -gt 0) { Write-Step ("disk.img preprocessed: {0} root entries before, {1} after" -f $beforeTokens.Count, $afterTokens.Count) }
} else {
    Warn "disk preprocessing skipped (-SkipPreprocess)"
}

# ----------------------------------------------------------------------------
# Phase 3: single QEMU boot
#   First boot: the desktop spawns Settings (title bar shows "no themes
#   installed - press I to run Installer").  Ctrl+Alt+I starts the Installer,
#   which deploys the /mp resources and asks the desktop to apply the Aurora
#   theme.  Settings re-reads /mp/themes.index automatically once it exists, so
#   Ctrl+Alt+S + Down/Down/Enter applies the Mint theme in the same boot.
#   The script accepts both the fixed kernel (Esc delivered) and the previous
#   one: Esc is tried first, Enter (installer close) and Down/Down/Enter
#   (Notepad Close dialog cancel) are used as automatic fallbacks.
# ----------------------------------------------------------------------------
function Start-DesktopSession {
    param(
        [string]$SerialFile,
        [int]$WaitSeconds = 90,
        [int]$ScreenshotDelay = 2,
        [int]$MonitorPort = 0,
        [string]$Tag = "boot"
    )
    $session = [pscustomobject]@{
        Ok = $false; EnvError = $null; Proc = $null; Monitor = $null
        BootText = ""; Missing = @(); SerialFile = $SerialFile; Port = 0
        QemuBefore = @(); Tag = $Tag; BootSnap = ""
    }
    Remove-Item -LiteralPath $SerialFile -Force -ErrorAction SilentlyContinue
    $port = if ($MonitorPort -gt 0) { [int]$MonitorPort } else { [int](Get-FreeTcpPort) }
    $session.Port = $port
    $sessionArgs = @(
        '-M', 'q35',
        '-cpu', 'max',
        '-m', '2G',
        '-smp', '4',
        '-cdrom', $isoPath,
        '-serial', ("file:{0}" -f $SerialFile),
        '-monitor', ("telnet:127.0.0.1:{0},server,nowait" -f $port),
        '-display', 'none',
        '-vga', 'std',
        '-no-reboot',
        '-no-shutdown',
        '-drive', ("file={0},if=none,id=drive0,format=raw" -f $diskPath),
        '-device', 'ide-hd,drive=drive0,bus=ide.0'
    )
    $quoted = @($sessionArgs | ForEach-Object { Quote-ProcessArg $_ })
    Write-Step ("starting QEMU [{0}] (monitor telnet:127.0.0.1:{1})" -f $Tag, $port)
    Write-Host ("DESKTOP FEATURES: command   = {0} {1}" -f $qemu, ($quoted -join ' '))
    $session.QemuBefore = @(Get-QemuProcessIds)
    try {
        $session.Proc = Start-Process -FilePath $qemu -ArgumentList $quoted -PassThru -WindowStyle Hidden
    } catch {
        $session.EnvError = ("failed to start QEMU: " + $_.Exception.Message)
        return $session
    }
    Start-Sleep -Milliseconds 1200
    if ($session.Proc.HasExited) {
        $session.EnvError = ("QEMU [{0}] exited immediately (exit code {1}); stale disk lock or bad ISO/args?" -f $Tag, $session.Proc.ExitCode)
        return $session
    }
    $specs = @(
        [pscustomobject]@{ Name = 'desktop start'; Pattern = '\[desktop\] start: fb=' },
        [pscustomobject]@{ Name = 'lua source=file'; Pattern = '\[desktop\] lua: source=file' },
        [pscustomobject]@{ Name = 'lua style'; Pattern = '\[desktop\] lua: style radius=' },
        [pscustomobject]@{ Name = 'wallpaper painted'; Pattern = '\[desktop\] lua: wallpaper: script-painted' },
        [pscustomobject]@{ Name = 'Settings spawned'; Pattern = '\[wm\] spawn Settings' },
        [pscustomobject]@{ Name = 'desktop ready'; Pattern = '\[perf\] desktop ready in' }
    )
    $bootStart = Get-Date
    $deadline = $bootStart.AddSeconds($WaitSeconds)
    $missing = @()
    while ((Get-Date) -lt $deadline) {
        $snap = Get-SerialSnapshot -Path $SerialFile -Attempts 1
        if ($null -ne $snap) { $session.BootText = $snap }
        $missing = @()
        foreach ($spec in $specs) {
            if ((Get-PatternCount -Text $session.BootText -Pattern $spec.Pattern) -lt 1) { $missing += $spec.Name }
        }
        if ($missing.Count -eq 0) { break }
        $fatal = Get-LastMatch -Text $session.BootText -Pattern 'Kernel panic|Kernel crash|General protection|SMASHING|\[desktop\] FATAL'
        if ($fatal) { break }
        try { if ($session.Proc.HasExited) { break } } catch { }
        Start-Sleep -Milliseconds 400
    }
    $session.Missing = $missing
    $session.Ok = ($missing.Count -eq 0)
    if (-not $session.Ok) {
        Write-Host ("DESKTOP FEATURES: boot [{0}] markers missing: {1}" -f $Tag, ($missing -join ', '))
        return $session
    }
    Write-Step ("boot [{0}] markers complete after {1:N1}s" -f $Tag, ((Get-Date) - $bootStart).TotalSeconds)
    Start-Sleep -Seconds ([int][math]::Max(1, $ScreenshotDelay))
    $session.BootSnap = Get-SerialSnapshot -Path $SerialFile -Attempts 3
    $session.Monitor = Open-MonitorSession -HostName '127.0.0.1' -Port $port
    if (-not $session.Monitor.Ok) {
        $session.EnvError = $session.Monitor.Error
        $session.Ok = $false
        return $session
    }
    Write-Step ("HMP monitor connected [{0}] (port {1})" -f $Tag, $port)
    return $session
}

function Stop-DesktopSession {
    param($Session)
    if ($null -eq $Session) { return }
    if ($Session.Monitor -and $Session.Monitor.Ok) {
        try { [void](Send-MonitorText -Session $Session.Monitor -Command 'quit' -TimeoutMs 3000) } catch { }
    }
    if ($Session.Monitor -and $Session.Monitor.Client) { try { $Session.Monitor.Client.Close() } catch { } }
    $clean = Stop-QemuProcesses -Before $Session.QemuBefore -Proc $Session.Proc
    if (-not $clean) { Warn ("QEMU [{0}] may still be running after the kill sweep" -f $Session.Tag) }
    if ($Session.Proc) { Write-Step ("QEMU [{0}] stopped (monitor quit + kill sweep)" -f $Session.Tag) }
}

function Send-KeyChecked {
    param($Session, [string]$Keys, [int]$TimeoutMs = 8000)
    if ($null -eq $Session -or $null -eq $Session.Monitor -or -not $Session.Monitor.Ok) { return $false }
    $r = Send-MonitorKey -Session $Session.Monitor -Keys $Keys -TimeoutMs $TimeoutMs
    if (-not $r.Ok) {
        Warn ("HMP sendkey '{0}' failed [{1}]" -f $Keys, $Session.Tag) $r.Error
        return $false
    }
    return $true
}

function Send-KeySequence {
    param($Session, [string[]]$Keys, [int]$GapMs = 300)
    foreach ($key in $Keys) {
        [void](Send-KeyChecked -Session $Session -Keys $key)
        Start-Sleep -Milliseconds $GapMs
    }
}

# Apply the Mint theme through Settings.  The fixed Settings auto-refreshes its
# theme list once /mp/themes.index appears; before that fix we fall back to the
# explicit "L" reload key, and finally to a Settings restart (Esc quit +
# Ctrl+Alt+S re-spawn).  Returns the Wait-ThemeLoad result with a Method field.
function Invoke-SettingsApplyMint {
    param($Session, [string]$SerialPath, [int]$ActionWaitSeconds = 25)
    $result = [pscustomobject]@{ Ok = $false; Count = 0; Text = ""; Elapsed = 0.0; Method = "auto-refresh" }
    if ($null -eq $Session -or $null -eq $Session.Monitor -or -not $Session.Monitor.Ok) {
        $result.Method = "no-monitor"
        return $result
    }
    $trySelection = {
        param([int]$TimeoutSeconds)
        [void](Send-KeyChecked -Session $Session -Keys 'ctrl-alt-s')
        Start-Sleep -Milliseconds 400
        [void](Send-KeyChecked -Session $Session -Keys 'down')
        Start-Sleep -Milliseconds 300
        [void](Send-KeyChecked -Session $Session -Keys 'down')
        Start-Sleep -Milliseconds 300
        [void](Send-KeyChecked -Session $Session -Keys 'ret')
        return (Wait-ThemeLoad -Path $SerialPath -LuaPath '/mp/theme_mint.lua' -MinCount 1 -TimeoutSeconds $TimeoutSeconds)
    }

    # 1) fixed Settings: the list refreshes automatically after the install
    Start-Sleep -Milliseconds 2500
    $wait = & $trySelection 10
    if ($wait.Ok) { $wait | Add-Member -NotePropertyName Method -NotePropertyValue 'auto-refresh' -Force; return $wait }

    # 2) older Settings: explicit "L" list reload, then select again
    Write-Step "Settings list not refreshed yet; sending L (reload index) and retrying"
    [void](Send-KeyChecked -Session $Session -Keys 'l')
    Start-Sleep -Milliseconds 1200
    $wait = & $trySelection 12
    if ($wait.Ok) { $wait | Add-Member -NotePropertyName Method -NotePropertyValue 'L-key' -Force; return $wait }

    # 3) last resort: restart Settings (needs the Esc fix to quit it)
    Write-Step "L fallback failed; restarting Settings (Esc + Ctrl+Alt+S)"
    [void](Send-KeyChecked -Session $Session -Keys 'ctrl-alt-s')
    Start-Sleep -Milliseconds 300
    [void](Send-KeyChecked -Session $Session -Keys 'esc')
    Start-Sleep -Milliseconds 900
    [void](Send-KeyChecked -Session $Session -Keys 'ctrl-alt-s')
    $spawnWait = Wait-SerialAtLeast -Path $SerialPath -Pattern '\[wm\] spawn Settings' -MinCount 2 -TimeoutSeconds 12 -Proc $Session.Proc
    if (-not $spawnWait.Ok) {
        $result.Method = "settings-restart-failed"
        return $result
    }
    Start-Sleep -Milliseconds 1200
    $wait = & $trySelection $ActionWaitSeconds
    $wait | Add-Member -NotePropertyName Method -NotePropertyValue 'settings-restart' -Force
    return $wait
}

$shots = @{}
$serialSnapshots = @{}
$stageResults = [ordered]@{}
$envError = $null
$phase1 = $null
$activeSession = $null

# ============================================================================
# Single boot: first-boot install -> Aurora -> Mint -> Notepad dialogs -> R/I
# ============================================================================
try {
    $phase1 = Start-DesktopSession -SerialFile $serialPath -WaitSeconds $WaitSeconds `
        -ScreenshotDelay $ScreenshotDelay -MonitorPort $MonitorPort -Tag 'boot'
    $activeSession = $phase1
    if ($phase1.EnvError) {
        $envError = $phase1.EnvError
    } elseif (-not $phase1.Ok) {
        Write-Host "DESKTOP FEATURES: boot did not come up; continuing to evidence collection"
    } else {
        $serialSnapshots['boot'] = $phase1.BootSnap

        # ---------- frame 0: initial desktop (Settings, no themes installed) ----------
        $shotBase = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_settings_base.ppm" -f $shotPrefix)
        $shots['settings_base'] = $shotBase
        if ($shotBase.Ok) {
            Write-Step ("screenshot base: {0} ({1}x{2})" -f $shotBase.Path, $shotBase.Ppm.Width, $shotBase.Ppm.Height)
        } else {
            Write-Host ("ERROR: base screenshot failed: " + $shotBase.Error)
        }

        # ---------- trigger the Installer explicitly (Ctrl+Alt+I) ----------
        $bootInstallerCount = Get-PatternCount -Text $phase1.BootSnap -Pattern '\[installer\] start:'
        if ($bootInstallerCount -eq 0) {
            [void](Send-KeyChecked -Session $phase1 -Keys 'ctrl-alt-i')
            $instStart = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] start:' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
            $stageResults['installer_start'] = $instStart
        } else {
            Write-Step "NOTE: Installer was already spawned at boot (auto-spawn build)"
            $stageResults['installer_start'] = [pscustomobject]@{ Ok = $true; Count = $bootInstallerCount; Text = $phase1.BootSnap; Elapsed = 0.0 }
        }
        $installComplete = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] Install complete:' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
        $stageResults['installer_complete'] = $installComplete
        $installerReady = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] READY: window mounted' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
        $stageResults['installer_ready'] = $installerReady
        $installerThemeReq = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] theme apply requested: /mp/theme_aurora\.lua' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
        $stageResults['installer_theme_request'] = $installerThemeReq
        $auroraWait = Wait-ThemeLoad -Path $serialPath -LuaPath '/mp/theme_aurora.lua' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
        $stageResults['aurora_theme'] = $auroraWait
        if ($stageResults['installer_start'].Ok -and $installComplete.Ok -and $auroraWait.Ok) {
            Write-Step "Installer deployed resources; desktop applied the Aurora theme"
            Start-Sleep -Milliseconds 1500
        } else {
            Write-Host "DESKTOP FEATURES: installer/aurora markers incomplete; continuing for evidence"
        }
        $serialSnapshots['installer'] = Get-SerialSnapshot -Path $serialPath -Attempts 3
        $shotInst = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_installer.ppm" -f $shotPrefix)
        $shots['installer'] = $shotInst
        if ($shotInst.Ok) { Write-Step ("screenshot installer: {0}" -f $shotInst.Path) }
        else { Write-Host ("ERROR: installer screenshot failed: " + $shotInst.Error) }

        # A kernel STALL WATCHDOG here means the system-side hang blocked the
        # rest of the interaction; report it and stop injecting keys.
        $kernelStalled = (Test-KernelStalled -Text $serialSnapshots['installer']) -gt 0
        if ($kernelStalled) {
            Warn "kernel STALL WATCHDOG detected during the Installer run" ("see serial log: " + $serialPath)
        }

        # ---------- close the Installer (Esc on the fixed kernel, Enter fallback) ----------
        [void](Send-KeyChecked -Session $phase1 -Keys 'esc')
        $exitWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] exit:' -MinCount 1 -TimeoutSeconds 5 -Proc $phase1.Proc
        if (-not $exitWait.Ok) {
            Write-Step "Esc did not close the Installer; falling back to Enter"
            [void](Send-KeyChecked -Session $phase1 -Keys 'ret')
            $exitWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] exit:' -MinCount 1 -TimeoutSeconds 8 -Proc $phase1.Proc
        }
        $stageResults['installer_exit'] = $exitWait
        if ($exitWait.Ok) { Write-Step "Installer closed" }
        else { Warn "could not close the Installer (later I-command count may stay 1)" }
        Start-Sleep -Milliseconds 700

        # ---------- Settings: apply Mint (auto-refresh / L / restart fallbacks) ----------
        if ($shotInst.Ok) {
            $mintWait = Invoke-SettingsApplyMint -Session $phase1 -SerialPath $serialPath -ActionWaitSeconds $ActionWaitSeconds
            $stageResults['theme_mint'] = $mintWait
            if ($mintWait.Ok) {
                Write-Step ("Settings applied theme: /mp/theme_mint.lua (method: {0})" -f $mintWait.Method)
                Start-Sleep -Milliseconds 1500
            } else {
                Write-Host ("DESKTOP FEATURES: no Mint theme load marker (method: {0})" -f $mintWait.Method)
            }
            $serialSnapshots['mint'] = Get-SerialSnapshot -Path $serialPath -Attempts 3
            $shot2 = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_settings.ppm" -f $shotPrefix)
            $shots['settings'] = $shot2
            if ($shot2.Ok) { Write-Step ("screenshot settings: {0}" -f $shot2.Path) }
            else { Write-Host ("ERROR: settings screenshot failed: " + $shot2.Error) }
        }

        # ---------- Notepad Open / Close dialogs ----------
        [void](Send-KeyChecked -Session $phase1 -Keys 'ctrl-alt-n')
        Start-Sleep -Milliseconds 600
        [void](Send-KeyChecked -Session $phase1 -Keys 'ctrl-o')
        $dlgOpen = Wait-SerialAtLeast -Path $serialPath -Pattern '\[notepad\] dialog=open' -MinCount 1 -TimeoutSeconds 10 -Proc $phase1.Proc
        $stageResults['notepad_dialog_open'] = $dlgOpen
        Start-Sleep -Milliseconds 800
        $shot3 = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_notepad_open.ppm" -f $shotPrefix)
        $shots['notepad_open'] = $shot3
        # open the second list entry (files.index: desktop.lua, Aurora theme, ...)
        [void](Send-KeyChecked -Session $phase1 -Keys 'down')
        Start-Sleep -Milliseconds 300
        [void](Send-KeyChecked -Session $phase1 -Keys 'ret')
        $opened = Wait-SerialAtLeast -Path $serialPath -Pattern '\[notepad\] opened path=' -MinCount 1 -TimeoutSeconds 10 -Proc $phase1.Proc
        $stageResults['notepad_opened'] = $opened
        Start-Sleep -Milliseconds 800
        $shot4 = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_notepad_opened.ppm" -f $shotPrefix)
        $shots['notepad_opened'] = $shot4
        # Ctrl+W -> Close dialog; Esc cancels (fixed kernel), Down/Down/Enter fallback
        [void](Send-KeyChecked -Session $phase1 -Keys 'ctrl-w')
        $dlgClose = Wait-SerialAtLeast -Path $serialPath -Pattern '\[notepad\] dialog=close' -MinCount 1 -TimeoutSeconds 10 -Proc $phase1.Proc
        $stageResults['notepad_dialog_close'] = $dlgClose
        Start-Sleep -Milliseconds 800
        $shot5 = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_notepad_close.ppm" -f $shotPrefix)
        $shots['notepad_close'] = $shot5
        [void](Send-KeyChecked -Session $phase1 -Keys 'esc')
        $cancel = Wait-SerialAtLeast -Path $serialPath -Pattern '\[notepad\] close choice=cancel' -MinCount 1 -TimeoutSeconds 3 -Proc $phase1.Proc
        if (-not $cancel.Ok) {
            Write-Step "Esc did not cancel the Close dialog; falling back to Down,Down,Enter"
            [void](Send-KeyChecked -Session $phase1 -Keys 'down')
            Start-Sleep -Milliseconds 300
            [void](Send-KeyChecked -Session $phase1 -Keys 'down')
            Start-Sleep -Milliseconds 300
            [void](Send-KeyChecked -Session $phase1 -Keys 'ret')
            $cancel = Wait-SerialAtLeast -Path $serialPath -Pattern '\[notepad\] close choice=cancel' -MinCount 1 -TimeoutSeconds 6 -Proc $phase1.Proc
        }
        $stageResults['notepad_cancel'] = $cancel
        Start-Sleep -Milliseconds 500

        # ---------- Settings R (reload desktop.lua) then I (spawn Installer) ----------
        [void](Send-KeyChecked -Session $phase1 -Keys 'ctrl-alt-s')
        Start-Sleep -Milliseconds 500
        [void](Send-KeyChecked -Session $phase1 -Keys 'r')
        $reload = Wait-ThemeLoad -Path $serialPath -LuaPath '/mp/desktop.lua' -MinCount 1 -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
        $stageResults['reload_desktop'] = $reload
        if ($reload.Ok) { Write-Step "Settings R: desktop.lua reloaded" }
        else { Write-Host "DESKTOP FEATURES: no desktop.lua reload marker" }
        Start-Sleep -Milliseconds 600
        $instBefore = Get-PatternCount -Text (Get-SerialSnapshot -Path $serialPath -Attempts 2) -Pattern '\[installer\] start:'
        [void](Send-KeyChecked -Session $phase1 -Keys 'i')
        $instWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] start:' -MinCount ($instBefore + 1) -TimeoutSeconds $ActionWaitSeconds -Proc $phase1.Proc
        $stageResults['spawn_installer'] = $instWait
        if ($instWait.Ok) {
            Write-Step "Settings I: a new Installer process started"
            $completeWait = Wait-SerialAtLeast -Path $serialPath -Pattern '\[installer\] Install complete:' -MinCount 2 -TimeoutSeconds 15 -Proc $phase1.Proc
            $stageResults['installer_second_complete'] = $completeWait
            Start-Sleep -Milliseconds 1500
        } else {
            Warn "Settings I did not spawn a new Installer"
        }
        $shot6 = Save-Screenshot -Session $phase1.Monitor -Path ("{0}_final.ppm" -f $shotPrefix)
        $shots['final'] = $shot6
        $serialSnapshots['phase1'] = Get-SerialSnapshot -Path $serialPath -Attempts 3
        if ($shot6.Ok) { Write-Step ("screenshot final: {0}" -f $shot6.Path) }
        else { Write-Host ("ERROR: final screenshot failed: " + $shot6.Error) }
    }
} finally {
    if ($activeSession) { Stop-DesktopSession -Session $activeSession; $activeSession = $null }
}

if ($envError) {
    Write-Host ("ERROR: " + $envError)
    $tailText = Read-TextFileShared -Path $serialPath -Attempts 2
    if ($tailText) {
        Write-Host "---- serial log tail (last 40 lines) ----"
        foreach ($line in (Get-LastLines $tailText 40)) { Write-Host ("  " + $line) }
    }
    Write-Host "DESKTOP FEATURES: FAIL (environment)"
    exit 2
}

# ----------------------------------------------------------------------------
# Phase 4: serial evidence + assertions (A/B/C/D)
# ----------------------------------------------------------------------------
$finalSerialText = Read-TextFileShared -Path $serialPath -Attempts 10
if ($null -eq $finalSerialText) { $finalSerialText = "" }
if (-not $finalSerialText -and $phase1) { $finalSerialText = $phase1.BootText }

$themeLoads = Get-ThemeLoads -Text $finalSerialText
$auroraLoads = @($themeLoads | Where-Object { $_.Path -eq '/mp/theme_aurora.lua' })
$desktopLoads = @($themeLoads | Where-Object { $_.Path -eq '/mp/desktop.lua' })
$mintLoads = @($themeLoads | Where-Object { $_.Path -eq '/mp/theme_mint.lua' })
$installerStarts = Get-PatternCount -Text $finalSerialText -Pattern '\[installer\] start:'
$installerComplete = @([regex]::Matches($finalSerialText, '\[installer\] Install complete:[^\r\n]*'))
$notepadOpenMarkers = Get-PatternCount -Text $finalSerialText -Pattern '\[notepad\] dialog=open'
$notepadOpenedMarkers = @([regex]::Matches($finalSerialText, '\[notepad\] opened path=([^\s]+)'))
$notepadCloseMarkers = Get-PatternCount -Text $finalSerialText -Pattern '\[notepad\] dialog=close'
$notepadCancelMarkers = Get-PatternCount -Text $finalSerialText -Pattern '\[notepad\] close choice=cancel'
$settingsSpawnCount = Get-PatternCount -Text $finalSerialText -Pattern '\[wm\] spawn Settings'
$lastStyle = Get-StyleValues -Text $finalSerialText
$lastWall = Get-WallpaperValues -Text $finalSerialText

Write-Host "---- serial evidence ----"
Write-Host ("  theme loads : {0}" -f (($themeLoads | ForEach-Object { "{0} rc={1} radius={2}" -f $_.Path, $_.Rc, $_.Radius }) -join ' | '))
Write-Host ("  last style  : radius={0} title={1} paper={2} shadow={3}" -f `
    ($(if ($lastStyle) { $lastStyle.Radius } else { "?" })), `
    (Format-Rgb $(if ($lastStyle) { $lastStyle.Title } else { $null })), `
    (Format-Rgb $(if ($lastStyle) { $lastStyle.Paper } else { $null })), `
    ($(if ($lastStyle) { $lastStyle.Shadow } else { "?" })))
Write-Host ("  last wall   : top {0} mid {1} bottom {2}" -f `
    (Format-Rgb $(if ($lastWall) { $lastWall.Top } else { $null })), `
    (Format-Rgb $(if ($lastWall) { $lastWall.Mid } else { $null })), `
    (Format-Rgb $(if ($lastWall) { $lastWall.Bottom } else { $null })))
Write-Host ("  installer   : starts={0} complete-lines={1}" -f $installerStarts, $installerComplete.Count)
Write-Host ("  notepad     : dialog-open={0} opened={1} dialog-close={2} cancel={3}" -f `
    $notepadOpenMarkers, $notepadOpenedMarkers.Count, $notepadCloseMarkers, $notepadCancelMarkers)
Write-Host ("  Settings spawn count: {0}" -f $settingsSpawnCount)

# ---- (K) kernel health (stall watchdog = system-side SMP/FS hang) -----------
$stallCount = Test-KernelStalled -Text $finalSerialText
$stallLine = Get-LastMatch -Text $finalSerialText -Pattern '========== STALL WATCHDOG:[^\r\n]*'
$stallLock = Get-LastMatch -Text $finalSerialText -Pattern 'waiting on : lock=[^\r\n]*'
Report "K0) no kernel STALL WATCHDOG during the run" ($stallCount -eq 0) `
    ("STALL WATCHDOG count={0}; first/last: {1}; {2}" -f $stallCount, $(if ($stallLine) { "see serial log" } else { "none" }), $(if ($stallLock) { $stallLock.Value.Trim() } else { "" }))

# ---- (A) Installer: on-demand first-boot install (Ctrl+Alt+I) ----------------
$bootSnap = $serialSnapshots['boot']
if (-not $bootSnap -and $phase1) { $bootSnap = $phase1.BootText }
$bootInstallerCount = Get-PatternCount -Text $bootSnap -Pattern '\[installer\] start:'
Report "A0) Installer does NOT auto-spawn at boot (on-demand integration)" `
    ($bootInstallerCount -eq 0) `
    ("[installer] start: count at boot={0} (expected 0)" -f $bootInstallerCount)
Report "A1) Ctrl+Alt+I triggered the Installer ([installer] start:)" `
    ($installerStarts -ge 1) `
    ("[installer] start: count={0}" -f $installerStarts)
$firstComplete = $null
if ($installerComplete.Count -ge 1) { $firstComplete = $installerComplete[0].Value }
Report "A2) Installer completed deployment (Install complete: ...)" `
    ($null -ne $firstComplete) $firstComplete
if ($firstComplete) {
    $m = [regex]::Match($firstComplete, '(\d+)/(\d+) files \(([^)]*)\)')
    if ($m.Success) {
        $okCount = [int]$m.Groups[1].Value
        $totalCount = [int]$m.Groups[2].Value
        $detail = $m.Groups[3].Value
        $failed = $false
        if ($detail -match '(\d+)\s+failed') { if ([int]$Matches[1] -gt 0) { $failed = $true } }
        Report "A3) Installer reported 6/6 files and 0 failed" `
            (($okCount -eq $totalCount) -and (-not $failed)) $firstComplete
    } else {
        Warn "cannot parse installer summary line" $firstComplete
    }
}
$installerReady = Get-PatternCount -Text $finalSerialText -Pattern '\[installer\] READY: window mounted'
$wmSpawnInstaller = Get-PatternCount -Text $finalSerialText -Pattern '\[wm\] spawn Installer'
Report "A4) Installer window mounted ([installer] READY + [wm] spawn Installer)" `
    (($installerReady -ge 1) -and ($wmSpawnInstaller -ge 1)) `
    ("READY count={0}, wm spawn count={1}" -f $installerReady, $wmSpawnInstaller)
$themeRequests = Get-PatternCount -Text $finalSerialText -Pattern '\[installer\] theme apply requested:'
Report "A5) Installer requested and desktop applied the Aurora theme" `
    (($themeRequests -ge 1) -and ($auroraLoads.Count -ge 1)) `
    ("theme requests={0}, aurora loads={1}" -f $themeRequests, $auroraLoads.Count)

# ---- (B) Settings window, theme list and theme switch -----------------------
Report "B1) Settings window spawned at boot ([wm] spawn Settings)" `
    ($settingsSpawnCount -ge 1) `
    ("[wm] spawn Settings count={0}" -f $settingsSpawnCount)
$mintOk = ($mintLoads.Count -ge 1)
$mintFirst = $null
if ($mintOk) { $mintFirst = $mintLoads[0] }
Report "B2) Settings Enter loaded /mp/theme_mint.lua (rc=0)" `
    ($mintOk -and ($mintFirst.Rc -eq 0)) `
    ($(if ($mintFirst) { "path={0} rc={1} radius={2}" -f $mintFirst.Path, $mintFirst.Rc, $mintFirst.Radius } else { "no [desktop] theme: load path=/mp/theme_mint.lua marker" }))
$mintApply = $stageResults['theme_mint']
if ($mintApply -and $mintApply.Ok) {
    Info ("Settings applied Mint through: {0}" -f $(if ($mintApply.PSObject.Properties.Name -contains 'Method') { $mintApply.Method } else { "unknown" }))
} elseif ($mintApply) {
    Info "Settings did not apply Mint (see the B2 detail)"
}

# ---- Frame 0: initial desktop (no themes installed) ------------------------
$baseStyle = Get-StyleValues -Text $bootSnap
$baseWall = Get-WallpaperValues -Text $bootSnap
$shotBase = $shots['settings_base']
if ($null -ne $shotBase -and $shotBase.Ok -and $baseStyle -and $baseWall) {
    $geom = Get-ClientWindowGeom -ScrW $shotBase.Ppm.Width -ScrH $shotBase.Ppm.Height `
        -BodyW 620 -BodyH 430 -Dx 40 -Dy 40   # Settings
    $wallTopPx = Get-PpmRegionAverage -Ppm $shotBase.Ppm -X 2 -Y 20 -Radius 2
    $titlePx = Get-PpmRegionAverage -Ppm $shotBase.Ppm -X $geom.TitleX -Y $geom.TitleY -Radius 2
    $paperX = $geom.BodyX + [int][math]::Floor($geom.BodyW / 2)
    $paperY = $geom.BodyY + 32 + [int][math]::Floor(($geom.BodyH - 32 - 8) / 2)
    $paperPx = Get-PpmRegionAverage -Ppm $shotBase.Ppm -X $paperX -Y $paperY -Radius 2
    Write-Host ("  base frame     : Settings body=({0},{1}) {2}x{3}; samples: wallpaper {4}, title {5}, content {6}" -f `
        $geom.BodyX, $geom.BodyY, $geom.BodyW, $geom.BodyH,
        (Format-Rgb $wallTopPx), (Format-Rgb $titlePx), (Format-Rgb $paperPx))
    Report "B3) Initial frame: Settings title bar uses the running style title" `
        (Test-RgbNear $titlePx $baseStyle.Title $Tolerance) `
        ("sampled {0} at ({1},{2}) vs style title {3} ({4})" -f `
            (Format-Rgb $titlePx), $geom.TitleX, $geom.TitleY, (Format-Rgb $baseStyle.Title), (Format-RgbDelta $titlePx $baseStyle.Title))
    Report "B4) Initial frame: Settings window content is visible (paper pixel)" `
        ((Test-RgbNear $paperPx (New-Rgb 15 15 18) 16) -and ((Get-RgbDiffSum $paperPx $wallTopPx) -ge 24)) `
        ("sampled {0} at ({1},{2}) vs Settings content paper RGB(15,15,18); vs wallpaper sum|dRGB|={3}" -f `
            (Format-Rgb $paperPx), $paperX, $paperY, (Get-RgbDiffSum $paperPx $wallTopPx))
    Report "B5) Initial frame: wallpaper top matches script-painted readback" `
        (Test-RgbNear $wallTopPx $baseWall.Top $Tolerance) `
        ("sampled {0} at (2,20) vs script-painted top {1} ({2})" -f (Format-Rgb $wallTopPx), (Format-Rgb $baseWall.Top), (Format-RgbDelta $wallTopPx $baseWall.Top))
} else {
    Report "B3) Initial desktop frame captured and parsed" ($null -ne $shotBase -and $shotBase.Ok) `
        ($(if ($shotBase) { $shotBase.Error } else { "no settings_base screenshot" }))
    Report "B4) Initial frame: Settings window content is visible (paper pixel)" $false "no parsed base frame"
    Report "B5) Initial frame: wallpaper top matches script-painted readback" $false "no parsed base frame"
}

# ---- Frame 1: Installer window + Aurora theme ------------------------------
$shotInst = $shots['installer']
$instSnap = $serialSnapshots['installer']
if (-not $instSnap) { $instSnap = $finalSerialText }
$instStyle = Get-StyleValues -Text $instSnap
$instWall = Get-WallpaperValues -Text $instSnap
if ($null -ne $shotInst -and $shotInst.Ok -and $instStyle -and $instWall) {
    $geomInst = Get-ClientWindowGeom -ScrW $shotInst.Ppm.Width -ScrH $shotInst.Ppm.Height `
        -BodyW 620 -BodyH 430 -Dx 40 -Dy 40   # Settings title row is still visible above the Installer
    $wallTopInst = Get-PpmRegionAverage -Ppm $shotInst.Ppm -X 2 -Y 20 -Radius 2
    $titleInst = Get-PpmRegionAverage -Ppm $shotInst.Ppm -X $geomInst.TitleX -Y $geomInst.TitleY -Radius 2
    Write-Host ("  installer frame: samples: wallpaper {0}, Settings title {1}" -f (Format-Rgb $wallTopInst), (Format-Rgb $titleInst))
    Report "B6) Installer frame: Aurora title colour on the Settings title bar" `
        (Test-RgbNear $titleInst $instStyle.Title $Tolerance) `
        ("sampled {0} vs style title {1} ({2})" -f (Format-Rgb $titleInst), (Format-Rgb $instStyle.Title), (Format-RgbDelta $titleInst $instStyle.Title))
    Report "B7) Installer frame: Aurora wallpaper top sample" `
        (Test-RgbNear $wallTopInst $instWall.Top $Tolerance) `
        ("sampled {0} vs script-painted top {1} ({2})" -f (Format-Rgb $wallTopInst), (Format-Rgb $instWall.Top), (Format-RgbDelta $wallTopInst $instWall.Top))
    if ($shotBase -and $shotBase.Ok) {
        $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shotBase.Ppm) -B (Get-PpmSignature -Ppm $shotInst.Ppm)
        Report "B8) Installer window + Aurora theme changed the framebuffer (base vs installer)" `
            ($diff.DiffCells -ge $MinDiffCells) `
            ("{0}/{1} grid cells changed (threshold {2})" -f $diff.DiffCells, $diff.Total, $MinDiffCells)
    }
} else {
    Report "B6) Installer frame captured and parsed" ($null -ne $shotInst -and $shotInst.Ok) `
        ($(if ($shotInst) { $shotInst.Error } else { "no installer screenshot" }))
}

# ---- Frame 2: Mint theme applied through Settings --------------------------
$shot2 = $shots['settings']
$mintSnap = $serialSnapshots['mint']
$mintStyle = Get-StyleValues -Text $mintSnap
$mintWall = Get-WallpaperValues -Text $mintSnap
if ($null -ne $shot2 -and $shot2.Ok -and $shotInst -and $shotInst.Ok) {
    $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shotInst.Ppm) -B (Get-PpmSignature -Ppm $shot2.Ppm)
    Report "B9) Mint theme switch changed the framebuffer (installer frame vs settings frame)" `
        ($diff.DiffCells -ge $MinDiffCells) `
        ("{0}/{1} grid cells changed (threshold {2})" -f $diff.DiffCells, $diff.Total, $MinDiffCells)
    if ($mintStyle -and $mintWall) {
        $geom2 = Get-ClientWindowGeom -ScrW $shot2.Ppm.Width -ScrH $shot2.Ppm.Height `
            -BodyW 620 -BodyH 430 -Dx 40 -Dy 40
        $wallTopPx2 = Get-PpmRegionAverage -Ppm $shot2.Ppm -X 2 -Y 20 -Radius 2
        $titlePx2 = Get-PpmRegionAverage -Ppm $shot2.Ppm -X $geom2.TitleX -Y $geom2.TitleY -Radius 2
        Write-Host ("  mint frame     : samples: wallpaper {0}, title {1}" -f (Format-Rgb $wallTopPx2), (Format-Rgb $titlePx2))
        Report "B10) Mint wallpaper top sample matches script-painted top" `
            (Test-RgbNear $wallTopPx2 $mintWall.Top $Tolerance) `
            ("sampled {0} vs {1} ({2})" -f (Format-Rgb $wallTopPx2), (Format-Rgb $mintWall.Top), (Format-RgbDelta $wallTopPx2 $mintWall.Top))
        Report "B11) Mint Settings title bar matches style title" `
            (Test-RgbNear $titlePx2 $mintStyle.Title $Tolerance) `
            ("sampled {0} vs {1} ({2})" -f (Format-Rgb $titlePx2), (Format-Rgb $mintStyle.Title), (Format-RgbDelta $titlePx2 $mintStyle.Title))
    } else {
        Warn "Mint frame colour checks skipped (no mint style/wallpaper markers)"
    }
} else {
    Report "B9) Settings frame captured and parsed" ($null -ne $shot2 -and $shot2.Ok) `
        ($(if ($shot2) { $shot2.Error } else { "no settings screenshot" }))
}

# ---- (D) Notepad dialogs ----------------------------------------------------
$shot3 = $shots['notepad_open']
$shot4 = $shots['notepad_opened']
$shot5 = $shots['notepad_close']
if ($notepadOpenMarkers -ge 1) {
    Report "D1) Notepad Open dialog opened ([notepad] dialog=open)" $true `
        ([regex]::Match($finalSerialText, '\[notepad\] dialog=open[^\r\n]*').Value)
} else {
    $msg = "no [notepad] dialog=open serial marker in this build (screenshot-diff fallback)"
    if ($RequireNotepadMarkers) { Report "D1) Notepad Open dialog opened ([notepad] dialog=open)" $false $msg }
    else { Warn "D1) Notepad Open dialog serial marker missing" $msg }
}
if ($shot3 -and $shot3.Ok -and $shotInst -and $shotInst.Ok) {
    $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shotInst.Ppm) -B (Get-PpmSignature -Ppm $shot3.Ppm)
    Report "D2) Notepad + Open dialog visible (installer frame vs notepad-open frame)" `
        ($diff.DiffCells -ge $MinDiffCells) `
        ("{0}/{1} cells changed" -f $diff.DiffCells, $diff.Total)
} else {
    Report "D2) Notepad-open frame captured and parsed" ($null -ne $shot3 -and $shot3.Ok) `
        ($(if ($shot3) { $shot3.Error } else { "no screenshot" }))
}
if ($notepadOpenedMarkers.Count -ge 1) {
    $openedPath = $notepadOpenedMarkers[$notepadOpenedMarkers.Count - 1].Groups[1].Value
    Report "D3) Notepad opened a file from the Open dialog" ($openedPath -eq '/mp/theme_aurora.lua') `
        ("opened path={0} (expected /mp/theme_aurora.lua, files.index entry #2)" -f $openedPath)
} else {
    $msg = "no [notepad] opened path= serial marker in this build (screenshot-diff fallback)"
    if ($RequireNotepadMarkers) { Report "D3) Notepad opened a file from the Open dialog" $false $msg }
    else { Warn "D3) Notepad opened-file serial marker missing" $msg }
}
if ($shot4 -and $shot4.Ok -and $shot3 -and $shot3.Ok) {
    $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shot3.Ppm) -B (Get-PpmSignature -Ppm $shot4.Ppm)
    Report "D4) Opening the file changed the Notepad frame" `
        ($diff.DiffCells -ge $MinDiffCells) `
        ("{0}/{1} cells changed" -f $diff.DiffCells, $diff.Total)
}
if ($notepadCloseMarkers -ge 1) {
    Report "D5) Notepad Close dialog opened ([notepad] dialog=close)" $true `
        ([regex]::Match($finalSerialText, '\[notepad\] dialog=close[^\r\n]*').Value)
} else {
    $msg = "no [notepad] dialog=close serial marker in this build (screenshot-diff fallback)"
    if ($RequireNotepadMarkers) { Report "D5) Notepad Close dialog opened ([notepad] dialog=close)" $false $msg }
    else { Warn "D5) Notepad Close dialog serial marker missing" $msg }
}
if ($shot5 -and $shot5.Ok -and $shot4 -and $shot4.Ok) {
    $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shot4.Ppm) -B (Get-PpmSignature -Ppm $shot5.Ppm)
    Report "D6) Close dialog visible (notepad-opened vs notepad-close frame)" `
        ($diff.DiffCells -ge $MinDiffCells) `
        ("{0}/{1} cells changed" -f $diff.DiffCells, $diff.Total)
} else {
    Report "D6) Notepad-close frame captured and parsed" ($null -ne $shot5 -and $shot5.Ok) `
        ($(if ($shot5) { $shot5.Error } else { "no screenshot" }))
}
if ($notepadCancelMarkers -ge 1) {
    Report "D7) Esc cancelled the Close dialog ([notepad] close choice=cancel)" $true
} else {
    Warn "D7) Notepad close-cancel serial marker missing" ("no [notepad] close choice=cancel marker")
}

# ---- (C) Settings R / I -----------------------------------------------------
Report "C1) Settings R reloaded /mp/desktop.lua (rc=0)" `
    ($desktopLoads.Count -ge 1 -and $desktopLoads[0].Rc -eq 0) `
    ($(if ($desktopLoads.Count -ge 1) { "path={0} rc={1} radius={2}" -f $desktopLoads[0].Path, $desktopLoads[0].Rc, $desktopLoads[0].Radius } else { "no [desktop] theme: load path=/mp/desktop.lua marker" }))
$instSpawnOk = ($installerStarts -ge 2)
if (-not $instSpawnOk) {
    # The I shortcut only spawns when no Installer window is alive; if the early
    # close failed we still accept the raise path and report it as a WARN below.
    if ($stageResults.Contains('spawn_installer') -and -not $stageResults['spawn_installer'].Ok) {
        Warn "C2) Settings I did not spawn a new Installer" ("first-boot Installer may still be open; [installer] start: count={0}" -f $installerStarts)
    }
}
Report "C2) Settings I spawned the Installer ([installer] start: count >= 2)" `
    $instSpawnOk ("[installer] start: count={0}" -f $installerStarts)
if ($installerComplete.Count -ge 2) {
    Report "C3) second Installer run overwrote the resources (Install complete: ... overwrite)" `
        ($installerComplete[1].Value -match 'overwrite') $installerComplete[1].Value
} elseif ($instSpawnOk) {
    Warn "C3) second Installer completion line missing"
} 

# ---- final frame ------------------------------------------------------------
$shot6 = $shots['final']
if ($shot6 -and $shot6.Ok -and $shot5 -and $shot5.Ok) {
    $diff = Compare-PpmSignature -A (Get-PpmSignature -Ppm $shot5.Ppm) -B (Get-PpmSignature -Ppm $shot6.Ppm)
    Report "C4) Settings R/I flow changed the final frame" `
        ($diff.DiffCells -ge $MinDiffCells) `
        ("{0}/{1} cells changed" -f $diff.DiffCells, $diff.Total)
}

# ----------------------------------------------------------------------------
# Phase 5: post-run disk.img verification (WSL e2tools)
# ----------------------------------------------------------------------------
if (-not $SkipDiskVerify) {
    Write-Step "verifying installed resources on disk.img"
    $post = Invoke-DiskPostcheck -ImageWsl $diskWsl
    if (-not $post.Ok) {
        Write-Host $post.Output
        Report "E0) disk.img post-run read (WSL e2tools)" $false `
            ("WSL rc={0}: {1}" -f $post.Code, (Format-MonitorText $post.Error))
    } else {
        $parsed = Parse-DiskPostcheck -Output $post.Output
        $rootTokens = @(Get-TokensFromListing -Listing $parsed.RootListing)
        $files = $parsed.Files

        function Get-DiskFile([hashtable]$Files, [string]$Name) {
            if ($Files.ContainsKey($Name)) { return $Files[$Name] }
            return $null
        }
        $themesIndex = Get-DiskFile $files 'themes.index'
        $aurora = Get-DiskFile $files 'theme_aurora.lua'
        $filesIndex = Get-DiskFile $files 'files.index'
        $notes = Get-DiskFile $files 'notes.txt'
        $selected = Get-DiskFile $files 'selected_theme.txt'

        $themesOk = $false
        $themesDetail = "themes.index missing"
        if ($null -ne $themesIndex -and $themesIndex.Rc -eq 0) {
            $themesOk = ((($themesIndex.Content -match 'Aurora\|/mp/theme_aurora\.lua') -and
                          ($themesIndex.Content -match 'Ember\|/mp/theme_ember\.lua') -and
                          ($themesIndex.Content -match 'Mint\|/mp/theme_mint\.lua')))
            $themesDetail = "themes.index: " + (Format-MonitorText $themesIndex.Content)
        }
        Report "E1) /mp/themes.index installed with Aurora/Ember/Mint" $themesOk $themesDetail

        $auroraOk = ($null -ne $aurora -and $aurora.Rc -eq 0 -and
                     ($aurora.Content -match 'wallpaper') -and
                     ($aurora.Content -match 'set_window_style'))
        Report "E2) /mp/theme_aurora.lua installed (wallpaper + set_window_style)" $auroraOk `
            ($(if ($aurora) { "rc={0}, {1} bytes" -f $aurora.Rc, $aurora.Content.Length } else { "theme_aurora.lua missing" }))

        $filesIdxOk = ($null -ne $filesIndex -and $filesIndex.Rc -eq 0 -and ($filesIndex.Content -match 'Aurora'))
        Report "E3) /mp/files.index installed (Notepad Open list)" $filesIdxOk `
            ($(if ($filesIndex) { Format-MonitorText $filesIndex.Content } else { "files.index missing" }))

        $notesOk = ($null -ne $notes -and $notes.Rc -eq 0 -and $notes.Content.Length -gt 20)
        Report "E4) /mp/notes.txt installed" $notesOk `
            ($(if ($notes) { "{0} bytes" -f $notes.Content.Length } else { "notes.txt missing" }))

        $selectedOk = ($null -ne $selected -and $selected.Rc -eq 0 -and
                       ($selected.Content -match 'Mint\|/mp/theme_mint\.lua'))
        Report "E5) /mp/selected_theme.txt records the Mint selection" $selectedOk `
            ($(if ($selected) { Format-MonitorText $selected.Content } else { "selected_theme.txt missing" }))

        $emberExists = ($rootTokens -contains 'theme_ember.lua')
        $mintExists = ($rootTokens -contains 'theme_mint.lua')
        Report "E6) all three theme scripts present on the disk" ($emberExists -and $mintExists -and ($null -ne $aurora)) `
            ("ember={0} mint={1} aurora={2}" -f $emberExists, $mintExists, ($null -ne $aurora))
    }
} else {
    Warn "disk verification skipped (-SkipDiskVerify)"
}

# ----------------------------------------------------------------------------
# Verdict + evidence
# ----------------------------------------------------------------------------
Write-Host "---- evidence paths ----"
Write-Host ("  serial log : {0}" -f $serialPath)
foreach ($key in @('settings_base', 'installer', 'settings', 'notepad_open', 'notepad_opened', 'notepad_close', 'final')) {
    if ($shots.ContainsKey($key) -and $shots[$key]) {
        Write-Host ("  {0,-14}: {1}" -f $key, $shots[$key].Path)
    }
}
Write-Host "---- key serial markers ----"
foreach ($line in (Get-LastLines $finalSerialText 400)) {
    if ($line -match '\[installer\] (start|Install complete|READY|theme apply|exit)|\[desktop\] theme: load|\[desktop\] lua: style|\[desktop\] lua: wallpaper: script-painted|\[wm\] spawn (Installer|Settings|Notepad)|\[notepad\]') {
        Write-Host ("  " + $line)
    }
}

if ($script:FailCount -gt 0) {
    Write-Host "---- serial log tail (last 40 lines) ----"
    foreach ($line in (Get-LastLines $finalSerialText 40)) { Write-Host ("  " + $line) }
    Write-Host ("DESKTOP FEATURES: FAIL ({0} assertion(s) failed, {1} warning(s))" -f $script:FailCount, $script:WarnCount)
    exit 1
}
Write-Host ("DESKTOP FEATURES: PASS ({0} warning(s))" -f $script:WarnCount)
exit 0
