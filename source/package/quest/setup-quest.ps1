# Doom VR for Quest and Steam Frame (standalone): install, uninstall, log
# collection and start, in one script, run on the PC with the headset plugged in
# by USB (a Quest) or reached over Wi-Fi (a Steam Frame running Lepton).
#
# The games come from this PC. Doom VR's PC version keeps everything it set up
# - the games you own, the free games and mods, a launcher for each - in its
# VR\game folder, and that folder is what goes to the headset: install the PC
# version first and set it up, and this Setup finds it (Steam, GOG,
# Documents\Doom VR, or a folder you drop on Setup.bat). Without a PC install
# it puts the same folder together itself, here, with the PC Setup's own game
# search and downloads, and keeps it for the next update.
#
# The app on the headset carries the engine; the folder carries your games.
# No game data ships with this download.
#
#   Setup.bat                   install (or update) on a Quest, by USB
#   Uninstall.bat               -Uninstall: copies your settings and saved
#                               games onto this PC, then removes the app
#   Collect logs.bat            -Collect: the headset's logs, in one zip on your desktop
#   Setup Steam Frame.bat       -SteamFrame: the same on a Steam Frame, over the home
#                               network, into the Frame's own library (the steam-frame kit)
#   Start on Steam Frame.bat    -Start -SteamFrame: starts it as the Frame's library would
#   Uninstall Steam Frame.bat   -Uninstall -SteamFrame;  Collect Steam Frame logs.bat  -Collect -SteamFrame
#
# A Quest talks to the PC through adb, Google's Android tool. This script
# uses one already on the PC (SideQuest's, or one on PATH), or downloads
# Google's own the first time. A Steam Frame is reached over SSH, paired once
# (on the Frame: Settings, Developer, Pair new host, then Accept).
#
# Safe to run again at any time: files already on the headset are not copied
# again, and your settings and saved games are never overwritten.
#
# For tools and hubs (the same switches in every Game Or Die port):
#   -Quiet              no questions, no pause, no Explorer window
#   -GamePath <folder>  the PC's Doom VR game folder (VR\game), its VR folder, or the
#                       DOOM + DOOM II folder it is in; several separated by ';'; no searching
#   -Json               print the result as JSON on stdout (nothing else)
#   -Detect             change nothing; report the game, the headset and the install
#   -Serial <id>        the Quest to use when several are connected
#   -SteamFrame         a Steam Frame instead of a Quest (every mode)
#   -Frame <name>       the Steam Frame's name or address (else the last one, else a search)
#   -RefreshHz <n>      the refresh rate SteamVR gives the game on the Frame (0: unchanged)
#   -NoDownload         when the folder is put together here: no free games or mods
#   -Mirror <url>       test builds: the mirrored mods from here (a file:/// folder)
# Exit codes: 0 done, 1 no games, 2 error, 3 no headset.
#
# Copyright (C) 2026 Game Or Die. All rights reserved. May be redistributed
# only as part of Doom VR, unchanged.

[CmdletBinding(PositionalBinding = $false)]
param(
    [switch] $Uninstall,
    [switch] $Collect,
    [switch] $Start,
    [switch] $Detect,
    [switch] $Quiet,
    [switch] $Json,
    [string[]] $GamePath,
    [switch] $NoDownload,
    [string] $Mirror = '',
    [string] $Serial,
    [switch] $SteamFrame,
    [string] $Frame = '',
    # 72 Hz until the game's own rate on the Frame is measured.
    [int] $RefreshHz = 72,
    # Tests: no questions; only the folders given; this adb; how long to wait.
    [switch] $NoPrompt,
    [switch] $NoSearch,
    [string] $Adb,
    [int] $WaitSeconds = 600,
    [string] $FrameKeys,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Dropped
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$QuestHere = (Resolve-Path -LiteralPath $PSScriptRoot).ProviderPath
if ($Quiet -or $Json) { $NoPrompt = $true }
if ($GamePath -and $GamePath[0]) { $NoSearch = $true; $Dropped = @($Dropped) + @($GamePath | ForEach-Object { $_ -split ';' } | Where-Object { $_ }) }

$Product = 'Doom VR'
$ProductFile = 'Doom VR'
$Version = '1.1'
$AppId = 'net.gameordie.doomvr'
$AppName = 'Doom VR'
$Activity = $AppId + '/.DoomVRActivity'
$Remote = '/sdcard/Android/data/' + $AppId + '/files'
$RemoteGame = $Remote + '/game'
$Apk = Join-Path $QuestHere 'Doom-VR.apk'
# The game folder put together on this PC when there is no PC install to take it from; kept.
$Stage = Join-Path $env:LOCALAPPDATA 'Game Or Die\Doom VR for Quest'
# What the game writes on the headset: made by the app itself (SetupReceiver), so the game can
# write them (a folder adb makes is adb's).
$WritableDirs = @('game', 'game/wads', 'game/mods', 'game/My WADs', 'game/logs', 'game/gametome', 'game/screenshots')
# The player's own, on the headset (relative to game/): kept on this PC by every Setup and Uninstall.
$SaveItems = @('doomvr.ini', 'saves', 'My WADs', 'doomvr-weapons.txt', 'doomvr-hands.txt')
$SaveRoot = Join-Path ([Environment]::GetFolderPath('MyDocuments')) ($ProductFile + '\Quest saves')
$IsFrame = [bool]$SteamFrame
# The Steam Frame (the steam-frame kit): our folder there, the app's file name, the library name.
$FrameSlug = 'doom-vr'
$FrameApkName = Split-Path -Leaf $Apk
$FrameSaveRoot = Join-Path ([Environment]::GetFolderPath('MyDocuments')) ($ProductFile + '\Steam Frame saves')

# The engine's own files: the app carries them, so the PC's copies stay here.
$EngineFiles = @('uzdoom.exe', 'uzdoom.pk3', 'brightmaps.pk3', 'game_support.pk3', 'game_widescreen_gfx.pk3', 'lights.pk3',
                 'openxr_loader.dll', 'soft_oal.dll', 'sndfile.dll', 'licenses.zip', 'version.txt', 'engine-version.txt')
# Folders of the PC's game folder that stay on the PC: the PC Setup's own scripts and tools, the
# Game Tome's Windows layer and books (no book on the headsets yet), logs, screenshots, saves.
$SkipDirs = @('setup', 'logs', 'screenshots', 'saves', 'soundfonts', 'fm_banks', '.config')

# --- the log ----------------------------------------------------------------------

$Mode = 'install'
if ($Uninstall) { $Mode = 'uninstall' }
if ($Collect)   { $Mode = 'collect' }
if ($Detect)    { $Mode = 'detect' }
if ($Start)     { $Mode = 'start' }
$LogName = 'doom-vr' + $(if ($SteamFrame) { '-frame-' } else { '-quest-' }) + $Mode + '.txt'
$LogFile = Join-Path ([System.IO.Path]::GetTempPath()) $LogName
if ($Mode -eq 'install' -and (Test-Path -LiteralPath $Apk)) { $LogFile = Join-Path (Split-Path -Parent $QuestHere) 'setup-log.txt' }
$Result = [ordered]@{ product = $Product; platform = $(if ($SteamFrame) { 'steamframe' } else { 'quest' }); version = $Version; mode = $Mode; result = 'ok' }
$Platform = $(if ($SteamFrame) { 'Steam Frame' } else { 'Quest' })
try { Set-Content -LiteralPath $LogFile -Value ($Product + ' for ' + $Platform + ' ' + $Version + ' ' + $Mode + ', ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' on ' + $env:COMPUTERNAME) -Encoding UTF8 }
catch { $LogFile = Join-Path ([System.IO.Path]::GetTempPath()) $LogName; Set-Content -LiteralPath $LogFile -Value ($Product + ' for ' + $Platform + ' ' + $Mode) -Encoding UTF8 }

function Say([string] $m, [string] $color) {
    if ($Json) { return }
    if ($color) { Write-Host $m -ForegroundColor $color } else { Write-Host $m }
}
function Log([string] $m) {
    Say $m
    try { Add-Content -LiteralPath $LogFile -Value $m -Encoding UTF8 } catch { }
}
function Ask-YesNo([string] $q) {
    if ($NoPrompt) { return $true }
    while ($true) {
        $a = Read-Host ('  ' + $q + ' [y/n]')
        if ($a -match '^[yY]') { return $true }
        if ($a -match '^[nN]') { return $false }
    }
}
function Finish([int] $code) {
    if ($Json) {
        $Result['exitCode'] = $code
        Write-Output ($Result | ConvertTo-Json -Depth 5)
    }
    exit $code
}
trap {
    Log ''
    Log ('ERROR: ' + $_.Exception.Message)
    Log ('  at ' + $_.InvocationInfo.PositionMessage)
    Log ''
    Log ('Setup stopped.  Run "' + $(if ($SteamFrame) { 'Collect Steam Frame logs.bat' } else { 'Collect logs.bat' }) + '" and send us the file it makes.')
    $Result['result'] = 'error'
    $Result['error'] = $_.Exception.Message
    if ($Json) { $Result['exitCode'] = 2; Write-Output ($Result | ConvertTo-Json -Depth 5) }
    exit 2
}

function Banner([string] $what) {
    Say ''
    Say ('  ' + $Product + ' for ' + $Platform + ' ' + $Version + ': ' + $what) 'Cyan'
    Say ('  ' + ('-' * ($Product.Length + $Platform.Length + $Version.Length + $what.Length + 8)))
    Say ''
}

function Size-Text([long] $bytes) {
    if ($bytes -ge 1GB) { return ('{0:N1} GB' -f ($bytes / 1GB)) }
    return ('{0:N0} MB' -f [Math]::Max(1, [Math]::Round($bytes / 1MB)))
}

# --- the game folder: the PC version's, or one put together here -----------------------

# A set-up Doom VR game folder: the launchers Setup wrote and the games they start.
function Is-GameFolder([string] $dir) {
    return (Test-Path -LiteralPath (Join-Path $dir 'launchers')) -and (Test-Path -LiteralPath (Join-Path $dir 'wads')) -and
           @(Get-ChildItem -LiteralPath (Join-Path $dir 'launchers') -Filter '*.bat' -File -ErrorAction SilentlyContinue).Count -gt 0
}

# The game folder in or under a folder the player named: the folder itself, its VR\game, its game.
function GameFolder-In([string] $dir) {
    if (-not $dir -or -not (Test-Path -LiteralPath $dir)) { return $null }
    foreach ($c in @($dir, (Join-Path $dir 'game'), (Join-Path $dir 'VR\game'))) {
        if ((Test-Path -LiteralPath $c) -and (Is-GameFolder $c)) { return (Resolve-Path -LiteralPath $c).ProviderPath }
    }
    return $null
}

function Steam-Libraries {
    $libs = @()
    $root = $null
    foreach ($k in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try { $v = (Get-ItemProperty -Path $k -ErrorAction Stop); if ($v.SteamPath) { $root = $v.SteamPath } elseif ($v.InstallPath) { $root = $v.InstallPath } } catch { }
        if ($root) { break }
    }
    if (-not $root) { return $libs }
    $root = $root.Replace('/', '\')
    $libs += $root
    $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $p = $m.Groups[1].Value.Replace('\\', '\')
            if ($libs -notcontains $p) { $libs += $p }
        }
    }
    return $libs
}

# The PC version's set-up game folder, if there is one: what the player named first, then
# every Steam library's Doom folder, GOG's, and Documents\Doom VR.
function Find-PcGame {
    foreach ($d in @($Dropped) | Where-Object { $_ }) {
        $g = GameFolder-In $d
        if ($g) { Log ('the PC''s game folder (named): ' + $g); return $g }
        Log ('not a Doom VR game folder: ' + $d)
    }
    if ($NoSearch) { return $null }
    $found = @()
    foreach ($lib in Steam-Libraries) {
        $common = Join-Path $lib 'steamapps\common'
        if (-not (Test-Path -LiteralPath $common)) { continue }
        foreach ($d in Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue) {
            $g = GameFolder-In (Join-Path $d.FullName 'VR')
            if ($g) { $found += $g }
        }
    }
    foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games')) {
        if (-not (Test-Path $k)) { continue }
        foreach ($sub in Get-ChildItem -Path $k -ErrorAction SilentlyContinue) {
            try { $p = (Get-ItemProperty -Path $sub.PSPath -ErrorAction Stop).path } catch { continue }
            $g = GameFolder-In (Join-Path $p 'VR')
            if ($g) { $found += $g }
        }
    }
    foreach ($d in @((Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Doom VR'), (Join-Path $env:USERPROFILE 'Games\Doom VR'))) {
        $g = GameFolder-In $d
        if ($g) { $found += $g }
    }
    $found = @($found | Sort-Object -Unique)
    if ($found.Count -eq 0) { Log 'the PC version: not set up on this PC'; return $null }
    # Several: the one set up most recently (its launchers folder).
    $pick = $found | Sort-Object { (Get-Item -LiteralPath (Join-Path $_ 'launchers')).LastWriteTime } -Descending | Select-Object -First 1
    Log ('the PC''s game folder: ' + $pick + $(if ($found.Count -gt 1) { ' (of ' + $found.Count + ')' } else { '' }))
    return $pick
}

# No PC install: the same folder put together here, by the PC Setup's own game search
# (every Steam library, GOG, the folders named), into $Stage\game. Kept for the next update.
function Stage-Game {
    $game = Join-Path $Stage 'game'
    New-Item -ItemType Directory -Force -Path $game | Out-Null
    # The PC Setup's scripts and the port's own files from this download, refreshed each time.
    $src = Join-Path $QuestHere 'game'
    if (-not (Test-Path -LiteralPath (Join-Path $src 'setup\setup.ps1'))) { throw ('this download has no PC Setup to put the games together with (' + $src + ')') }
    foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File) {
        $rel = $f.FullName.Substring($src.Length + 1)
        $dst = Join-Path $game $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        if (-not (Test-Path -LiteralPath $dst) -or (Get-Item -LiteralPath $dst).Length -ne $f.Length -or (Get-Item -LiteralPath $dst).LastWriteTimeUtc -lt $f.LastWriteTimeUtc) {
            Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        }
    }
    Say ''
    Say '  Looking for your Doom-family games on this PC and putting the game folder together...'
    Say '  (the PC Setup runs here; the free games and mods it fetches are kept for the next update)'
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $game 'setup\setup.ps1'), '-Install', $game, '-Unattended')
    if ($NoDownload) { $args2 += '-NoDownload' }
    if ($Mirror) { $args2 += @('-Mirror', $Mirror) }
    foreach ($d in @($Dropped) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }) { $args2 += $d }
    $env:DOOMVR_HEADSET_SETUP = '1'
    try {
        $p = Start-Process -FilePath 'powershell' -ArgumentList ($args2 | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -NoNewWindow -Wait -PassThru
        Log ('the PC Setup finished with exit code ' + $p.ExitCode)
    } finally { $env:DOOMVR_HEADSET_SETUP = $null }
    if (-not (Is-GameFolder $game)) { return $null }
    return $game
}

# What goes to the headset from a game folder: relative path (with /) -> the file. Not the
# engine's own files (the app has them), not the PC Setup's folder, not logs or saves.
function Game-Files([string] $game) {
    $game = (Resolve-Path -LiteralPath $game).ProviderPath.TrimEnd('\')
    $list = @{}
    foreach ($f in Get-ChildItem -LiteralPath $game -Recurse -File) {
        $rel = $f.FullName.Substring($game.Length + 1).Replace('\', '/')
        $top = ($rel -split '/')[0]
        if ($rel -notmatch '/' -and ($EngineFiles -contains $f.Name)) { continue }
        if ($SkipDirs -contains $top) { continue }
        if ($f.Extension -in @('.exe', '.dll')) { continue }
        # The Game Tome: its books go (tome, tome-<game>, books.txt); the PC's layer (.dll, .json, layer\) stays.
        if ($top -eq 'gametome' -and ($f.Extension -eq '.json' -or $rel -match '^gametome/layer/' -or $f.Name -match '^GameTomeLayer')) { continue }
        if ($f.Name -match '^log-doomvr.*\.txt$|^log-hands\.txt$|^setup-log\.txt$|^doomvr\.ini$|\.prev\.txt$|^\.') { continue }
        $list[$rel] = $f
    }
    return $list
}

# launch.txt: what the app starts with, one argument a line - the game DoomVR.bat starts on
# the PC, from its launcher's own line, with the picker up over it (as on the PC).
function Launch-Lines([string] $game) {
    $start = ''
    $top = Join-Path $game 'DoomVR.bat'
    if (Test-Path -LiteralPath $top) {
        foreach ($line in Get-Content -LiteralPath $top) { if ($line -match 'launchers\\(.+)\.bat"') { $start = $Matches[1]; break } }
    }
    $launchers = Join-Path $game 'launchers'
    if (-not $start) {
        foreach ($d in @('Doom (2024 re-release)', 'Doom', 'Doom II (2024 re-release)', 'Doom II')) { if (Test-Path -LiteralPath (Join-Path $launchers ($d + '.bat'))) { $start = $d; break } }
    }
    if (-not $start) { $first = Get-ChildItem -LiteralPath $launchers -Filter '*.bat' -File | Select-Object -First 1; if ($first) { $start = $first.BaseName } }
    if (-not $start) { return @() }
    $bat = Join-Path $launchers ($start + '.bat')
    $cmd = ''
    foreach ($line in Get-Content -LiteralPath $bat) { if ($line -match '^\s*\.\\uzdoom\.exe\s+(.*)$') { $cmd = $Matches[1]; break } }
    if (-not $cmd) { return @() }
    # The launcher's words, quotes off; the launcher's own folder is launchers\, so %~dp0.. is the game folder.
    $words = @()
    foreach ($m in [regex]::Matches($cmd, '"([^"]*)"|(\S+)')) { $words += $(if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }) }
    $out = @()
    for ($i = 0; $i -lt $words.Count; $i++) {
        $w = $words[$i]
        if ($w -in @('-config', '+logfile', '+vid_preferbackend', '+vid_fullscreen', '+vr_mode')) { $i++; continue }
        if ($w -eq '%*') { continue }
        $w = $w.Replace('%~dp0..\', '').Replace('%~dp0..', '.').Replace('%~dp0', 'launchers\').Replace('\', '/')
        $out += $w
    }
    $out += @('+vr_open_picker', '1')
    Log ('  starts with: ' + $start)
    return $out
}

# --- adb: talking to the headset ----------------------------------------------------------

$AdbExe = $null
$AdbCache = Join-Path $env:LOCALAPPDATA 'Game Or Die\platform-tools'

function Find-Adb {
    if ($Adb) { return $Adb }
    $c = @()
    try { $c += (Get-Command adb.exe -ErrorAction Stop).Source } catch { }
    $c += Join-Path $env:APPDATA 'SideQuest\platform-tools\adb.exe'
    $c += Join-Path $env:LOCALAPPDATA 'Programs\SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe'
    $c += Join-Path $AdbCache 'adb.exe'
    foreach ($p in $c) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }
    return $null
}

function Get-Adb {
    $script:AdbExe = Find-Adb
    if ($AdbExe) { Log ('adb: ' + $AdbExe); return }
    Say '  To talk to the headset this needs adb, Google''s Android tool (about 7 MB).'
    Say '  It comes straight from Google, under their terms:'
    Say '  https://developer.android.com/studio/releases/platform-tools'
    if (-not $NoPrompt -and -not (Ask-YesNo 'Download it now?')) { throw 'adb is needed; nothing was changed.' }
    $zip = Join-Path ([System.IO.Path]::GetTempPath()) 'gameordie-platform-tools.zip'
    Log 'Downloading Google''s platform tools...'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $zip
    $parent = Split-Path -Parent $AdbCache
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if (Test-Path -LiteralPath $AdbCache) { Remove-Item -LiteralPath $AdbCache -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $parent -Force
    Remove-Item -LiteralPath $zip -Force
    $script:AdbExe = Join-Path $AdbCache 'adb.exe'
    if (-not (Test-Path -LiteralPath $AdbExe)) { throw 'the platform tools download had no adb.exe' }
    Log ('adb: ' + $AdbExe + ' (downloaded)')
}

# Runs adb and returns @{ Code; Out } (stdout and stderr together). Through a
# Process, so adb's progress on stderr never trips PowerShell's error handling.
function Run-Adb([string[]] $a, [switch] $NoSerial) {
    $all = @()
    if ($script:Serial -and -not $NoSerial) { $all += @('-s', $script:Serial) }
    $all += $a
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $AdbExe
    $psi.Arguments = (($all | ForEach-Object { if ($_ -match '[\s"&]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    # If this call started adb's server, the server holds our output open and
    # it never ends: once adb itself is done, wait a moment, not forever.
    $done = $o.Wait(5000) -and $e.Wait(1000)
    $out = if ($done) { ($o.Result + $e.Result).Trim() } else { '' }
    return @{ Code = $p.ExitCode; Out = $out }
}

# adb's server, started on its own through the shell so that it inherits no
# handle of ours (a hub reading -Json may be capturing this script's output).
function Start-AdbServer {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $AdbExe
    $psi.Arguments = 'start-server'
    $psi.UseShellExecute = $true
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $p = [System.Diagnostics.Process]::Start($psi)
    [void]$p.WaitForExit(30000)
}

function Shell([string] $cmd) { return (Run-Adb @('shell', $cmd)).Out }

# Waits for one headset that has allowed this PC, saying what to do meanwhile.
function Wait-Headset {
    Start-AdbServer
    # A headset on the network (a name or an address, not a USB serial): connect to it.
    if ($Serial -and $Serial -notmatch '^emulator-' -and $Serial -match '[.:]|^[A-Za-z][\w-]*$' -and $Serial -notmatch '^[0-9A-Z]{10,}$') {
        if ($Serial -notmatch ':\d+$') { $script:Serial = $Serial + ':5555' }
        $r = Run-Adb @('connect', $Serial) -NoSerial
        Log ('network headset ' + $Serial + ': ' + $r.Out)
    }
    $said = ''
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ($true) {
        $lines = ((Run-Adb @('devices', '-l') -NoSerial).Out -split "`r?`n") | Where-Object { $_ -match '^\S+\s+(device|unauthorized|offline|authorizing|no permissions)' }
        $devs = @($lines | ForEach-Object { $f = $_ -split '\s+'; @{ Id = $f[0]; State = $f[1]; Line = $_ } })
        # No headset named: the Quest on the cable, never one left connected over the network.
        if ($Serial) { $devs = @($devs | Where-Object { $_.Id -eq $Serial }) }
        else { $devs = @($devs | Where-Object { $_.Id -notmatch ':\d+$' }) }
        $ready = @($devs | Where-Object { $_.State -eq 'device' })
        if ($ready.Count -ge 1) {
            $pick = $ready | Where-Object { $_.Line -match 'Quest|eureka|hollywood|seacliff|panther' } | Select-Object -First 1
            if (-not $pick) { $pick = $ready[0] }
            $script:Serial = $pick.Id
            # A desk emulator's adb may run as root: a file it pushes into the app's folder is then
            # the root user's, and the app may not even look at it. Back to the shell user first.
            if ((Shell 'id') -match 'uid=0\(') {
                Log '  adb runs as root on this device: dropping to the shell user (adb unroot)'
                Run-Adb @('unroot') | Out-Null
                Start-Sleep -Seconds 3
                $script:Serial = $pick.Id
            }
            return $true
        }
        $now = 'none'
        if ($devs | Where-Object { $_.State -eq 'unauthorized' -or $_.State -eq 'authorizing' }) { $now = 'unauthorized' }
        elseif ($devs | Where-Object { $_.State -eq 'offline' -or $_.State -eq 'no permissions' }) { $now = 'offline' }
        if ($now -ne $said) {
            $said = $now
            Say ''
            switch ($now) {
                'none' {
                    Say '  Waiting for the headset.' 'Yellow'
                    Say '  Turn it on and plug it into this PC with a USB-C cable (the one it charges'
                    Say '  with works if it carries data). Developer mode has to be on: README.txt,'
                    Say '  "Before you start", shows how in a minute.'
                }
                'unauthorized' {
                    Say '  The headset is asking whether to trust this PC.' 'Yellow'
                    Say '  Put it on and choose Allow (tick "Always allow from this computer").'
                    Say '  No message? Unplug the cable and plug it back in, with the headset on.'
                }
                'offline' {
                    Say '  The headset is connected but not answering.' 'Yellow'
                    Say '  Unplug the cable and plug it back in.'
                }
            }
            Log ('headset: ' + $now)
        }
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep -Seconds 2
    }
}

function Headset-Info {
    $make = Shell 'getprop ro.product.manufacturer'
    $model = Shell 'getprop ro.product.model'
    $os = Shell 'getprop ro.build.version.release'
    Log ('headset: ' + $Serial + ', ' + $make + ' ' + $model + ', Android ' + $os)
    $Result['headset'] = [ordered]@{ id = $Serial; make = $make; model = $model; android = $os }
    if ($make -match 'Valve' -or $model -match 'Lepton') {
        $script:IsFrame = $true
        $script:SaveRoot = Join-Path ([Environment]::GetFolderPath('MyDocuments')) ($ProductFile + '\Steam Frame saves')
        Say '  This is a Steam Frame: the same app runs there, under Lepton.'
    } elseif (-not ($make -match 'Oculus|Meta' -or $model -match 'Quest')) {
        Say ('  This is a ' + $make + ' ' + $model + ', not a Meta Quest. Carrying on; the app is made for Quest.') 'Yellow'
    }
}

function Installed-Version {
    $d = Shell ('dumpsys package ' + $AppId)
    if ($d -match 'versionName=(\S+)') { return $Matches[1] }
    return $null
}

# Every file under the game's folder on the headset: name -> bytes.
function Remote-Sizes {
    $map = @{}
    $out = Shell ('cd "' + $RemoteGame + '" 2>/dev/null && find . -type f -exec stat -c "%s|%n" {} + 2>/dev/null')
    foreach ($line in ($out -split "`r?`n")) {
        if ($line -match '^(\d+)\|\./(.+)$') { $map[$Matches[2]] = [long]$Matches[1] }
    }
    return $map
}

# Folders adb makes belong to adb's user, and the game may not look into them;
# opened to everyone. The folders the app made itself are left alone.
function Open-AdbDirs {
    Shell ('cd "' + $RemoteGame + '" 2>/dev/null && find . -type d -user shell -exec chmod 2775 {} + 2>/dev/null; true') | Out-Null
}

# The app makes its writable folders and takes in any name.restore Setup copied
# (DoomVRActivity.prepare). Returns how many folders it can write.
function App-Prepare {
    $r = Run-Adb @('shell', 'am', 'broadcast', '-f', '32', '-n', ($AppId + '/.SetupReceiver'))
    if ($r.Out -match 'result=(\d+)') { return [int]$Matches[1] }
    Log ('  the app did not answer Setup: ' + $r.Out)
    return 0
}

# --- settings and saved games -------------------------------------------------------------------

function Backup-Saves([string] $to) {
    $n = 0
    foreach ($item in $SaveItems) {
        $there = (Shell ('ls -d "' + $RemoteGame + '/' + $item + '" 2>/dev/null')).Trim()
        if (-not $there -or $there -match 'No such') { continue }
        $dst = Join-Path $to ($item.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        $r = Run-Adb @('pull', ($RemoteGame + '/' + $item), $dst)
        if ($r.Code -eq 0) { $n += @(Get-ChildItem -LiteralPath $dst -Recurse -File -ErrorAction SilentlyContinue).Count; if (Test-Path -LiteralPath $dst -PathType Leaf) { $n++ } }
        else { Log ('  could not copy ' + $item + ': ' + $r.Out) }
    }
    return $n
}

# Puts saved files back as name.restore; the app takes them in as its own.
function Restore-Saves([string] $from) {
    $n = 0
    foreach ($f in Get-ChildItem -LiteralPath $from -Recurse -File -ErrorAction SilentlyContinue) {
        $rel = $f.FullName.Substring($from.Length + 1).Replace('\', '/')
        $r = Run-Adb @('push', $f.FullName, ($RemoteGame + '/' + $rel + '.restore'))
        if ($r.Code -eq 0) { $n++ }
    }
    if ($n) { App-Prepare | Out-Null }
    return $n
}

function Has-Settings {
    $out = Shell ('ls "' + $RemoteGame + '/doomvr.ini" 2>/dev/null')
    return [bool]($out.Trim() -and $out -notmatch 'No such')
}

# --- the game folder for this run -----------------------------------------------------------------

function Get-Game {
    Say '  Looking for Doom VR''s game folder on this PC...'
    $game = Find-PcGame
    $Result['pcGame'] = $game
    if (-not $game) {
        $game = Stage-Game
        $Result['staged'] = [bool]$game
    }
    if (-not $game) {
        Say ''
        Say '  No Doom-family game was found on this PC, so there is nothing to put on the headset.' 'Yellow'
        Say '  Install Doom VR''s PC version and run its Setup first, or drop your DOOM + DOOM II folder on Setup.bat.'
        $Result['result'] = 'no games'
        Finish 1
    }
    $files = Game-Files $game
    $total = ($files.Values | Measure-Object -Property Length -Sum).Sum
    Log ('the game folder: ' + $game + ' - ' + $files.Count + ' files, ' + (Size-Text $total) + ' for the headset')
    $launch = Launch-Lines $game
    if (-not $launch) { Log '  (no launcher to start with: the engine will look for a game itself)' }
    return @{ Dir = $game; Files = $files; Total = $total; Launch = $launch }
}

function Write-Temp([string] $name, [string[]] $lines) {
    $p = Join-Path ([System.IO.Path]::GetTempPath()) ('doom-vr-quest-' + $name)
    [System.IO.File]::WriteAllLines($p, $lines, (New-Object System.Text.UTF8Encoding($false)))
    return $p
}

# --- install ------------------------------------------------------------------------------

function Do-Install {
    Banner 'Setup'
    if (-not (Test-Path -LiteralPath $Apk)) { throw ('the app is missing from this folder: ' + $Apk) }

    # 1. The game folder: the PC version's, or one put together here.
    $g = Get-Game

    # 2. The headset.
    Get-Adb
    if (-not (Wait-Headset)) { Log 'No headset after waiting; nothing was changed.'; Finish 3 }
    Say ''
    Say '  Headset connected.' 'Green'
    Headset-Info

    # 3. Room for it all.
    $need = (Get-Item -LiteralPath $Apk).Length + $g.Total + 100MB
    $f2 = (Shell 'df -k /sdcard | tail -1') -split '\s+'
    if ($f2.Count -ge 4 -and $f2[3] -match '^\d+$') {
        $free = [long]$f2[3] * 1KB
        Log ('free on the headset: ' + (Size-Text $free) + ', needed at most ' + (Size-Text $need))
        if ($free -lt $need) { throw ('the headset needs ' + (Size-Text $need) + ' free and has ' + (Size-Text $free) + '. Remove something and run Setup again.') }
    }

    # 4. Settings and saved games: a copy on this PC first, every time.
    $was = Installed-Version
    $stamp = Get-Date -Format 'yyyy-MM-dd HHmm'
    if ($was) {
        Log ('installed now: ' + $AppName + ' ' + $was)
        $n = Backup-Saves (Join-Path $SaveRoot $stamp)
        if ($n) { Log ('  ' + $n + ' settings and save files copied to ' + (Join-Path $SaveRoot $stamp)) }
    }

    # 5. The app.
    Say ''
    Say ('  Installing ' + $AppName + ' ' + $Version + ' on the headset...')
    $r = Run-Adb @('install', '-r', $Apk)
    if ($r.Out -match 'INSTALL_FAILED_UPDATE_INCOMPATIBLE|signatures do not match') {
        # A build signed with another key (a test build) is in the way: it has
        # to go first, which also clears its files. The saves are on the PC.
        Log '  A different build of the app is on the headset; replacing it.'
        $u = Run-Adb @('uninstall', $AppId)
        Log ('  removed: ' + $u.Out)
        $r = Run-Adb @('install', $Apk)
    }
    if ($r.Code -ne 0 -or $r.Out -notmatch 'Success') { throw ('the app did not install: ' + $r.Out) }
    Log ('  installed (' + ($r.Out -split "`r?`n" | Select-Object -Last 1) + ')')

    # 6. The app's own folders (the ones the game writes), made by the app.
    # The app answers 1 when every one of its folders can be written, 0 otherwise.
    $w = App-Prepare
    Log ('  the app made its folders: ' + $(if ($w -eq 1) { 'all ' + $WritableDirs.Count + ' writable' } else { 'NOT all writable' }))
    if ($w -ne 1) { Log '  (settings or saved games may not be kept: Collect logs.bat and send us the zip)' }

    # 7. The game's files: only what is missing or changed.
    $have = Remote-Sizes
    # A book's own notes (ink.txt, typed.txt: what the player wrote in the headset) are never written over.
    $todo = @($g.Files.Keys | Where-Object { (-not $have.ContainsKey($_) -or $have[$_] -ne $g.Files[$_].Length) -and -not ($have.ContainsKey($_) -and $_ -match '/(ink|typed)\.txt$') } | Sort-Object)
    if (-not $todo) { Log '  the game''s files are already on the headset' }
    else {
        $bytes = ($todo | ForEach-Object { $g.Files[$_].Length } | Measure-Object -Sum).Sum
        Say ''
        Say ('  Copying the games to the headset (' + $todo.Count + ' files, ' + (Size-Text $bytes) + ')...')
        # Whole folders the headset does not have yet go over as one archive each,
        # unpacked on the headset by its own tar: much quicker than file by file,
        # and adb push may not make folders inside the app's storage on Android 13
        # and later (a shell on the headset may).
        $missingTop = @($todo | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique | Where-Object { $t = $_; -not ($have.Keys | Where-Object { $_ -like ($t + '/*') -or $_ -eq $t } | Select-Object -First 1) })
        $done = @{}
        $tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
        foreach ($top in $missingTop) {
            $files = @($todo | Where-Object { $_ -like ($top + '/*') })
            if ($files.Count -lt 20 -or -not (Test-Path -LiteralPath $tarExe)) { continue }
            $list = Write-Temp ('list-' + ($top -replace '[^\w]', '_') + '.txt') $files
            $tar = Join-Path ([System.IO.Path]::GetTempPath()) ('doom-vr-quest-' + ($top -replace '[^\w]', '_') + '.tar')
            & $tarExe -cf $tar -C $g.Dir -T $list
            if ($LASTEXITCODE) { throw ('packing ' + $top + ' failed') }
            Say ('    ' + $top + '/  (' + $files.Count + ' files)')
            $tmpRemote = '/data/local/tmp/doom-vr-' + ($top -replace '[^\w]', '_') + '.tar'
            $p = Run-Adb @('push', $tar, $tmpRemote)
            if ($p.Code -ne 0) { throw ('copying ' + $top + ' failed: ' + $p.Out) }
            $x = Shell ('cd "' + $RemoteGame + '" && tar -xf "' + $tmpRemote + '" && echo UNPACKED; rm -f "' + $tmpRemote + '"')
            if ($x -notmatch 'UNPACKED') { throw ('unpacking ' + $top + ' on the headset failed: ' + $x) }
            foreach ($k in $files) { $done[$k] = $true }
            Remove-Item -LiteralPath $tar, $list -Force
        }
        $rest = @($todo | Where-Object { -not $done.ContainsKey($_) })
        $parents = @($rest | ForEach-Object { $i = $_.LastIndexOf('/'); if ($i -gt 0) { $_.Substring(0, $i) } } | Sort-Object -Unique)
        if ($parents) { Shell ('cd "' + $RemoteGame + '" && mkdir -p ' + (($parents | ForEach-Object { '"' + $_ + '"' }) -join ' ')) | Out-Null }
        $i = 0
        foreach ($k in $rest) {
            $i++
            Say ('    ' + $i + '/' + $rest.Count + '  ' + $k)
            $p = Run-Adb @('push', $g.Files[$k].FullName, ($RemoteGame + '/' + $k))
            if ($p.Code -ne 0) { throw ('copying ' + $k + ' failed: ' + $p.Out) }
        }
        Open-AdbDirs
        $have = Remote-Sizes
        $bad = @($g.Files.Keys | Where-Object { -not $have.ContainsKey($_) -or $have[$_] -ne $g.Files[$_].Length })
        if ($bad) { throw ($bad.Count + ' files did not arrive whole (' + (($bad | Select-Object -First 5) -join ', ') + '). Run Setup again.') }
        Log ('  ' + $todo.Count + ' files copied and checked')
    }

    # 7b. The Game Tome's controller models (its Controls page draws the real controllers): a
    #     PC reads them from SteamVR, a headset has none, so this release carries them and
    #     they go beside the books, once (only when missing or changed).
    $ctl = Join-Path $QuestHere 'game\gametome\controllers'
    if (Test-Path -LiteralPath $ctl) {
        $ctlRoot = (Resolve-Path -LiteralPath $ctl).ProviderPath.TrimEnd('\')
        $ctlFiles = @{}
        foreach ($f in Get-ChildItem -LiteralPath $ctlRoot -Recurse -File) { $ctlFiles['gametome/controllers/' + $f.FullName.Substring($ctlRoot.Length + 1).Replace('\', '/')] = $f }
        $ctlTodo = @($ctlFiles.Keys | Where-Object { -not $have.ContainsKey($_) -or $have[$_] -ne $ctlFiles[$_].Length })
        if ($ctlTodo) {
            Say ('  Copying the controller models for the Game Tome (' + $ctlFiles.Count + ' files)...')
            $tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
            $tar = Join-Path ([System.IO.Path]::GetTempPath()) 'doom-vr-quest-controllers.tar'
            & $tarExe -cf $tar -C (Split-Path -Parent $ctlRoot) 'controllers'
            if ($LASTEXITCODE) { throw 'packing the controller models failed' }
            $p = Run-Adb @('push', $tar, '/data/local/tmp/doom-vr-controllers.tar')
            if ($p.Code -ne 0) { throw ('copying the controller models failed: ' + $p.Out) }
            $x = Shell ('mkdir -p "' + $RemoteGame + '/gametome" && cd "' + $RemoteGame + '/gametome" && tar -xf /data/local/tmp/doom-vr-controllers.tar && echo UNPACKED; rm -f /data/local/tmp/doom-vr-controllers.tar')
            if ($x -notmatch 'UNPACKED') { throw ('unpacking the controller models on the headset failed: ' + $x) }
            Remove-Item -LiteralPath $tar -Force
            Open-AdbDirs
            Log ('  controller models: ' + $ctlFiles.Count + ' files copied')
        } else { Log '  the controller models are already on the headset' }
    }

    # 8. What the app starts with, from the PC's own start launcher.
    if ($g.Launch) {
        $lf = Write-Temp 'launch.txt' $g.Launch
        $p = Run-Adb @('push', $lf, ($RemoteGame + '/launch.txt'))
        if ($p.Code -ne 0) { throw ('copying launch.txt failed: ' + $p.Out) }
        Remove-Item -LiteralPath $lf -Force
    }

    # 9. The settings, the first time only (the player's own after that): the PC's, so hand
    #    fits and options carry over.
    $ini = Join-Path $g.Dir 'doomvr.ini'
    if ((Test-Path -LiteralPath $ini) -and -not (Has-Settings)) {
        $r = Run-Adb @('push', $ini, ($RemoteGame + '/doomvr.ini.restore'))
        if ($r.Code -eq 0) { App-Prepare | Out-Null; Log '  settings: the PC''s put in place' }
    }

    # 10. The saves back, if the headset has none now (a replaced build).
    $dirs = @(Get-ChildItem -LiteralPath $SaveRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    if ($dirs -and -not (Has-Settings)) {
        $n = Restore-Saves $dirs[0].FullName
        if ($n) { Log ('  ' + $n + ' settings and save files put back (from ' + $dirs[0].Name + ')') }
    }

    Say ''
    Log 'Done.'
    Say ''
    Say '  On the headset: Library, then Unknown Sources, then Doom VR.' 'Green'
    Say '  You can unplug the headset now.'
    if (Test-Path -LiteralPath $SaveRoot) { Say ('  Settings and saves are copied to ' + $SaveRoot + ' each time Setup runs.') }
    Finish 0
}

# --- uninstall ----------------------------------------------------------------------------

function Do-Uninstall {
    Banner 'Uninstall'
    Get-Adb
    if (-not (Wait-Headset)) { Log 'No headset; nothing was changed.'; Finish 3 }
    Headset-Info
    $was = Installed-Version
    if (-not $was) { Log ('  ' + $AppName + ' is not on this headset.'); Finish 0 }
    $stamp = Get-Date -Format 'yyyy-MM-dd HHmm'
    $kept = Backup-Saves (Join-Path $SaveRoot $stamp)
    if ($kept) { Log ('  ' + $kept + ' settings and save files copied to this PC') }
    $u = Run-Adb @('uninstall', $AppId)
    if ($u.Out -notmatch 'Success') { throw ('the app was not removed: ' + $u.Out) }
    Log ('  ' + $AppName + ' ' + $was + ' removed from the headset, with the game files it was given.')
    if ($kept) { Say ('  Your settings and saved games are kept in ' + $SaveRoot + '; Setup puts them back.') }
    if (Test-Path -LiteralPath $Stage) {
        Say ('  The game folder put together for the headset stays on this PC (' + $Stage + '),')
        Say '  so a new install needs no download. Delete that folder to free the space.'
    }
    Log 'Done.'
    Finish 0
}

# --- collect logs -------------------------------------------------------------------------

function Do-Collect {
    Banner 'Collect logs'
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('doom-vr-quest-logs-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    foreach ($f in @((Join-Path (Split-Path -Parent $QuestHere) 'setup-log.txt'), (Join-Path ([System.IO.Path]::GetTempPath()) 'doom-vr-quest-install.txt'), (Join-Path ([System.IO.Path]::GetTempPath()) 'doom-vr-quest-uninstall.txt'))) {
        if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $tmp }
    }
    Get-Adb
    if (Wait-Headset) {
        Headset-Info
        $v = Installed-Version
        Log ('installed: ' + $(if ($v) { $AppName + ' ' + $v } else { 'not installed' }))
        $to = Join-Path $tmp 'headset'
        New-Item -ItemType Directory -Force -Path (Join-Path $to 'logs') | Out-Null
        # The game's logs (this run and the last), the kept runs, the settings, the start line.
        foreach ($n in @('log-doomvr.txt', 'log-doomvr.prev.txt', 'log-doomvr-shutdown.txt', 'log-doomvr-shutdown.prev.txt', 'log-hands-report.txt', 'log-setup.txt', 'doomvr.ini', 'launch.txt', 'engine-version.txt')) {
            Run-Adb @('pull', ($RemoteGame + '/' + $n), (Join-Path $to $n)) | Out-Null
        }
        # The Game Tome's layer: its own log.
        Run-Adb @('pull', ($RemoteGame + '/gametome/GameTomeLayer_log.txt'), (Join-Path $to 'GameTomeLayer_log.txt')) | Out-Null
        $logs = ((Shell ('ls -t "' + $RemoteGame + '/logs" 2>/dev/null | head -6')) -split "`r?`n") | Where-Object { $_.Trim() -and $_ -notmatch 'No such' }
        foreach ($n in $logs) { Run-Adb @('pull', ($RemoteGame + '/logs/' + $n.Trim()), (Join-Path (Join-Path $to 'logs') $n.Trim())) | Out-Null }
        # What the folder holds, by name and size (no file contents).
        Set-Content -LiteralPath (Join-Path $to 'files.txt') -Value (Shell ('ls -lR "' + $Remote + '" 2>&1 | head -600')) -Encoding UTF8
        # The app's own lines and any crash, nothing from other apps.
        $lc = (Run-Adb @('logcat', '-d', '-v', 'time', '-s', 'DoomVR:*', 'SDL:*', 'SDL/APP:*', 'OpenXR-Loader:*', 'openal:*', 'AndroidRuntime:*', 'DEBUG:*', 'libc:F')).Out
        Set-Content -LiteralPath (Join-Path $tmp 'logcat.txt') -Value $lc -Encoding UTF8
    } else {
        Log 'No headset connected: only this PC''s logs are collected.'
    }
    Copy-Item -LiteralPath $LogFile -Destination $tmp
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ($ProductFile + $(if ($IsFrame) { ' Steam Frame' } else { ' Quest' }) + ' logs.zip')
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip
    Remove-Item -LiteralPath $tmp -Recurse -Force
    $Result['zip'] = $zip
    Say ''
    Say ('  Logs saved to ' + $zip) 'Green'
    Say '  Send that file to us (Discord or GitHub).'
    if (-not $NoPrompt) { try { Start-Process explorer.exe ('/select,"' + $zip + '"') } catch { } }
    Finish 0
}

# --- detect -------------------------------------------------------------------------------

function Do-Detect {
    $game = Find-PcGame
    $Result['pcGame'] = $game
    $Result['staged'] = Test-Path -LiteralPath (Join-Path $Stage 'game\launchers')
    $script:AdbExe = Find-Adb
    if ($AdbExe) {
        $script:WaitSeconds = 0
        if (Wait-Headset) { Headset-Info; $v = Installed-Version; $Result['installed'] = $v; Log ('installed: ' + $v) }
        else { $Result['headset'] = $null; Log 'headset: none ready' }
    } else { Log 'adb: none on this PC yet (Setup downloads it)' }
    Finish 0
}

# Starts the app on the headset.
function Do-Start {
    Banner 'Start'
    Get-Adb
    if (-not (Wait-Headset)) { Log 'No headset; nothing was started.'; Finish 3 }
    $r = Run-Adb @('shell', 'am', 'start', '-n', $Activity)
    Log ('started: ' + $r.Out)
    Say '  Started. Put the headset on.' 'Green'
    Finish 0
}

# --- the Steam Frame (the steam-frame kit) ----------------------------------------------------

$FrameKitFile = Join-Path $QuestHere 'frame\frame.ps1'
if (Test-Path -LiteralPath $FrameKitFile) { . $FrameKitFile }
elseif ($SteamFrame) { throw ('this release has no Steam Frame files (' + $FrameKitFile + ')') }

# Reached, paired and read, or the player's next step and exit 3.
function Frame-Reach {
    if (Frame-Connect) { Say ''; Say '  Steam Frame connected.' 'Green'; return }
    Frame-Help
    $Result['result'] = 'no headset'
    Finish 3
}

# Settings and saves onto this PC: never while the game runs (it would lose them).
function Frame-KeepSaves([string] $why) {
    if (Frame-GameRunning) { throw ('the game is running on the Frame. Quit it there, then run this again.') }
    $to = Join-Path $FrameSaveRoot ((Get-Date -Format 'yyyy-MM-dd HHmm') + $why)
    $n = Frame-Backup ($FrameFiles + '/game') $SaveItems $to
    if ($n) { Log ('  ' + $n + ' settings and save files copied to ' + $to) }
    return $n
}

function Files-Under([string] $dir) {
    $dir = (Resolve-Path -LiteralPath $dir).ProviderPath.TrimEnd('\')
    $map = @{}
    foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File) { $map[$f.FullName.Substring($dir.Length + 1).Replace('\', '/')] = $f }
    return $map
}

function Do-FrameInstall {
    Banner 'Setup'
    if (-not (Test-Path -LiteralPath $Apk)) { throw ('the app is missing from this folder: ' + $Apk) }

    # 1. The game folder, as for a Quest.
    $g = Get-Game

    # 2. The Frame: found, paired once, read; room for it all.
    Frame-Reach
    Frame-Ready ((Get-Item -LiteralPath $Apk).Length + $g.Total)

    # 3. Settings and saves: a copy on this PC first, every time.
    Frame-KeepSaves '' | Out-Null

    # 4. Our files: the app, the library scripts, the art.
    Say ''
    Say ('  Copying ' + $Product + ' ' + $Version + ' to the Frame...')
    $c = Frame-CopyOurs $Apk (Join-Path $QuestHere 'frame')

    # 5. The games into the app's own folder; what it starts with; the settings the first
    #    time only (the player's own after that).
    $remoteGame = $FrameFiles + '/game'
    Frame-SyncFiles $g.Files $g.Dir $remoteGame 'the games' | Out-Null
    if ($g.Launch) {
        $lf = Write-Temp 'launch.txt' $g.Launch
        if (-not (Scp-To $lf ($remoteGame + '/launch.txt'))) { throw 'copying launch.txt to the Frame failed' }
        Remove-Item -LiteralPath $lf -Force
    }
    $ini = Join-Path $g.Dir 'doomvr.ini'
    if (Test-Path -LiteralPath $ini) {
        if (Frame-SyncFiles @{ 'doomvr.ini' = (Get-Item -LiteralPath $ini) } $g.Dir $remoteGame 'the settings' @('doomvr.ini')) { Log '  settings: the PC''s put in place' }
    }

    # 6. The saves back, if the Frame has none now: the newest copy Setup kept.
    $has = (Ssh ('ls "' + $remoteGame + '/saves" 2>/dev/null | head -1') 30).Out.Trim()
    $dirs = @(Get-ChildItem -LiteralPath $FrameSaveRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    if (-not $has -and $dirs) {
        $saved = Files-Under $dirs[0].FullName
        if ($saved.Count) {
            $n = Frame-SyncFiles $saved $dirs[0].FullName $remoteGame 'your settings and saves' @($saved.Keys)
            if ($n) { Log ('  ' + $n + ' settings and save files put back (from ' + $dirs[0].Name + ')') }
        }
    }

    # 7. The game in the Frame's own library; its own refresh rate.
    Frame-EnsureEntry $c.ArtChanged
    Frame-SetRefresh $RefreshHz

    Say ''
    Log 'Done.'
    Say ''
    Say ('  On the Frame: Library, then ' + $AppName + '.') 'Green'
    if (Test-Path -LiteralPath $FrameSaveRoot) { Say ('  Settings and saves are copied to ' + $FrameSaveRoot + ' each time Setup runs.') }
    Finish 0
}

function Do-FrameUninstall {
    Banner 'Uninstall'
    Frame-Reach
    $kept = Frame-KeepSaves ''
    Frame-Remove
    if ($kept) { Say ('  Your settings and saved games are kept in ' + $FrameSaveRoot + '.') }
    Say '  The game''s own folder on the Frame (its saves and settings) stays, so a new install picks them up.'
    Log 'Done.'
    Finish 0
}

function Do-FrameCollect {
    Banner 'Collect logs'
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('doom-vr-frame-logs-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    foreach ($f in @((Join-Path (Split-Path -Parent $QuestHere) 'setup-log.txt'), (Join-Path ([System.IO.Path]::GetTempPath()) 'doom-vr-frame-uninstall.txt'))) {
        if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $tmp }
    }
    if (Frame-Connect) {
        $to = Join-Path $tmp 'frame'
        New-Item -ItemType Directory -Force -Path (Join-Path $to 'logs') | Out-Null
        $remoteGame = $FrameFiles + '/game'
        foreach ($n in @('log-doomvr.txt', 'log-doomvr.prev.txt', 'log-doomvr-shutdown.txt', 'log-doomvr-shutdown.prev.txt', 'log-hands-report.txt', 'log-setup.txt', 'doomvr.ini', 'launch.txt', 'engine-version.txt')) {
            Scp-From ($remoteGame + '/' + $n) (Join-Path $to $n) | Out-Null
        }
        $logs = @(((Ssh ('ls -t "' + $remoteGame + '/logs" 2>/dev/null | head -6') 30).Out -split "`r?`n") | Where-Object { $_.Trim() })
        foreach ($n in $logs) { Scp-From ($remoteGame + '/logs/' + $n.Trim()) (Join-Path (Join-Path $to 'logs') $n.Trim()) | Out-Null }
        Scp-From ($FrameDir + '/library.log') (Join-Path $to 'library.log') | Out-Null
        $n = Frame-LeptonLogs (Join-Path $to 'lepton')
        if ($n) { Log ('  ' + $n + ' Lepton log files') }
        Set-Content -LiteralPath (Join-Path $to 'files.txt') -Value ((Ssh ('ls -lR "' + $FrameFiles + '" 2>&1 | grep -v "\.\(wad\|pk3\|png\|jpg\|ogg\|flac\|mp3\|lmp\)$" | head -600; ls -l "' + $FrameDir + '" 2>&1') 60).Out) -Encoding UTF8
        $Result['frame'] = [ordered]@{ address = $FrameAddress; appid = $FrameAppId; files = $FrameFiles }
    } else {
        Log 'The Steam Frame could not be reached: only this PC''s logs are collected.'
    }
    Copy-Item -LiteralPath $LogFile -Destination $tmp
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ($ProductFile + ' Steam Frame logs.zip')
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip
    Remove-Item -LiteralPath $tmp -Recurse -Force
    $Result['zip'] = $zip
    Say ''
    Say ('  Logs saved to ' + $zip) 'Green'
    Say '  Send that file to us (Discord or GitHub).'
    if (-not $NoPrompt) { try { Start-Process explorer.exe ('/select,"' + $zip + '"') } catch { } }
    Finish 0
}

function Do-FrameStart {
    Banner 'Start'
    Frame-Reach
    if (-not (Frame-Start)) { throw 'Steam on the Frame did not start the game. Start it from the Frame''s library.' }
    Say '  Started. Put the Frame on.' 'Green'
    Finish 0
}

function Do-FrameDetect {
    $game = Find-PcGame
    $Result['pcGame'] = $game
    $Result['staged'] = Test-Path -LiteralPath (Join-Path $Stage 'game\launchers')
    $Result['frame'] = Frame-Detect
    Finish 0
}

if ($SteamFrame) {
    switch ($Mode) {
        'start'     { Do-FrameStart }
        'uninstall' { Do-FrameUninstall }
        'collect'   { Do-FrameCollect }
        'detect'    { Do-FrameDetect }
        default     { Do-FrameInstall }
    }
}
switch ($Mode) {
    'start'     { Do-Start }
    'uninstall' { Do-Uninstall }
    'collect'   { Do-Collect }
    'detect'    { Do-Detect }
    default     { Do-Install }
}
