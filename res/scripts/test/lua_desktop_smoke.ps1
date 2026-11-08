#Requires -Version 5.1
# ============================================================================
# res/scripts/test/lua_desktop_smoke.ps1
#   Headless QEMU gate for the desktop's EMBEDDED Lua theme configuration.
#
# WHAT IT PROVES
#   (1) the desktop process really starts:
#         [desktop] start: fb=<addr> <w>x<h> psl=<pitch> bufsz=<bytes>
#   (2) the embedded Lua runtime really ran and got the wallpaper framebuffer
#       (Lua 5.4 in programs/desktop/lua, glue in programs/desktop/lua_glue.cpp):
#         [desktop] lua: init (Lua 5.4) wallpaper_fb=<addr> <w>x<h>
#         [desktop] lua: source=file|embedded
#   (3) the desktop theme is configured by Lua:
#         [desktop] lua: style radius=<n> title=0x...... border=0x......
#                       ink=0x...... close=0x...... paper=0x...... shadow=<n>
#       and the WALLPAPER IS PAINTED BY THE SCRIPT ITSELF into that framebuffer
#       (C++ no longer parses gradient/color theme data).  The glue reads the
#       script-painted pixels back as serial evidence:
#         [desktop] lua: wallpaper: script-painted fb=<addr> <w>x<h>
#                       top=0x... mid=0x... bottom=0x...
#       (top = row 0 x=0, mid = row h/2 centre, bottom = row h-1 centre)
#   (4) those values reached the FRAMEBUFFER: a QEMU hidden-monitor
#       "screendump" PPM (P6) is parsed with pure PowerShell and the wallpaper
#       sample points / the centered console window title bar are compared with
#       the expected colors (per-channel tolerance).
#
# WHERE THE EXPECTED COLORS COME FROM
#   WALLPAPER: only explicit -ExpectWallpaperTop / -ExpectWallpaperBottom are
#   compared against pixel samples.  The script does NOT try to invert whatever
#   drawing the Lua script did (it could be any shape/gradient/flat fill): when
#   the explicit expectations are absent it only checks sanity properties --
#   pixels are not left pure black and the wall is visually distinct from the
#   window chrome -- and prints SKIP/INFO lines.
#   WINDOW CHROME: explicit -ExpectTitleColor wins, otherwise the window style
#   line printed by the glue is used.
#
# USAGE (run AFTER the Lead built the ISO and the user disk image)
#   powershell -ExecutionPolicy Bypass -File res/scripts/test/lua_desktop_smoke.ps1 `
#       -UseDisk -WaitSeconds 90 -MinSelftestWait 20 -RequireSource file `
#       -ExpectWallpaperTop 0x102040 -ExpectWallpaperBottom 0x7A2F6E `
#       -ExpectTitleColor 0x232A45
#   powershell -ExecutionPolicy Bypass -File res/scripts/test/lua_desktop_smoke.ps1
#   (the plain form only sanity-checks the wallpaper and derives the title
#    colour from the window-style serial marker)
#   powershell -ExecutionPolicy Bypass -File res/scripts/test/lua_desktop_smoke.ps1 -NoDisk
#
#   Extra parameters:
#     -Root <dir>            repository root (default: 3 levels above this script)
#     -Qemu <exe>            qemu-system-x86_64.exe
#     -Iso <file>            ISO path (default <Root>\SkylineSystem-x86_64.iso)
#     -Disk <file>           user disk image (default <Root>\disk.img)
#     -UseDisk / -NoDisk     force attach / force skip disk.img
#                            (default: attach it when it exists)
#     -WaitSeconds <n>       serial-marker timeout (default 60)
#     -MinSelftestWait <n>   minimum total seconds from QEMU start to screendump
#                            (default 5; the desktop markers appear much earlier
#                            than the first fully composited frame)
#     -ScreenshotDelay <n>   extra settle seconds after "[perf] desktop ready"
#                            (default 2)
#     -DesktopReadyWaitSeconds <n>  max wait for "[perf] desktop ready in ..."
#                            before taking the screenshot anyway (default 20)
#     -SerialLog <file>      default <Root>\lua_desktop_serial.log
#     -Screenshot <file>     default <Root>\lua_desktop.ppm (always kept as evidence)
#     -RequireSource <s>     "file" or "embedded": fail unless that source is used
#     -Tolerance <n>         per-channel color tolerance (default 12)
#     -MonitorPort <n>       fixed HMP telnet port (default: free ephemeral port)
#     -SelfTestPpm <file>    offline: parse/sample a PPM and exit (no QEMU)
#     -SelfTestSerial <file> offline: validate serial markers in a saved log and exit
#
# PIXEL SAMPLING GEOMETRY (1280x800 typical; derived from the actual PPM size)
#   Window body is 620x400, WM centers the 672x452 body+shadow surface
#   (lib/include/graphic/winstyle.h), so:
#       body left  = (W - 620) / 2          body top = (H - 400) / 2
#   wallpaper top    : (x=2, y=20)            left column, cursor-initial-area safe
#   wallpaper bottom : (x=W/2, y=H-80)        above the 44px acrylic taskbar
#   title bar        : (x=W/2-80, W/2, W/2+80 ; y=bodyTop+8)
#                      +8 keeps clear of the 1px rounded rim and of the caption
#                      glyphs/caption buttons, and is above the first client
#                      window spawned by the desktop (Notepad/DOOM start lower).
#   Each sample is a 5x5 average to shed single-pixel noise / AA.
#
# NOTES / ASSUMPTIONS
#   - Windows PowerShell 5.1 compatible (no PS7-only syntax).  Windows only:
#     HMP over telnet TCP, FileShare.ReadWrite|Delete serial reads.
#   - HMP "screendump" needs a whitespace-free filename; when -Screenshot
#     contains spaces the dump is written to a temp staging file and copied.
#   - 1280x800 is the expected QEMU/Limine resolution, but all sample
#     coordinates are computed from the real PPM header, so other sizes work.
#   - The wallpaper is arbitrary Lua output.  Without explicit -Expect*
#     wallpaper colors the pixel checks are sanity-only (see above), so a
#     script-painted wallpaper of any shape never causes a false color failure.
#   - The old "[desktop] lua: wallpaper type=..." marker was removed from the
#     glue; if it is still present in a serial log this script only prints a
#     WARN (old build) and never depends on it.
#   - This script builds nothing and runs only the ISO/disk passed in.
#
# EXIT CODES: 0 = PASS, 1 = assertion/screenshot failure, 2 = environment error.
# ============================================================================
param(
    [string]$Root = "",
    [string]$Qemu = "",
    [string]$Iso = "",
    [string]$Disk = "",
    [switch]$UseDisk,
    [switch]$NoDisk,
    [int]$WaitSeconds = 60,
    [int]$MinSelftestWait = 5,
    [int]$ScreenshotDelay = 2,
    [int]$DesktopReadyWaitSeconds = 20,
    [string]$SerialLog = "",
    [string]$Screenshot = "",
    [string]$ExpectWallpaperTop = "",
    [string]$ExpectWallpaperBottom = "",
    [string]$ExpectTitleColor = "",
    [string]$RequireSource = "",
    [int]$Tolerance = 12,
    [int]$MonitorPort = 0,
    [string]$SelfTestPpm = "",
    [string]$SelfTestSerial = ""
)

$ErrorActionPreference = "Continue"
$script:FailCount = 0

# ----------------------------------------------------------------------------
# Path resolution: explicit args > environment > repository root from script path
# ----------------------------------------------------------------------------
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = if ($Root) { $Root }
        elseif ($env:SKYLINE_ROOT) { $env:SKYLINE_ROOT }
        else { Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $scriptDir)) }
$qemu = if ($Qemu) { $Qemu }
        elseif ($env:SKYLINE_QEMU) { $env:SKYLINE_QEMU }
        else { "C:\Program Files\qemu\qemu-system-x86_64.exe" }

# Make everything absolute so QEMU / HMP / file reads see a stable path.
try { $root = [System.IO.Path]::GetFullPath($root) } catch { }
$isoPath = if ($Iso) { $Iso } else { Join-Path $root "SkylineSystem-x86_64.iso" }
$diskPath = if ($Disk) { $Disk } else { Join-Path $root "disk.img" }
$serialPath = if ($SerialLog) { $SerialLog } else { Join-Path $root "lua_desktop_serial.log" }
$shotPath = if ($Screenshot) { $Screenshot } else { Join-Path $root "lua_desktop.ppm" }
try { $serialPath = [System.IO.Path]::GetFullPath($serialPath) } catch { }
try { $shotPath = [System.IO.Path]::GetFullPath($shotPath) } catch { }

# ============================================================================
# SHARED HELPERS (pure functions: no side effects beyond stdout; this block is
# also extracted verbatim by the offline self-tests, keep it self-contained).
# ============================================================================
function Write-Info {
    param([string]$Text)
    Write-Host ("LUA DESKTOP SMOKE: {0}" -f $Text)
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
    return [pscustomobject]@{
        R = [int]$R
        G = [int]$G
        B = [int]$B
    }
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
    if ([math]::Abs([int]$A.R - [int]$B.R) -gt $Tolerance) { return $false }
    if ([math]::Abs([int]$A.G - [int]$B.G) -gt $Tolerance) { return $false }
    if ([math]::Abs([int]$A.B - [int]$B.B) -gt $Tolerance) { return $false }
    return $true
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

# Parse a user supplied color: 0xRRGGBB, #RRGGBB, RRGGBB or 0xAARRGGBB
# (the Skyline convention keeps the low 24 bits, see desktop.lua docs).
function Convert-UserColor {
    param([string]$Text)
    if (-not $Text) { return $null }
    $t = $Text.Trim()
    if ($t -match '^(?:#|0[xX])?([0-9A-Fa-f]{6})$') {
        return (Convert-Hex6ToRgb $Matches[1])
    }
    if ($t -match '^(?:#|0[xX])?([0-9A-Fa-f]{8})$') {
        return (Convert-Hex6ToRgb $Matches[1].Substring(2))
    }
    return $null
}

function Get-FirstCapture {
    param([string]$Text, [string]$Pattern, [int]$Group = 1)
    if (-not $Text) { return $null }
    $m = [regex]::Match($Text, $Pattern)
    if ($m.Success) { return $m.Groups[$Group].Value }
    return $null
}

# Return the whole line that contains $Pattern (ASCII markers, tolerant of
# kernel prefixes / ANSI payload because the search is unanchored).
function Get-MarkerLine {
    param([string]$Text, [string]$Pattern)
    if (-not $Text) { return $null }
    $m = [regex]::Match($Text, $Pattern + '[^\r\n]*')
    if ($m.Success) { return $m.Value.TrimEnd() }
    return $null
}

function Get-LastLines {
    param([string]$Text, [int]$Count)
    if (-not $Text) { return @() }
    $lines = $Text -split "`n"
    $start = [math]::Max(0, $lines.Count - $Count)
    $out = @()
    for ($i = $start; $i -lt $lines.Count; $i++) {
        $out += ($lines[$i]).TrimEnd("`r")
    }
    return $out
}

# Read a file QEMU may still be writing. FileShare.ReadWrite|Delete is used so a
# live -serial file: is readable, and a killed QEMU cannot block the final read.
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
# PPM P6 parser (pure PowerShell, no external libraries).
# Layout: "P6" <ws/comments> <w> <ws/comments> <h> <ws/comments> <maxval>
#         <one whitespace> <w*h*3 raw RGB bytes>
# The single whitespace after maxval is consumed exactly (comments are NOT
# allowed there by the spec: raster data may legitimately start with '#').
# ---------------------------------------------------------------------------
function Skip-PpmSeparators {
    param([byte[]]$Bytes, [hashtable]$State)
    while ($State.Pos -lt $State.Len) {
        $b = [int]$Bytes[$State.Pos]
        if ($b -eq 32 -or $b -eq 9 -or $b -eq 10 -or $b -eq 13) {
            $State.Pos = $State.Pos + 1
            continue
        }
        if ($b -eq 35) { # '#': comment runs to end of line
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
    if ($State.Pos -ge $State.Len) { throw "PPM header truncated (expected an integer)" }
    $start = [int]$State.Pos
    while ($State.Pos -lt $State.Len) {
        $b = [int]$Bytes[$State.Pos]
        if ($b -ge 48 -and $b -le 57) { $State.Pos = $State.Pos + 1 } else { break }
    }
    if ($State.Pos -eq $start) { throw "PPM header contains a non-numeric token where an integer was expected" }
    $text = [System.Text.Encoding]::ASCII.GetString($Bytes, $start, $State.Pos - $start)
    return [int]$text
}

function Read-PpmFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw ("PPM file not found: {0}" -f $Path) }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $state = @{ Pos = 0; Len = [int]$bytes.Length }
    Skip-PpmSeparators -Bytes $bytes -State $state
    if (($state.Pos + 1) -ge $state.Len) { throw "PPM file is too short for a P6 header" }
    $c0 = [char][int]$bytes[$state.Pos]
    $c1 = [char][int]$bytes[$state.Pos + 1]
    if ($c0 -ne 'P' -or $c1 -ne '6') {
        throw ("unsupported PPM magic '{0}{1}' (only binary P6 is supported)" -f $c0, $c1)
    }
    $state.Pos = $state.Pos + 2
    Skip-PpmSeparators -Bytes $bytes -State $state
    $w = Read-PpmInteger -Bytes $bytes -State $state
    Skip-PpmSeparators -Bytes $bytes -State $state
    $h = Read-PpmInteger -Bytes $bytes -State $state
    Skip-PpmSeparators -Bytes $bytes -State $state
    $maxv = Read-PpmInteger -Bytes $bytes -State $state
    if ($w -le 0 -or $h -le 0) { throw ("invalid PPM size {0}x{1}" -f $w, $h) }
    if ($maxv -ne 255) { throw ("unsupported PPM maxval {0} (only 255 is supported)" -f $maxv) }
    if ($state.Pos -ge $state.Len) { throw "PPM header has no raster data" }
    $sep = [int]$bytes[$state.Pos]
    if (-not ($sep -eq 32 -or $sep -eq 9 -or $sep -eq 10 -or $sep -eq 13)) {
        throw "PPM header is missing the whitespace separator before the raster"
    }
    $state.Pos = $state.Pos + 1
    if ($sep -eq 13 -and $state.Pos -lt $state.Len -and [int]$bytes[$state.Pos] -eq 10) {
        $state.Pos = $state.Pos + 1   # CRLF separator
    }
    $dataStart = [int]$state.Pos
    $need = [int64]$w * [int64]$h * 3
    if (([int64]$dataStart + $need) -gt [int64]$state.Len) {
        throw ("PPM raster truncated: {0} bytes declared, {1} bytes present" -f `
            $need, ([int64]$state.Len - [int64]$dataStart))
    }
    return [pscustomobject]@{
        Width     = [int]$w
        Height    = [int]$h
        MaxVal    = [int]$maxv
        Bytes     = $bytes
        DataStart = $dataStart
        Path      = $Path
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
    $sumR = 0
    $sumG = 0
    $sumB = 0
    $count = 0
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

# ---------------------------------------------------------------------------
# Serial-marker extraction.  Every marker is ASCII; unanchored substring
# matching keeps this robust against kernel prefixes / interleaved output.
# ---------------------------------------------------------------------------
function Get-LuaMarkers {
    param([string]$Text)
    $markers = [pscustomobject]@{
        DesktopStart  = $null
        CompositorUp  = $null
        LuaInit       = $null
        InitWallpaperFb = $null
        InitWallpaperW  = $null
        InitWallpaperH  = $null
        LuaSource     = $null
        SourceValue   = $null
        StyleLine     = $null
        StyleRadius   = $null
        StyleTitleHex = $null
        StyleInkHex   = $null
        StylePaperHex = $null
        StyleShadow   = $null
        WallpaperLine = $null
        WallFb        = $null
        WallWidth     = $null
        WallHeight    = $null
        WallTopHex    = $null
        WallMidHex    = $null
        WallBottomHex = $null
        LegacyWallpaperLine = $null
        LuaError      = $null
        FileLuaLog    = $null
        DesktopReady  = $null
    }
    if (-not $Text) { return $markers }

    $markers.DesktopStart = Get-MarkerLine $Text '\[desktop\] start: fb='
    $markers.CompositorUp = Get-MarkerLine $Text '\[desktop\] compositor up'
    $markers.LuaInit = Get-MarkerLine $Text '\[desktop\] lua: init'
    $markers.LuaSource = Get-MarkerLine $Text '\[desktop\] lua: source='
    $markers.StyleLine = Get-MarkerLine $Text '\[desktop\] lua: style '
    # New (framebuffer-painted) evidence line; the old "wallpaper type=..."
    # line is kept only to warn about a stale build and is never required.
    $markers.WallpaperLine = Get-MarkerLine $Text '\[desktop\] lua: wallpaper: script-painted'
    $markers.LegacyWallpaperLine = Get-MarkerLine $Text '\[desktop\] lua: wallpaper type='
    $markers.LuaError = Get-MarkerLine $Text '\[desktop\] lua: error'
    $markers.FileLuaLog = Get-MarkerLine $Text '\[lua\] desktop\.lua'
    $markers.DesktopReady = Get-MarkerLine $Text '\[perf\] desktop ready in'

    if ($markers.LuaInit) {
        $im = [regex]::Match($markers.LuaInit, 'wallpaper_fb=(\S+)\s+(\d+)x(\d+)')
        if ($im.Success) {
            $markers.InitWallpaperFb = $im.Groups[1].Value
            $markers.InitWallpaperW = [int]$im.Groups[2].Value
            $markers.InitWallpaperH = [int]$im.Groups[3].Value
        }
    }
    if ($markers.LuaSource) {
        $v = Get-FirstCapture $markers.LuaSource 'source=(file|embedded)'
        if ($v) { $markers.SourceValue = $v }
    }
    if ($markers.StyleLine) {
        $v = Get-FirstCapture $markers.StyleLine 'radius=(\d+)'
        if ($v) { $markers.StyleRadius = [int]$v }
        $markers.StyleTitleHex = Get-FirstCapture $markers.StyleLine 'title=0x([0-9A-Fa-f]{6})'
        $markers.StyleInkHex = Get-FirstCapture $markers.StyleLine 'ink=0x([0-9A-Fa-f]{6})'
        $markers.StylePaperHex = Get-FirstCapture $markers.StyleLine 'paper=0x([0-9A-Fa-f]{6})'
        $v = Get-FirstCapture $markers.StyleLine 'shadow=(\d+)'
        if ($v) { $markers.StyleShadow = [int]$v }
    }
    if ($markers.WallpaperLine) {
        $wm = [regex]::Match($markers.WallpaperLine,
            'fb=(\S+)\s+(\d+)x(\d+)\s+top=0x([0-9A-Fa-f]{6})\s+mid=0x([0-9A-Fa-f]{6})\s+bottom=0x([0-9A-Fa-f]{6})')
        if ($wm.Success) {
            $markers.WallFb = $wm.Groups[1].Value
            $markers.WallWidth = [int]$wm.Groups[2].Value
            $markers.WallHeight = [int]$wm.Groups[3].Value
            $markers.WallTopHex = $wm.Groups[4].Value
            $markers.WallMidHex = $wm.Groups[5].Value
            $markers.WallBottomHex = $wm.Groups[6].Value
        }
    }
    return $markers
}

function Get-MissingLuaMarkers {
    param($Markers)
    $missing = @()
    if (-not $Markers) {
        return @('serial text is empty')
    }
    if (-not $Markers.DesktopStart) { $missing += '[desktop] start: fb=' }
    if (-not $Markers.LuaInit) { $missing += '[desktop] lua: init ... wallpaper_fb=...' }
    if (-not $Markers.LuaSource) { $missing += '[desktop] lua: source=file|embedded' }
    if (-not $Markers.StyleLine) { $missing += '[desktop] lua: style ...' }
    if (-not $Markers.WallpaperLine) { $missing += '[desktop] lua: wallpaper: script-painted ...' }
    return $missing
}

function Wait-SerialMarkers {
    param([string]$Path, $Proc, [int]$TimeoutSeconds)
    $startTime = Get-Date
    $deadline = $startTime.AddSeconds($TimeoutSeconds)
    $text = ""
    $lastReport = 0.0
    $missing = @('serial log not produced')
    while ((Get-Date) -lt $deadline) {
        $snapshot = Read-TextFileShared -Path $Path -Attempts 1
        if ($null -ne $snapshot) { $text = $snapshot }
        $markers = Get-LuaMarkers $text
        $missing = Get-MissingLuaMarkers $markers
        if ($missing.Count -eq 0) {
            return @{ Ok = $true; Text = $text; Missing = @(); Fatal = $null; Elapsed = ((Get-Date) - $startTime).TotalSeconds }
        }
        if ($markers.LuaError) {
            return @{ Ok = $false; Text = $text; Missing = $missing; Fatal = ("Lua config error: " + $markers.LuaError); Elapsed = ((Get-Date) - $startTime).TotalSeconds }
        }
        $fatal = Get-MarkerLine $text 'Kernel panic|Kernel crash|General protection|SMASHING|\[desktop\] FATAL|Panic:'
        if ($fatal) {
            return @{ Ok = $false; Text = $text; Missing = $missing; Fatal = ("crash/fatal line: " + $fatal); Elapsed = ((Get-Date) - $startTime).TotalSeconds }
        }
        if ($Proc) {
            try {
                if ($Proc.HasExited) {
                    return @{ Ok = $false; Text = $text; Missing = $missing; Fatal = ("QEMU exited by itself (exit code {0}) before the desktop Lua markers" -f $Proc.ExitCode); Elapsed = ((Get-Date) - $startTime).TotalSeconds }
                }
            } catch { }
        }
        $elapsed = ((Get-Date) - $startTime).TotalSeconds
        if (($elapsed - $lastReport) -ge 5.0) {
            $lastReport = $elapsed
            Write-Host ("LUA DESKTOP SMOKE:   ... waited {0:N0}s; missing: {1}" -f $elapsed, ($missing -join ', '))
        }
        Start-Sleep -Milliseconds 500
    }
    return @{ Ok = $false; Text = $text; Missing = $missing; Fatal = $null; Timeout = $true; Elapsed = ((Get-Date) - $startTime).TotalSeconds }
}

function Wait-SerialPattern {
    param([string]$Path, $Proc, [string]$Pattern, [int]$TimeoutSeconds)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $snapshot = Read-TextFileShared -Path $Path -Attempts 1
        if ($snapshot -and [regex]::IsMatch($snapshot, $Pattern)) { return $true }
        if ($Proc) {
            try { if ($Proc.HasExited) { return $false } } catch { }
        }
        Start-Sleep -Milliseconds 400
    }
    return $false
}

# ---------------------------------------------------------------------------
# QEMU process management / monitor plumbing
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

# Kill the QEMU instance we started plus any QEMU process that appeared after we
# snapshotted the pre-run PIDs (handles launcher / PID mapping corner cases).
function Stop-QemuProcesses {
    param([int[]]$Before, $Proc)
    if ($Proc) {
        try {
            if (-not $Proc.HasExited) {
                $Proc.Kill()
                [void]$Proc.WaitForExit(3000)
            }
        } catch { }
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
    foreach ($qpid in $left) {
        try { & taskkill /F /PID $qpid 2>$null | Out-Null } catch { }
    }
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
        } catch {
            break
        }
        if ($read -gt 0) {
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            if ($sb.ToString().Contains($Token)) { break }
        } elseif ($read -eq 0) {
            break
        } else {
            Start-Sleep -Milliseconds 100
        }
    }
    return $sb.ToString()
}

function Send-MonitorCommand {
    param([System.Net.Sockets.NetworkStream]$Stream, [string]$Command, [int]$TimeoutMs = 15000)
    $payload = [System.Text.Encoding]::ASCII.GetBytes($Command + "`n")
    $Stream.Write($payload, 0, $payload.Length)
    $Stream.Flush()
    return (Read-MonitorUntil -Stream $Stream -Token '(qemu)' -TimeoutMs $TimeoutMs)
}

# Open one HMP monitor session, take a screendump PPM (retrying), then quit QEMU
# gracefully.  Returns an object with Ok/Error/Output/QuitSent.
function Invoke-MonitorSession {
    param([string]$HostName = '127.0.0.1', [int]$Port, [string]$ScreendumpPath, [int]$Retries = 3)
    $result = [pscustomobject]@{
        Ok       = $false
        Error    = ""
        Output   = ""
        QuitSent = $false
    }
    $client = $null
    try {
        # ---- connect (QEMU binds the monitor before the desktop is up) ----
        $lastConnectError = ""
        $connected = $false
        for ($attempt = 1; $attempt -le 5 -and -not $connected; $attempt++) {
            $candidate = $null
            try {
                $candidate = New-Object System.Net.Sockets.TcpClient
                $iar = $candidate.BeginConnect($HostName, $Port, $null, $null)
                if (-not $iar.AsyncWaitHandle.WaitOne(3000)) { throw "connect timed out" }
                $candidate.EndConnect($iar)
                $client = $candidate
                $connected = $true
            } catch {
                $lastConnectError = $_.Exception.Message
                if ($candidate) { try { $candidate.Close() } catch { } }
                Start-Sleep -Milliseconds 700
            }
        }
        if (-not $connected) {
            $result.Error = ("cannot connect to QEMU monitor on {0}:{1} ({2})" -f $HostName, $Port, $lastConnectError)
            return $result
        }
        $stream = $client.GetStream()

        # ---- wait for the "(qemu)" prompt ----
        $banner = Read-MonitorUntil -Stream $stream -Token '(qemu)' -TimeoutMs 12000
        if (-not $banner.Contains('(qemu)')) {
            $result.Error = ("no monitor prompt from QEMU (output: {0})" -f (Format-MonitorText $banner))
            return $result
        }

        # HMP splits arguments on whitespace; forward slashes are accepted by
        # QEMU on Windows and dodge any backslash-handling questions.
        $hmpPath = $ScreendumpPath.Replace('\', '/')
        $command = 'screendump ' + $hmpPath
        for ($attempt = 1; $attempt -le $Retries; $attempt++) {
            if (Test-Path -LiteralPath $ScreendumpPath) {
                Remove-Item -LiteralPath $ScreendumpPath -Force -ErrorAction SilentlyContinue
            }
            $out = Send-MonitorCommand -Stream $stream -Command $command -TimeoutMs 20000
            $result.Output = $out
            $fileOk = $false
            for ($wait = 0; $wait -lt 10; $wait++) {
                if (Test-Path -LiteralPath $ScreendumpPath) {
                    $len = 0
                    try { $len = (Get-Item -LiteralPath $ScreendumpPath).Length } catch { }
                    if ($len -gt 100) { $fileOk = $true; break }
                }
                Start-Sleep -Milliseconds 200
            }
            if ($fileOk) {
                $result.Ok = $true
                break
            }
            $errMatch = [regex]::Match($out, '(?im)(error[^\r\n]*|could not[^\r\n]*|invalid[^\r\n]*)')
            if ($errMatch.Success) {
                $result.Error = ("screendump rejected: {0}" -f $errMatch.Value.Trim())
            } else {
                $result.Error = ("screendump produced no/empty file (attempt {0}/{1})" -f $attempt, $Retries)
            }
            Start-Sleep -Milliseconds 800
        }

        # ---- graceful shutdown (the kill sweep still runs afterwards) ----
        try {
            Send-MonitorCommand -Stream $stream -Command 'quit' -TimeoutMs 3000 | Out-Null
            $result.QuitSent = $true
        } catch { }
    } catch {
        $result.Error = $_.Exception.Message
    } finally {
        if ($client) { try { $client.Close() } catch { } }
    }
    return $result
}

# HMP screendump cannot take a filename with spaces; when the requested output
# path has spaces use a whitespace-free staging file and copy it afterwards.
function Get-ScreendumpStagingPath {
    param([string]$RequestedPath)
    if ($RequestedPath -and ($RequestedPath -notmatch '\s')) { return $RequestedPath }
    $candidates = @()
    try {
        $tmp = [System.IO.Path]::GetTempPath()
        if ($tmp) { $candidates += [System.IO.Path]::Combine($tmp, ("lua_desktop_shot_{0}.ppm" -f $PID)) }
    } catch { }
    if ($env:PUBLIC) { $candidates += [System.IO.Path]::Combine($env:PUBLIC, ("lua_desktop_shot_{0}.ppm" -f $PID)) }
    if ($env:USERPROFILE) { $candidates += [System.IO.Path]::Combine($env:USERPROFILE, ("lua_desktop_shot_{0}.ppm" -f $PID)) }
    foreach ($candidate in $candidates) {
        if ($candidate -and ($candidate -notmatch '\s')) { return $candidate }
    }
    return $null
}

# ============================================================================
# END OF SHARED HELPERS
# ============================================================================

# ----------------------------------------------------------------------------
# Offline self-tests (no QEMU): used to validate the script on a dev machine.
# ----------------------------------------------------------------------------
if ($SelfTestPpm) {
    try {
        $ppm = Read-PpmFile -Path $SelfTestPpm
        Write-Host ("PPM OK: {0}" -f $SelfTestPpm)
        Write-Host ("  size   : {0}x{1} maxval={2}" -f $ppm.Width, $ppm.Height, $ppm.MaxVal)
        $points = @(
            @{ Name = "wallpaper-top   "; X = 2; Y = 20 },
            @{ Name = "wallpaper-bottom"; X = [int]($ppm.Width / 2); Y = [int]($ppm.Height - 80) },
            @{ Name = "window-title    "; X = [int]($ppm.Width / 2); Y = ([int](($ppm.Height - 400) / 2) + 8) }
        )
        foreach ($pt in $points) {
            $sample = Get-PpmRegionAverage -Ppm $ppm -X $pt.X -Y $pt.Y -Radius 2
            $center = Get-PpmPixelRgb -Ppm $ppm -X $pt.X -Y $pt.Y
            Write-Host ("  {0} @({1},{2}): center {3} avg5x5 {4}" -f `
                $pt.Name, $pt.X, $pt.Y, (Format-Rgb $center), (Format-Rgb $sample))
        }
        exit 0
    } catch {
        Write-Host ("PPM FAIL: {0}" -f $_.Exception.Message)
        exit 1
    }
}

if ($SelfTestSerial) {
    $text = Read-TextFileShared -Path $SelfTestSerial -Attempts 3
    if ($null -eq $text) {
        Write-Host ("SERIAL FAIL: cannot read {0}" -f $SelfTestSerial)
        exit 1
    }
    $markers = Get-LuaMarkers $text
    Write-Host ("SERIAL: {0}" -f $SelfTestSerial)
    Write-Host ("  desktop start : {0}" -f $markers.DesktopStart)
    Write-Host ("  lua init      : {0}" -f $markers.LuaInit)
    Write-Host ("  lua source    : {0}" -f $markers.LuaSource)
    Write-Host ("  style line    : {0}" -f $markers.StyleLine)
    Write-Host ("  wallpaper line: {0}" -f $markers.WallpaperLine)
    Write-Host ("  legacy line   : {0}" -f $markers.LegacyWallpaperLine)
    Write-Host ("  desktop ready : {0}" -f $markers.DesktopReady)
    Write-Host ("  lua error     : {0}" -f $markers.LuaError)
    Write-Host ("  parsed style  : radius={0} title=0x{1} ink=0x{2} paper=0x{3} shadow={4}" -f `
        $markers.StyleRadius, $markers.StyleTitleHex, $markers.StyleInkHex, $markers.StylePaperHex, $markers.StyleShadow)
    Write-Host ("  init wallpaper: fb={0} {1}x{2}" -f `
        $markers.InitWallpaperFb, $markers.InitWallpaperW, $markers.InitWallpaperH)
    Write-Host ("  painted wall  : fb={0} {1}x{2} top=0x{3} mid=0x{4} bottom=0x{5}" -f `
        $markers.WallFb, $markers.WallWidth, $markers.WallHeight, `
        $markers.WallTopHex, $markers.WallMidHex, $markers.WallBottomHex)
    $missing = Get-MissingLuaMarkers $markers
    if ($missing.Count -gt 0) {
        Write-Host ("SERIAL FAIL: missing markers: {0}" -f ($missing -join ', '))
        exit 1
    }
    Write-Host "SERIAL OK: all required Lua markers present"
    exit 0
}

# ----------------------------------------------------------------------------
# Environment checks
# ----------------------------------------------------------------------------
Write-Host "LUA DESKTOP SMOKE: ===================================================="
Write-Host "LUA DESKTOP SMOKE: desktop embedded-Lua smoke gate (serial + screendump)"
Write-Host "LUA DESKTOP SMOKE: root      = $root"
Write-Host "LUA DESKTOP SMOKE: qemu      = $qemu"
Write-Host "LUA DESKTOP SMOKE: iso       = $isoPath"
Write-Host "LUA DESKTOP SMOKE: serial    = $serialPath"
Write-Host "LUA DESKTOP SMOKE: screenshot= $shotPath"

if (-not (Test-Path -LiteralPath $qemu)) {
    Write-Host "ERROR: QEMU not found: $qemu"
    Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
    exit 2
}
if (-not (Test-Path -LiteralPath $isoPath)) {
    Write-Host "ERROR: ISO not found, build it first: $isoPath"
    Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
    exit 2
}
if ($UseDisk -and $NoDisk) {
    Write-Host "ERROR: -UseDisk and -NoDisk are mutually exclusive"
    Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
    exit 2
}

$attachDisk = $false
if ($UseDisk) {
    if (-not (Test-Path -LiteralPath $diskPath)) {
        Write-Host "ERROR: -UseDisk given but disk image not found: $diskPath"
        Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
        exit 2
    }
    $attachDisk = $true
} elseif (-not $NoDisk) {
    if (Test-Path -LiteralPath $diskPath) {
        $attachDisk = $true
    } else {
        Write-Host "WARN: disk.img not found: $diskPath"
        Write-Host "WARN: booting ISO only; the desktop needs /mp/desktop.elf, /mp/hw2.elf,"
        Write-Host "WARN: /mp/desktop.lua, ... so it will probably never reach the desktop."
        Write-Host "WARN: rebuild the user disk image or pass -UseDisk."
    }
}
if ($attachDisk) {
    Write-Host "LUA DESKTOP SMOKE: disk      = $diskPath"
} else {
    Write-Host "LUA DESKTOP SMOKE: disk      = (not attached)"
}

# Validate user supplied expected colors early (clear environment error).
$userWallTop = $null
$userWallBottom = $null
$userTitle = $null
if ($ExpectWallpaperTop) {
    $userWallTop = Convert-UserColor $ExpectWallpaperTop
    if ($null -eq $userWallTop) {
        Write-Host ("ERROR: invalid -ExpectWallpaperTop '{0}' (use 0xRRGGBB)" -f $ExpectWallpaperTop)
        Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
        exit 2
    }
}
if ($ExpectWallpaperBottom) {
    $userWallBottom = Convert-UserColor $ExpectWallpaperBottom
    if ($null -eq $userWallBottom) {
        Write-Host ("ERROR: invalid -ExpectWallpaperBottom '{0}' (use 0xRRGGBB)" -f $ExpectWallpaperBottom)
        Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
        exit 2
    }
}
if ($ExpectTitleColor) {
    $userTitle = Convert-UserColor $ExpectTitleColor
    if ($null -eq $userTitle) {
        Write-Host ("ERROR: invalid -ExpectTitleColor '{0}' (use 0xRRGGBB)" -f $ExpectTitleColor)
        Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
        exit 2
    }
}
if ($RequireSource -and ($RequireSource -ne 'file') -and ($RequireSource -ne 'embedded')) {
    Write-Host "ERROR: -RequireSource must be 'file' or 'embedded'"
    Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
    exit 2
}

# Output directories.
$serialDir = Split-Path -Parent $serialPath
if ($serialDir -and -not (Test-Path -LiteralPath $serialDir)) {
    New-Item -ItemType Directory -Force -Path $serialDir | Out-Null
}
$shotDir = Split-Path -Parent $shotPath
if ($shotDir -and -not (Test-Path -LiteralPath $shotDir)) {
    New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
}

# Pre-run process snapshot: cleanup only touches our own QEMU.
$qemuBefore = @(Get-QemuProcessIds)
if ($qemuBefore.Count -gt 0) {
    Write-Host ("WARN: {0} QEMU process(es) already running (PID: {1}); they may hold" -f `
        $qemuBefore.Count, ($qemuBefore -join ', '))
    Write-Host "WARN: disk.img/serial.log locks. Stop them for a clean run."
}

Remove-Item -LiteralPath $serialPath, $shotPath -Force -ErrorAction SilentlyContinue
$stagingPath = Get-ScreendumpStagingPath -RequestedPath $shotPath
if (-not $stagingPath) {
    Write-Host "ERROR: cannot find a whitespace-free path for the HMP screendump"
    Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
    exit 2
}
if ($stagingPath -ne $shotPath) {
    Remove-Item -LiteralPath $stagingPath -Force -ErrorAction SilentlyContinue
    Write-Host "LUA DESKTOP SMOKE: screendump staging file = $stagingPath"
}

# ----------------------------------------------------------------------------
# Boot QEMU headless: serial to a file, HMP monitor on a private telnet port.
# ----------------------------------------------------------------------------
$monitorPort = if ($MonitorPort -gt 0) { [int]$MonitorPort } else { [int](Get-FreeTcpPort) }
Write-Host "LUA DESKTOP SMOKE: monitor   = telnet:127.0.0.1:$monitorPort"

$qemuArgs = @(
    '-M', 'q35',
    '-cpu', 'max',
    '-m', '2G',
    '-smp', '4',
    '-cdrom', $isoPath,
    '-serial', ("file:{0}" -f $serialPath),
    '-monitor', ("telnet:127.0.0.1:{0},server,nowait" -f $monitorPort),
    '-display', 'none',
    '-vga', 'std',
    '-no-reboot',
    '-no-shutdown'
)
if ($attachDisk) {
    $qemuArgs += @(
        '-drive', ("file={0},if=none,id=drive0,format=raw" -f $diskPath),
        '-device', 'ide-hd,drive=drive0,bus=ide.0'
    )
}
$quotedArgs = @($qemuArgs | ForEach-Object { Quote-ProcessArg $_ })
Write-Host ("LUA DESKTOP SMOKE: command   = {0} {1}" -f $qemu, ($quotedArgs -join ' '))

$qmProc = $null
$startTime = Get-Date
$envError = $null
$waitResult = $null
$monitorResult = $null

try {
    try {
        $qmProc = Start-Process -FilePath $qemu -ArgumentList $quotedArgs -PassThru -WindowStyle Hidden
    } catch {
        $envError = ("failed to start QEMU: {0}" -f $_.Exception.Message)
    }
    if (-not $envError) {
        if (-not $qmProc) {
            $envError = "Start-Process returned no QEMU process object"
        } else {
            Write-Host ("LUA DESKTOP SMOKE: QEMU started, pid={0}; waiting up to {1}s for desktop Lua markers..." -f $qmProc.Id, $WaitSeconds)
            Start-Sleep -Milliseconds 1200
            if ($qmProc.HasExited) {
                $envError = ("QEMU exited immediately (exit code {0}); is another QEMU holding disk.img/serial.log, or is the ISO/QEMU path wrong?" -f $qmProc.ExitCode)
                Write-Host ("ERROR: " + $envError)
            } else {
                $waitResult = Wait-SerialMarkers -Path $serialPath -Proc $qmProc -TimeoutSeconds $WaitSeconds
                if ($waitResult.Ok) {
                    Write-Host ("LUA DESKTOP SMOKE: desktop Lua markers seen after {0:N1}s" -f $waitResult.Elapsed)
                    $readySeen = Wait-SerialPattern -Path $serialPath -Proc $qmProc `
                        -Pattern '\[perf\] desktop ready in' -TimeoutSeconds $DesktopReadyWaitSeconds
                    if ($readySeen) {
                        Write-Host "LUA DESKTOP SMOKE: desktop first frame ready ([perf] desktop ready in ...)"
                    } else {
                        Write-Host "LUA DESKTOP SMOKE: WARN: no [perf] desktop ready marker; screenshot anyway"
                    }
                } else {
                    if ($waitResult.Fatal) {
                        Write-Host ("LUA DESKTOP SMOKE: aborting wait: {0}" -f $waitResult.Fatal)
                    } else {
                        Write-Host ("LUA DESKTOP SMOKE: timed out after {0:N0}s; missing markers: {1}" -f `
                            $WaitSeconds, ($waitResult.Missing -join ', '))
                    }
                }

                # Settle: minimum total runtime + extra delay after readiness.
                $elapsed = ((Get-Date) - $startTime).TotalSeconds
                $extra = [double]$ScreenshotDelay
                $remain = [double]$MinSelftestWait - $elapsed
                if ($remain -gt $extra) { $extra = $remain }
                if (-not $waitResult.Ok) {
                    # keep diagnostic latency short when the boot already failed
                    $extra = [math]::Min($extra, 2.0)
                }
                if ($extra -gt 0) {
                    Write-Host ("LUA DESKTOP SMOKE: settling {0:N1}s before screendump" -f $extra)
                    Start-Sleep -Milliseconds ([int]([math]::Ceiling($extra * 1000.0)))
                }

                # Screendump even on a failed boot: the image is diagnostic evidence.
                try {
                    if (-not $qmProc.HasExited) {
                        $monitorResult = Invoke-MonitorSession -HostName '127.0.0.1' -Port $monitorPort `
                            -ScreendumpPath $stagingPath -Retries 3
                        if ($monitorResult.Ok) {
                            Write-Host "LUA DESKTOP SMOKE: screendump captured"
                        } else {
                            Write-Host ("LUA DESKTOP SMOKE: screendump FAILED: {0}" -f $monitorResult.Error)
                        }
                    }
                } catch {
                    Write-Host ("LUA DESKTOP SMOKE: screendump exception: {0}" -f $_.Exception.Message)
                }
            }
        }
    }
} finally {
    # Robust kill sweep: never leave a QEMU behind, even on early errors.
    $clean = Stop-QemuProcesses -Before $qemuBefore -Proc $qmProc
    if (-not $clean) {
        Write-Host "WARN: a QEMU process may still be running after the kill sweep"
    }
    if ($monitorResult) {
        if ($monitorResult.QuitSent) {
            Write-Host "LUA DESKTOP SMOKE: QEMU stopped (monitor quit + kill sweep)"
        } else {
            Write-Host "LUA DESKTOP SMOKE: QEMU stopped (kill sweep)"
        }
    } else {
        Write-Host "LUA DESKTOP SMOKE: QEMU stopped (kill sweep)"
    }
    if ($stagingPath -and ($stagingPath -ne $shotPath)) {
        if (Test-Path -LiteralPath $stagingPath) {
            try {
                Copy-Item -LiteralPath $stagingPath -Destination $shotPath -Force
                Write-Host ("LUA DESKTOP SMOKE: screenshot copied to {0}" -f $shotPath)
            } catch {
                Write-Host ("WARN: could not copy screenshot to {0}: {1}" -f $shotPath, $_.Exception.Message)
            }
            Remove-Item -LiteralPath $stagingPath -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($envError) {
    Write-Host ("LUA DESKTOP SMOKE: ERROR: {0}" -f $envError)
    $tailText = Read-TextFileShared -Path $serialPath -Attempts 2
    if ($tailText) {
        Write-Host "---- serial log tail (last 40 lines) ----"
        foreach ($line in (Get-LastLines $tailText 40)) { Write-Host ("  " + $line) }
    }
    Write-Host "LUA DESKTOP SMOKE: FAIL (environment)"
    exit 2
}

# ----------------------------------------------------------------------------
# Final serial read (QEMU is gone, so this is a complete snapshot)
# ----------------------------------------------------------------------------
$serialText = Read-TextFileShared -Path $serialPath -Attempts 10
if ($null -eq $serialText) {
    if ($waitResult -and $waitResult.Text) {
        $serialText = $waitResult.Text
        Write-Host "WARN: serial log became unreadable; using the last in-flight snapshot"
    } else {
        $serialText = ""
    }
}
$markers = Get-LuaMarkers $serialText

# Resolve what the framebuffer should contain: explicit args > serial markers.
$serialTitle = $null
if ($markers.StyleTitleHex) { $serialTitle = Convert-Hex6ToRgb $markers.StyleTitleHex }

# NOTE: the wallpaper is drawn by the Lua script; the C++ glue only reports the
# pixels it read BACK from the script-painted framebuffer.  Those readback
# values are evidence, not a color "configuration", so they are displayed but
# never used to synthesise expected pixel colors.  Only explicit -Expect*
# arguments drive the wallpaper pixel assertions.
$wallReadbackTop = $null
$wallReadbackMid = $null
$wallReadbackBottom = $null
if ($markers.WallTopHex) { $wallReadbackTop = Convert-Hex6ToRgb $markers.WallTopHex }
if ($markers.WallMidHex) { $wallReadbackMid = Convert-Hex6ToRgb $markers.WallMidHex }
if ($markers.WallBottomHex) { $wallReadbackBottom = Convert-Hex6ToRgb $markers.WallBottomHex }

$effTitle = if ($userTitle) { $userTitle } else { $serialTitle }
$effTitleSource = if ($userTitle) { "-ExpectTitleColor" } else { "serial style marker" }

Write-Host "---- resolved theme configuration ----"
Write-Host ("  window title   : {0}   (source: {1})" -f (Format-Rgb $effTitle), $effTitleSource)
if ($null -ne $markers.StyleRadius) {
    Write-Host ("  window radius  : {0}   (serial style marker)" -f $markers.StyleRadius)
}
if ($null -ne $markers.StyleShadow) {
    Write-Host ("  shadow alpha   : {0}   (serial style marker)" -f $markers.StyleShadow)
}
Write-Host ("  wallpaper      : painted by Lua script into fb={0} ({1}x{2})" -f `
    $markers.WallFb, $markers.WallWidth, $markers.WallHeight)
Write-Host ("  readback (glue): top {0}  mid {1}  bottom {2}" -f `
    (Format-Rgb $wallReadbackTop), (Format-Rgb $wallReadbackMid), (Format-Rgb $wallReadbackBottom))
Write-Host ("  wallpaper expect: top {0}  bottom {1}" -f `
    $(if ($userWallTop) { (Format-Rgb $userWallTop) + " (-ExpectWallpaperTop)" } else { "<not given: sanity checks only>" }), `
    $(if ($userWallBottom) { (Format-Rgb $userWallBottom) + " (-ExpectWallpaperBottom)" } else { "<not given: sanity checks only>" }))
if ($markers.LegacyWallpaperLine) {
    Write-Host ("WARN: old 'wallpaper type=' marker present (stale desktop.elf?): {0}" -f $markers.LegacyWallpaperLine)
}

# ----------------------------------------------------------------------------
# Report function
# ----------------------------------------------------------------------------
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

# ----------------------------------------------------------------------------
# Assertions: serial markers
# ----------------------------------------------------------------------------
Report "(a) desktop process started ([desktop] start: fb=...)" `
    ($null -ne $markers.DesktopStart) $markers.DesktopStart
$initOk = (($null -ne $markers.LuaInit) -and
           ($null -ne $markers.InitWallpaperFb) -and
           ($null -ne $markers.InitWallpaperW) -and ($markers.InitWallpaperW -gt 0) -and
           ($null -ne $markers.InitWallpaperH) -and ($markers.InitWallpaperH -gt 0))
$initDetail = $markers.LuaInit
if (-not $initOk -and $null -ne $markers.LuaInit) {
    $initDetail = ("marker present but no wallpaper_fb=<addr> <w>x<h> handoff: {0}" -f $markers.LuaInit)
}
Report "(b) embedded Lua runtime initialised with wallpaper fb ([desktop] lua: init ... wallpaper_fb=...)" `
    $initOk $initDetail

$sourceOk = ($null -ne $markers.LuaSource)
$sourceDetail = $markers.LuaSource
if (-not ($null -ne $markers.LuaSource)) {
    $sourceDetail = "marker missing: [desktop] lua: source=file|embedded"
} elseif ($RequireSource -and ($markers.SourceValue -ne $RequireSource)) {
    $sourceOk = $false
    $sourceDetail = ("source is '{0}' but -RequireSource {1} was requested" -f $markers.SourceValue, $RequireSource)
}
Report "(c) Lua theme script executed (source=file|embedded)" $sourceOk $sourceDetail
if ($markers.SourceValue -eq 'embedded') {
    Write-Host "NOTE: source=embedded -> /mp/desktop.lua was not found on the user disk;"
    Write-Host "NOTE: the compiled-in fallback theme was applied instead. Rebuild the disk"
    Write-Host "NOTE: image (programs/Makefile copies desktop/desktop.lua) for source=file."
}
if ($markers.FileLuaLog) {
    Write-Host ("INFO: " + $markers.FileLuaLog)
}

Report "(d) window style values reported ([desktop] lua: style ...)" `
    ($null -ne $markers.StyleLine) $markers.StyleLine
$paintedOk = ($null -ne $markers.WallpaperLine)
$fbMatch = $false
if ($null -ne $markers.InitWallpaperFb -and $null -ne $markers.WallFb) {
    $fbMatch = ([string]$markers.InitWallpaperFb).ToLowerInvariant() -eq ([string]$markers.WallFb).ToLowerInvariant()
    if (-not $fbMatch) { $paintedOk = $false }
}
Report "(e) wallpaper painted by Lua into the handed framebuffer ([desktop] lua: wallpaper: script-painted ...)" `
    $paintedOk $markers.WallpaperLine
if ($null -ne $markers.InitWallpaperFb -and $null -ne $markers.WallFb) {
    Write-Host ("INFO: wallpaper fb cross-check: init={0} painted={1} match={2}" -f `
        $markers.InitWallpaperFb, $markers.WallFb, $fbMatch)
}
Report "(f) no desktop Lua config errors ([desktop] lua: error)" `
    ($null -eq $markers.LuaError) $markers.LuaError

$crashLine = Get-MarkerLine $serialText 'Exception|Kernel panic|Kernel crash|General protection|SMASHING|\[desktop\] FATAL'
Report "(g) no crash/panic keywords in serial log" `
    ($null -eq $crashLine) $crashLine

if ($markers.DesktopReady) {
    Write-Host ("INFO: {0}" -f $markers.DesktopReady)
}

# ----------------------------------------------------------------------------
# Assertions: screenshot parsed
# ----------------------------------------------------------------------------
$ppm = $null
$ppmError = ""
if (-not (Test-Path -LiteralPath $shotPath)) {
    $ppmError = ("screenshot file missing: {0}" -f $shotPath)
} else {
    try {
        $ppm = Read-PpmFile -Path $shotPath
    } catch {
        $ppmError = ("cannot parse screenshot: {0}" -f $_.Exception.Message)
    }
}
$shotDetail = $ppmError
if ($ppm) {
    $shotDetail = ("{0} ({1}x{2} PPM P6, {3:N0} bytes) is kept as evidence" -f `
        $shotPath, $ppm.Width, $ppm.Height, (Get-Item -LiteralPath $shotPath).Length)
}
Report "(h) screenshot captured and parsed" ($null -ne $ppm) $shotDetail

# ----------------------------------------------------------------------------
# Assertions: pixel sampling of wallpaper / window chrome
# ----------------------------------------------------------------------------
if ($ppm) {
    $topX = 2
    $topY = 20
    $bottomX = [int]($ppm.Width / 2)
    $bottomY = [int]($ppm.Height - 80)
    if ($bottomY -lt 1) { $bottomY = [int]($ppm.Height / 2) }
    $bodyTop = [int](($ppm.Height - 400) / 2)
    if ($bodyTop -lt 2) { $bodyTop = 2 }
    $titleY = $bodyTop + 8
    if ($titleY -ge $ppm.Height) { $titleY = [int]($ppm.Height / 2) }
    $titleXs = @()
    foreach ($offset in @(-80, 0, 80)) {
        $x = [int]($ppm.Width / 2) + $offset
        if ($x -ge 0 -and $x -lt $ppm.Width) { $titleXs += $x }
    }

    $wallTopSample = Get-PpmRegionAverage -Ppm $ppm -X $topX -Y $topY -Radius 2
    $wallBottomSample = Get-PpmRegionAverage -Ppm $ppm -X $bottomX -Y $bottomY -Radius 2
    $titleSamples = @()
    foreach ($x in $titleXs) {
        $titleSamples += (Get-PpmRegionAverage -Ppm $ppm -X $x -Y $titleY -Radius 2)
    }

    Write-Host "---- framebuffer samples ----"
    Write-Host ("  screenshot     : {0} ({1}x{2})" -f $shotPath, $ppm.Width, $ppm.Height)
    Write-Host ("  wallpaper top  : @({0},{1}) avg5x5 {2}" -f $topX, $topY, (Format-Rgb $wallTopSample))
    Write-Host ("  wallpaper bottom: @({0},{1}) avg5x5 {2}" -f $bottomX, $bottomY, (Format-Rgb $wallBottomSample))
    for ($i = 0; $i -lt $titleSamples.Count; $i++) {
        Write-Host ("  title bar      : @({0},{1}) avg5x5 {2}" -f $titleXs[$i], $titleY, (Format-Rgb $titleSamples[$i]))
    }
    if ($null -ne $wallTopSample -and $null -ne $wallBottomSample) {
        Write-Host ("  top/bottom diff: sum|dRGB|={0}" -f (Get-RgbDiffSum $wallTopSample $wallBottomSample))
    }

    # ---- window title bar reference color from the actual frame ----------
    $titleAvg = $null
    if ($titleSamples.Count -gt 0) {
        $sumR = 0; $sumG = 0; $sumB = 0
        foreach ($sample in $titleSamples) {
            $sumR += [int]$sample.R; $sumG += [int]$sample.G; $sumB += [int]$sample.B
        }
        $titleAvg = New-Rgb ([int][math]::Floor($sumR / $titleSamples.Count + 0.5)) `
                            ([int][math]::Floor($sumG / $titleSamples.Count + 0.5)) `
                            ([int][math]::Floor($sumB / $titleSamples.Count + 0.5))
    }

    # ---- wallpaper pixel assertions -------------------------------------
    # The wallpaper is ARBITRARY Lua output (any shape/gradient/flat fill), so
    # only explicit -ExpectWallpaperTop/-ExpectWallpaperBottom colors are
    # compared.  Without them the checks below are sanity-only (script actually
    # painted pixels; wall visually differs from the window chrome) and the
    # exact-color comparisons are reported as SKIP.  The glue readback samples
    # (top/mid/bottom) are evidence only and never turned into expectations.
    $wallTopIsBlack = $false
    $wallBottomIsBlack = $false
    $wallTopBottomDiff = 0
    if ($null -ne $wallTopSample) {
        $wallTopIsBlack = (($wallTopSample.R -eq 0) -and ($wallTopSample.G -eq 0) -and ($wallTopSample.B -eq 0))
    }
    if ($null -ne $wallBottomSample) {
        $wallBottomIsBlack = (($wallBottomSample.R -eq 0) -and ($wallBottomSample.G -eq 0) -and ($wallBottomSample.B -eq 0))
    }
    if ($null -ne $wallTopSample -and $null -ne $wallBottomSample) {
        $wallTopBottomDiff = Get-RgbDiffSum $wallTopSample $wallBottomSample
    }
    Write-Host ("  wallpaper sanity: top-not-black={0} bottom-not-black={1} top-vs-bottom sum|dRGB|={2}" -f `
        (-not $wallTopIsBlack), (-not $wallBottomIsBlack), $wallTopBottomDiff)

    if ($null -ne $userWallTop) {
        Report "(i) wallpaper top pixel matches -ExpectWallpaperTop" `
            (Test-RgbNear $wallTopSample $userWallTop $Tolerance) `
            ("sampled {0} at ({1},{2}) vs expected {3} ({4}; sample row is inside the wall, tolerance +/-{5}/channel)" -f `
                (Format-Rgb $wallTopSample), $topX, $topY, (Format-Rgb $userWallTop),
                (Format-RgbDelta $wallTopSample $userWallTop), $Tolerance)
    } else {
        Report "(i) wallpaper painted by script (not left pure black)" `
            (-not ($wallTopIsBlack -and $wallBottomIsBlack)) `
            ("sampled top {0}, bottom {1}" -f (Format-Rgb $wallTopSample), (Format-Rgb $wallBottomSample))
        Write-Host "SKIP: (i) no -ExpectWallpaperTop -> exact top color not asserted"
    }

    if ($null -ne $userWallBottom) {
        Report "(j) wallpaper bottom pixel matches -ExpectWallpaperBottom" `
            (Test-RgbNear $wallBottomSample $userWallBottom $Tolerance) `
            ("sampled {0} at ({1},{2}) vs expected {3} ({4}; sample row is inside the wall, tolerance +/-{5}/channel)" -f `
                (Format-Rgb $wallBottomSample), $bottomX, $bottomY, (Format-Rgb $userWallBottom),
                (Format-RgbDelta $wallBottomSample $userWallBottom), $Tolerance)
    } else {
        $chromeRef = $serialTitle
        if ($null -ne $titleAvg) { $chromeRef = $titleAvg }
        if ($null -ne $chromeRef -and $null -ne $wallTopSample -and $null -ne $wallBottomSample) {
            $diffTop = Get-RgbDiffSum $wallTopSample $chromeRef
            $diffBottom = Get-RgbDiffSum $wallBottomSample $chromeRef
            $distinct = (($diffTop -ge 24) -or ($diffBottom -ge 24))
            Report "(j) wallpaper distinguishable from window chrome" $distinct `
                ("window ref {0}; wallpaper top {1} sum|dRGB|={2}, bottom {3} sum|dRGB|={4} (threshold 24)" -f `
                    (Format-Rgb $chromeRef), (Format-Rgb $wallTopSample), $diffTop,
                    (Format-Rgb $wallBottomSample), $diffBottom)
        } else {
            Write-Host "SKIP: (j) no window chrome reference for the contrast check"
        }
        Write-Host "SKIP: (j) no -ExpectWallpaperBottom -> exact bottom color not asserted"
    }

    # ---- window title bar assertion --------------------------------------
    if ($null -ne $effTitle -and $titleSamples.Count -gt 0) {
        $titleOk = $true
        for ($i = 0; $i -lt $titleSamples.Count; $i++) {
            if (-not (Test-RgbNear $titleSamples[$i] $effTitle $Tolerance)) { $titleOk = $false }
        }
        $titleDetail = ("sampled {0} @({1},{2}) vs expected {3} ({4}; tolerance +/-{5}/channel)" -f `
            (Format-Rgb $titleAvg), [int]($ppm.Width / 2), $titleY, (Format-Rgb $effTitle),
            $effTitleSource, $Tolerance)
        Report "(k) window title bar pixel matches configured style title" $titleOk $titleDetail
        if (-not $titleOk) {
            Write-Host ("      note: style line was: {0}" -f $markers.StyleLine)
        }
    } elseif ($titleSamples.Count -gt 0) {
        $diffTop = Get-RgbDiffSum $titleAvg $wallTopSample
        $diffBottom = Get-RgbDiffSum $titleAvg $wallBottomSample
        $distinct = if ($diffTop -ge 24 -or $diffBottom -ge 24) { $true } else { $false }
        Report "(k) window area distinguishable from wallpaper" $distinct `
            ("title avg {0}; vs wallpaper top sum|dRGB|={1}, vs bottom={2} (threshold 24)" -f `
                (Format-Rgb $titleAvg), $diffTop, $diffBottom)
    } else {
        Write-Host "SKIP: (k) no title-bar samples available"
    }
} else {
    Write-Host "SKIP: (i)(j)(k) pixel checks need a parsed screenshot"
}

# ----------------------------------------------------------------------------
# Evidence + verdict
# ----------------------------------------------------------------------------
Write-Host "---- serial markers (evidence) ----"
if ($markers.DesktopStart) { Write-Host ("  " + $markers.DesktopStart) } else { Write-Host "  <missing> [desktop] start: fb=..." }
if ($markers.LuaInit) { Write-Host ("  " + $markers.LuaInit) } else { Write-Host "  <missing> [desktop] lua: init" }
if ($markers.LuaSource) { Write-Host ("  " + $markers.LuaSource) } else { Write-Host "  <missing> [desktop] lua: source=" }
if ($markers.StyleLine) { Write-Host ("  " + $markers.StyleLine) } else { Write-Host "  <missing> [desktop] lua: style ..." }
if ($markers.WallpaperLine) { Write-Host ("  " + $markers.WallpaperLine) } else { Write-Host "  <missing> [desktop] lua: wallpaper: script-painted ..." }
if ($markers.LuaError) { Write-Host ("  " + $markers.LuaError) }
Write-Host ("  serial log: {0}" -f $serialPath)
Write-Host ("  screenshot: {0}" -f $shotPath)

if ($script:FailCount -gt 0) {
    if (-not $markers.DesktopStart) {
        Write-Host "HINT: no [desktop] markers in the serial log. The desktop needs the"
        Write-Host "HINT: user disk image (/mp/desktop.elf, /mp/libsky.so, /mp/hw2.elf,"
        Write-Host "HINT: /mp/desktop.lua). Rebuild the disk image or pass -UseDisk."
    }
    Write-Host "---- serial log tail (last 40 lines) ----"
    foreach ($line in (Get-LastLines $serialText 40)) { Write-Host ("  " + $line) }
    Write-Host ("LUA DESKTOP SMOKE: FAIL ({0} assertion(s) failed)" -f $script:FailCount)
    exit 1
}

Write-Host "LUA DESKTOP SMOKE: PASS"
exit 0
