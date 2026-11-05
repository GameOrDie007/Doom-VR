# Doom VR: install, uninstall and log collection, in one script.
#
# Doom VR plays the Doom-family games you own - Doom, Doom II, Final Doom,
# DOOM + DOOM II (2024) with Legacy of Rust, Heretic, Hexen, Strife, Chex
# Quest and more - in VR. It makes a VR folder inside your DOOM + DOOM II
# folder (Steam, GOG or anywhere it is), or, without that game, in
# Documents\Doom VR (or %USERPROFILE%\Games\Doom VR when OneDrive keeps
# Documents). Then it looks for every Doom-family game on this PC (every
# Steam library, GOG, and any folder you drag in), copies their game files
# into the VR folder, fetches the free games and mods from where their
# authors publish them, and writes a launcher for each. No game data ships
# with this download.
#
#   Setup.bat           install (or update, or find games bought since): this
#                       script with no switch
#   Uninstall.bat       -Uninstall: removes everything it installed; your
#                       settings are kept (saved games are in Windows' Saved
#                       Games folder and are never touched)
#   Collect logs.bat    -Collect: zips the logs onto your desktop to send us
#   Reset settings.cmd  -ResetSettings: Doom VR's settings back to how they
#                       came (doomvr.ini set aside, dated); saved games, your
#                       weapon and hand fits and the Game Tome's notes stay
#
# Safe to run again at any time: it never overwrites your settings.
#
#   -NoDownload         never use the network (no free games or mods)
#
# For tools and hubs (the same switches in every Game Or Die port):
#   -Quiet              no questions, no pause, no Explorer window
#   -GamePath <folder>  install for this DOOM + DOOM II folder (and look for
#                       games there); several separated by ';'; no searching
#   -Json               print the result as JSON on stdout (nothing else)
#   -Detect             change nothing; report the game and the install
#   -AddToSteam         with -Quiet: add the Steam library entry
#   -CloseSteam         with -Quiet: close Steam to write it (restarted after)
#   -NoSteam, -NoShortcuts   skip the Steam entry / the desktop shortcut
# Exit codes: 0 done, 1 nothing could be installed, 2 error.
#
# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding(PositionalBinding = $false)]
param(
    [switch] $Uninstall,
    [switch] $Collect,
    [switch] $ResetSettings,
    [switch] $Detect,
    [switch] $Quiet,
    [switch] $Json,
    [string[]] $GamePath,
    [switch] $AddToSteam,
    [switch] $CloseSteam,
    [switch] $NoSteam,
    [switch] $NoShortcuts,
    [switch] $NoDownload,
    # Test builds only: the mirrored mods from this place instead of Doom VR's
    # GitHub release (a file:/// folder), passed on to the game search.
    [string] $Mirror,
    # Tests: no questions; only the folders given; a stand-in Steam folder;
    # a stand-in Documents folder; a stand-in user folder (%USERPROFILE%).
    [switch] $NoPrompt,
    [switch] $NoSearch,
    [string] $SteamRoot,
    [string] $DocumentsRoot,
    [string] $UserRoot,
    # Folders dropped on Setup.bat arrive as bare arguments; collected here,
    # so none is bound to a switch by position.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Dropped
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Product = 'Doom VR'
$ProductFile = 'Doom VR'
$Version = '1.0'
$Title = 'Doom VR'
# GameExe, not Exe: PowerShell names are not case-sensitive, and a function's
# local $exe would hide it.
$GameExe = 'uzdoom.exe'
$Launcher = 'Doom VR.cmd'
$Flat = 'Doom VR (flat).cmd'
$FindGames = 'Find my new games.cmd'
$ResetCmd = 'Reset settings.cmd'
$GetLook = 'Get the Modern VR look.cmd'
$IconFile = 'doom-vr.ico'	# Game Or Die's skull, for the shortcut and the Steam entry
$ShortcutName = 'Doom VR.lnk'
# Said before the first install, here and by tools and hubs (-Detect -Json).
$InstallNote = 'The first install downloads about 5.5 GB of free games and mods: about 6 minutes on a fast connection, longer on a slow one.'

if ($Quiet -or $Json) { $NoPrompt = $true }
if ($GamePath) { $NoSearch = $true; $Dropped = @($Dropped) + @($GamePath | ForEach-Object { $_ -split ';' } | Where-Object { $_ }) }

# ProviderPath, not Path: on a network share .Path carries a PowerShell
# provider prefix that .NET file calls refuse.
$Here = (Resolve-Path -LiteralPath $PSScriptRoot).ProviderPath

# A mirror.txt beside this script names another place for the mirrored mods
# (a folder or a URL), used when it is reachable. Release zips never carry one.
if (-not $Mirror -and (Test-Path -LiteralPath (Join-Path $Here 'mirror.txt'))) {
    $m = @(Get-Content -LiteralPath (Join-Path $Here 'mirror.txt') | Where-Object { $_.Trim() })[0]
    if ($m) { $m = $m.Trim() }
    if ($m -and ($m -match '^https?://' -or (Test-Path -LiteralPath $m -PathType Container))) { $Mirror = $m }
}
# Run from the unzipped download, Doom VR sits in files\game beside this
# script; run from an installed VR folder (Uninstall, Collect logs), this
# script sits in VR\tools.
$Package = $null
if (Test-Path -LiteralPath (Join-Path $Here ('game\' + $GameExe))) { $Package = $Here }

# The player's own, in VR\game: never removed, never overwritten. Saved games
# and screenshots are not in the install at all (Saved Games, Pictures).
# The player's own, never replaced or uninstalled: settings, fits, holds, and the
# Game Manual's notes and the page it was closed on.
$PlayerFiles = @('doomvr.ini', 'doomvr-weapons.txt', 'doomvr-holds.txt', 'gametome\tome\ink.txt', 'gametome\tome\place.txt')

# --- the log --------------------------------------------------------------------

$Mode = 'install'
if ($Uninstall) { $Mode = 'uninstall' }
if ($Collect)   { $Mode = 'collect' }
if ($ResetSettings) { $Mode = 'reset' }
if ($Detect)    { $Mode = 'detect' }
$LogFile = Join-Path ([System.IO.Path]::GetTempPath()) ('doomvr-' + $Mode + '.txt')
if ($Mode -eq 'install' -and $Package) { $LogFile = Join-Path (Split-Path -Parent $Here) 'setup-log.txt' }

$Result = [ordered]@{ product = $Product; version = $Version; mode = $Mode; result = 'ok' }

function Say([string] $m, [string] $color) {
    if ($Json) { return }	# stdout carries the JSON alone
    if ($color) { Write-Host $m -ForegroundColor $color } else { Write-Host $m }
}
function Log([string] $m) {
    # Line by line from the start, so a failure part-way still leaves a log.
    Add-Content -LiteralPath $LogFile -Value $m -Encoding UTF8
    Say $m
}
try { Set-Content -LiteralPath $LogFile -Value ($Product + ' ' + $Version + ' ' + $Mode + ', ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' on ' + $env:COMPUTERNAME) -Encoding UTF8 }
catch { $LogFile = Join-Path ([System.IO.Path]::GetTempPath()) ('doomvr-' + $Mode + '.txt'); Set-Content -LiteralPath $LogFile -Value ($Product + ' ' + $Mode) -Encoding UTF8 }

function Finish([int] $code) {
    if ($Json) {
        $Result['exitCode'] = $code
        $Result['game'] = Game-Status
        Write-Output ($Result | ConvertTo-Json -Depth 5)
    }
    exit $code
}
trap {
    Log ''
    Log ('ERROR: ' + $_.Exception.Message)
    Log ('  at ' + $_.InvocationInfo.PositionMessage)
    Log ''
    Log 'Setup stopped.  Run "Collect logs.bat" and send us the file it makes.'
    $Result['result'] = 'error'
    $Result['error'] = $_.Exception.Message
    if ($Json) { $Result['exitCode'] = 2; Write-Output ($Result | ConvertTo-Json -Depth 5) }
    exit 2
}

function Banner([string] $what) {
    Say ''
    Say ('  ' + $Product + ' ' + $Version + ': ' + $what) 'Cyan'
    Say ('  ' + ('-' * ($Product.Length + $Version.Length + $what.Length + 3)))
    Say ''
}

function Ask-YesNo([string] $q) {
    # Yes unless the player says no; no questions at all under -Quiet.
    if ($NoPrompt) { return $false }
    try { if ([Console]::IsInputRedirected) { return $false } } catch { }
    $a = Read-Host ('  ' + $q + ' [Y/n]')
    if ($null -eq $a) { return $false }
    return -not ($a.Trim() -match '^(n|no)$')
}

function Documents {
    if ($DocumentsRoot) { return $DocumentsRoot }
    return [Environment]::GetFolderPath('MyDocuments')
}
# Where the VR folder goes when the game's folder will not do: Documents,
# unless OneDrive keeps it (the default on many PCs), then the game files
# would all be uploaded, and a file OneDrive is busy with can stall the game.
# %USERPROFILE%\Games instead.
function VR-Home {
    $d = Documents
    $user = $env:USERPROFILE
    if ($UserRoot) { $user = $UserRoot }
    $synced = $d -match '(^|\\)OneDrive( - [^\\]*)?(\\|$)'
    foreach ($od in @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial)) {
        if ($od -and -not $DocumentsRoot -and $d.StartsWith($od.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { $synced = $true }
    }
    if ($synced) { return (Join-Path $user 'Games') }
    return $d
}
function Desktop { return [Environment]::GetFolderPath('Desktop') }

# --- finding DOOM + DOOM II -------------------------------------------------------
#
# The VR folder's home. Every other Doom-family game is found by DoomVR's own
# game search (game\setup\setup.ps1), which also copies their files in.

function True-Case([string] $p) {
    # The folder's name as it is on disk: Steam reports its path in lower case.
    try {
        $d = New-Object System.IO.DirectoryInfo $p
        if (-not $d.Exists) { return $p }
        if (-not $d.Parent) { return $d.FullName.ToUpper() }
        $match = $d.Parent.GetDirectories($d.Name) | Select-Object -First 1
        if (-not $match) { return $p }
        return (Join-Path (True-Case $d.Parent.FullName) $match.Name)
    } catch { return $p }
}

# A path joined without asking the drive: Join-Path stops the whole search
# when a Steam library is on a drive that is no longer there (an unplugged
# USB drive stays in libraryfolders.vdf); Test-Path on the result is False.
function Combine-Path([string]$a, [string]$b) {
    try { return [System.IO.Path]::Combine($a, $b) } catch { return "" }
}

$Roots = New-Object System.Collections.Generic.List[string]
function Add-Root([string] $p) {
    if (-not $p) { return }
    try { $p = [System.IO.Path]::GetFullPath($p.Trim().Trim('"').TrimEnd('\')) } catch { return }
    $p = True-Case $p
    foreach ($r in $Roots) { if ($r -ieq $p) { return } }
    if (Test-Path -LiteralPath $p -PathType Container) { $Roots.Add($p) }
}

function Steam-Dir {
    if ($SteamRoot) { return $SteamRoot }
    foreach ($k in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try {
            $v = Get-ItemProperty -LiteralPath $k -ErrorAction Stop
            foreach ($p in @($v.SteamPath, $v.InstallPath)) {
                if ($p -and (Test-Path -LiteralPath ($p -replace '/', '\'))) { return (True-Case ($p -replace '/', '\')) }
            }
        } catch { }
    }
    return $null
}

$Searched = $false
function Search-Roots {
    if ($Searched) { return }
    $script:Searched = $true
    foreach ($d in $Dropped) { if ($d) { Add-Root ($d.Trim('"')) } }
    if ($NoSearch) { return }

    # 1-2. Steam, from its own registry entry (and -SteamRoot's stand-in, in a
    # test), and every library it lists.
    $steamDirs = @()
    if ($SteamRoot) { $steamDirs += $SteamRoot }
    foreach ($k in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try {
            $v = Get-ItemProperty -LiteralPath $k -ErrorAction Stop
            if ($v.SteamPath)   { $steamDirs += $v.SteamPath }
            if ($v.InstallPath) { $steamDirs += $v.InstallPath }
        } catch { }
    }
    $libraries = @()
    foreach ($s in $steamDirs) {
        $s = $s -replace '/', '\'
        $libraries += $s
        foreach ($vdf in @((Combine-Path $s 'steamapps\libraryfolders.vdf'), (Combine-Path $s 'config\libraryfolders.vdf'))) {
            if (-not $vdf -or -not (Test-Path -LiteralPath $vdf)) { continue }
            foreach ($line in Get-Content -LiteralPath $vdf) {
                if ($line -match '"path"\s+"([^"]+)"') { $libraries += ($Matches[1] -replace '\\\\', '\') }
            }
        }
    }
    # DOOM + DOOM II (2024) is Steam's "Ultimate Doom" app folder.
    foreach ($lib in $libraries) { Add-Root (Combine-Path $lib 'steamapps\common\Ultimate Doom') }

    # 3. GOG registers each game on its own.
    foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games')) {
        try {
            foreach ($e in Get-ChildItem -LiteralPath $k -ErrorAction Stop) {
                $p = Get-ItemProperty -LiteralPath $e.PSPath
                if ($p.gameName -match 'DOOM' -and $p.path) { Add-Root $p.path }
            }
        } catch { }
    }

    # 4. The Windows list of installed programs: other stores.
    foreach ($k in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                     'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall')) {
        try {
            foreach ($e in Get-ChildItem -LiteralPath $k -ErrorAction Stop) {
                $p = Get-ItemProperty -LiteralPath $e.PSPath
                if ($p.DisplayName -match '^(Ultimate )?DOOM( \+ DOOM II)?$' -and $p.InstallLocation) { Add-Root $p.InstallLocation }
            }
        } catch { }
    }

    # 5. The usual folders on every drive, last.
    $drives = @()
    try { $drives = [System.IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady } | ForEach-Object { $_.RootDirectory.FullName } } catch { $drives = @('C:\') }
    foreach ($dr in $drives) {
        foreach ($f in @('DOOM + DOOM II', 'Ultimate Doom')) {
            Add-Root (Join-Path $dr ('GOG Games\' + $f))
            Add-Root (Join-Path $dr ('GOG Galaxy\Games\' + $f))
            Add-Root (Join-Path $dr ('Games\' + $f))
        }
    }
}

# DOOM + DOOM II (2024)'s folder holds rerelease\DOOM2.WAD (Steam) or DOOM2.WAD
# beside its exe (GOG).
function Find-Doom {
    foreach ($r in $Roots) {
        if ((Test-Path -LiteralPath (Join-Path $r 'rerelease\DOOM2.WAD')) -or (Test-Path -LiteralPath (Join-Path $r 'rerelease\doom2.wad'))) { return $r }
    }
    foreach ($r in $Roots) {
        if ((Test-Path -LiteralPath (Join-Path $r 'DOOM2.WAD')) -and (Test-Path -LiteralPath (Join-Path $r 'id1.wad'))) { return $r }
    }
    return $null
}

# A folder the player chose that holds the original games' WADs, loose or in
# folders of its own: no DOOM + DOOM II there, so Doom VR goes in Documents, and
# the game search (game\setup\setup.ps1, to the same depth) copies them from it.
function Find-OwnWads {
    $names = 'doom.wad', 'doom2.wad', 'tnt.wad', 'plutonia.wad', 'heretic.wad', 'hexen.wad', 'strife1.wad', 'chex3.wad', 'hacx.wad'
    foreach ($d in $Dropped) {
        if (-not $d) { continue }
        $p = $d.Trim('"')
        if (-not (Test-Path -LiteralPath $p -PathType Container)) { continue }
        $hit = Get-ChildItem -LiteralPath $p -File -Recurse -Depth 3 -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.Name.ToLower() } | Select-Object -First 1
        if ($hit) { return $p }
    }
    return $null
}

# Every VR folder of ours this PC has, from their install manifests.
function Installed-VRFolders {
    $list = New-Object System.Collections.Generic.List[string]
    $add = {
        param($p)
        if (-not $p) { return }
        foreach ($x in $list) { if ($x -ieq $p) { return } }
        if (Test-Path -LiteralPath (Join-Path $p 'install-manifest.txt')) { $list.Add($p) }
    }
    if ((Split-Path -Leaf $Here) -ieq 'tools') { & $add (Split-Path -Parent $Here) }
    $doom = Find-Doom
    if ($doom) { & $add (Join-Path $doom 'VR') }
    & $add (Join-Path (VR-Home) $ProductFile)
    & $add (Join-Path (Documents) $ProductFile)
    # Test installs from before the name had its space.
    & $add (Join-Path (VR-Home) 'DoomVR')
    & $add (Join-Path (Documents) 'DoomVR')
    return $list
}

# --- the Steam library ---------------------------------------------------------------
#
# Steam keeps the games added with "Add a Non-Steam Game" in a binary file,
# userdata\<account>\config\shortcuts.vdf, one per Steam account on this PC.
# It reads it when it starts and writes it when it exits, so it must not be
# running while this one is changed. Every other entry is kept byte for byte.

function Vdf-ReadString([byte[]] $b, [ref] $i) {
    $start = $i.Value
    while ($b[$i.Value] -ne 0) { $i.Value++ }
    $s = [System.Text.Encoding]::UTF8.GetString($b, $start, $i.Value - $start)
    $i.Value++
    return $s
}
function Vdf-ReadMap([byte[]] $b, [ref] $i) {
    $entries = New-Object System.Collections.ArrayList
    while ($true) {
        if ($i.Value -ge $b.Length) { throw 'shortcuts.vdf ends early' }
        $t = $b[$i.Value]; $i.Value++
        if ($t -eq 8) { return ,$entries }
        $k = Vdf-ReadString $b $i
        switch ($t) {
            0 { $v = Vdf-ReadMap $b $i }
            1 { $v = Vdf-ReadString $b $i }
            2 { $v = [BitConverter]::ToInt32($b, $i.Value); $i.Value += 4 }
            7 { $v = [BitConverter]::ToUInt64($b, $i.Value); $i.Value += 8 }
            default { throw ('shortcuts.vdf has a value type this does not know (' + $t + '); left alone') }
        }
        [void]$entries.Add(@{ T = [int]$t; K = $k; V = $v })
    }
}
function Vdf-Write($entries, [System.IO.MemoryStream] $ms) {
    foreach ($e in $entries) {
        $ms.WriteByte([byte]$e.T)
        $kb = [System.Text.Encoding]::UTF8.GetBytes($e.K); $ms.Write($kb, 0, $kb.Length); $ms.WriteByte(0)
        switch ($e.T) {
            0 { Vdf-Write $e.V $ms; $ms.WriteByte(8) }
            1 { $vb = [System.Text.Encoding]::UTF8.GetBytes([string]$e.V); $ms.Write($vb, 0, $vb.Length); $ms.WriteByte(0) }
            2 { $vb = [BitConverter]::GetBytes([int32]$e.V); $ms.Write($vb, 0, 4) }
            7 { $vb = [BitConverter]::GetBytes([uint64]$e.V); $ms.Write($vb, 0, 8) }
        }
    }
}
function Vdf-Get($entries, [string] $k) {
    foreach ($e in $entries) { if ($e.K -ieq $k) { return ,$e.V } }
    return $null
}

# Steam's id for a non-Steam game: CRC-32 of the quoted exe and the name, top
# bit set. It names the library art files. (PowerShell 5.1 reads 0xFFFFFFFF
# and friends as negative int32s: the constants are written out.)
$Crc = $null
$Poly = [uint32]3988292384	# 0xEDB88320
$TopBit = [uint32]2147483648	# 0x80000000
function Crc32([string] $s) {
    if (-not $script:Crc) {
        $script:Crc = New-Object 'uint32[]' 256
        for ($n = 0; $n -lt 256; $n++) {
            $c = [uint32]$n
            for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = [uint32]($Poly -bxor ($c -shr 1)) } else { $c = [uint32]($c -shr 1) } }
            $script:Crc[$n] = $c
        }
    }
    $c = [uint32]::MaxValue
    foreach ($byte in [System.Text.Encoding]::UTF8.GetBytes($s)) { $c = [uint32]($script:Crc[($c -bxor $byte) -band 0xFF] -bxor ($c -shr 8)) }
    return [uint32]($c -bxor [uint32]::MaxValue)
}
function Shortcut-Id([string] $exeQuoted, [string] $name) { return [uint32]((Crc32 ($exeQuoted + $name)) -bor $TopBit) }

function Steam-Configs {
    $s = Steam-Dir
    if (-not $s) { return @() }
    $u = Join-Path $s 'userdata'
    if (-not (Test-Path -LiteralPath $u)) { return @() }
    return @(Get-ChildItem -LiteralPath $u -Directory | Where-Object { $_.Name -match '^\d+$' -and $_.Name -ne '0' } |
        ForEach-Object { Join-Path $_.FullName 'config' } | Where-Object { Test-Path -LiteralPath $_ })
}

function Steam-Running {
    if ($SteamRoot) { return $false }	# a stand-in folder in a test
    return [bool](Get-Process -Name 'steam' -ErrorAction SilentlyContinue)
}

$SteamWasClosed = $false
function Steam-Close {
    # Steam's own way out, so it writes its files before it goes.
    $exe = Join-Path (Steam-Dir) 'steam.exe'
    Log '  Closing Steam...'
    Start-Process -FilePath $exe -ArgumentList '-shutdown'
    for ($n = 0; $n -lt 60 -and (Steam-Running); $n++) { Start-Sleep -Milliseconds 500 }
    if (Steam-Running) { Log '  Steam did not close.'; return $false }
    $script:SteamWasClosed = $true
    return $true
}
function Steam-Reopen {
    if (-not $SteamWasClosed) { return }
    Start-Process -FilePath (Join-Path (Steam-Dir) 'steam.exe')
    Log '  Steam started again.'
}

# Our entry: named as ours AND starting the game in a VR folder of ours.
function Is-Ours($entry) {
    $name = Vdf-Get $entry.V 'AppName'
    $exe = Vdf-Get $entry.V 'Exe'
    return (($name -eq $Title -or $name -eq 'DoomVR') -and ($exe -like ('*\game\' + $GameExe + '*')))
}

function Read-Shortcuts([string] $file) {
    if (-not (Test-Path -LiteralPath $file)) {
        $root = New-Object System.Collections.ArrayList
        [void]$root.Add(@{ T = 0; K = 'shortcuts'; V = (New-Object System.Collections.ArrayList) })
        return ,$root
    }
    $b = [System.IO.File]::ReadAllBytes($file)
    $i = 0
    $root = Vdf-ReadMap $b ([ref]$i)
    if (-not ($root | Where-Object { $_.K -ieq 'shortcuts' -and $_.T -eq 0 })) { throw ('no shortcuts list in ' + $file + '; left alone') }
    return ,$root
}
function Write-Shortcuts([string] $file, $root) {
    $ms = New-Object System.IO.MemoryStream
    Vdf-Write $root $ms
    $ms.WriteByte(8)
    $backup = $file + '.before-doomvr'
    if ((Test-Path -LiteralPath $file) -and -not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $file -Destination $backup }
    [System.IO.File]::WriteAllBytes($file, $ms.ToArray())
}
function Renumber($list) {
    for ($n = 0; $n -lt $list.Count; $n++) { $list[$n].K = [string]$n }
}

function New-ShortcutEntry([string] $vr, [string] $gameArgs) {
    # The game itself, not a launcher: no console window, and Steam sees it
    # running (playtime, overlay).
    $exe = '"' + (Join-Path $vr ('game\' + $GameExe)) + '"'
    $id = Shortcut-Id $exe $Title
    $v = New-Object System.Collections.ArrayList
    foreach ($f in @(
        @{ T = 2; K = 'appid'; V = [BitConverter]::ToInt32([BitConverter]::GetBytes($id), 0) },
        @{ T = 1; K = 'AppName'; V = $Title },
        @{ T = 1; K = 'Exe'; V = $exe },
        @{ T = 1; K = 'StartDir'; V = ('"' + (Join-Path $vr 'game') + '\"') },
        @{ T = 1; K = 'icon'; V = $(if (Test-Path -LiteralPath (Join-Path $vr ('tools\' + $IconFile))) { Join-Path $vr ('tools\' + $IconFile) } else { Join-Path $vr ('game\' + $GameExe) }) },
        @{ T = 1; K = 'ShortcutPath'; V = '' },
        @{ T = 1; K = 'LaunchOptions'; V = $gameArgs },
        @{ T = 2; K = 'IsHidden'; V = 0 },
        @{ T = 2; K = 'AllowDesktopConfig'; V = 1 },
        @{ T = 2; K = 'AllowOverlay'; V = 1 },
        @{ T = 2; K = 'OpenVR'; V = 1 },	# a VR game: listed in SteamVR's library too
        @{ T = 2; K = 'Devkit'; V = 0 },
        @{ T = 1; K = 'DevkitGameID'; V = '' },
        @{ T = 2; K = 'DevkitOverrideAppID'; V = 0 },
        @{ T = 2; K = 'LastPlayTime'; V = 0 },
        @{ T = 1; K = 'FlatpakAppID'; V = '' },
        @{ T = 0; K = 'tags'; V = (New-Object System.Collections.ArrayList) })) { [void]$v.Add($f) }
    return @{ Entry = @{ T = 0; K = '0'; V = $v }; Id = $id }
}

function Steam-Art([string] $config, [uint32] $id, [bool] $add, [string] $vr) {
    $grid = Join-Path $config 'grid'
    $files = @(($id.ToString() + '_hero.png'), ($id.ToString() + '.png'))
    if ($add) {
        # The splash the game shows: the player's own, else ours, else the one made from title screens.
        $splash = $null
        foreach ($c in @('game\boxart\graphics\vrsplash.png', 'game\setup\vrsplash.png', 'game\boxart\auto\vrsplash.png')) {
            if (-not $splash -and (Test-Path -LiteralPath (Join-Path $vr $c))) { $splash = Join-Path $vr $c }
        }
        if (-not $splash) { return }
        New-Item -ItemType Directory -Force -Path $grid | Out-Null
        foreach ($f in $files) { Copy-Item -LiteralPath $splash -Destination (Join-Path $grid $f) -Force }
    } else {
        foreach ($f in $files) { $p = Join-Path $grid $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
    }
}

function Steam-Add([string] $vr, [string] $gameArgs) {
    $configs = Steam-Configs
    if ($configs.Count -eq 0) { return 'no Steam' }
    foreach ($c in $configs) {
        $file = Join-Path $c 'shortcuts.vdf'
        $root = Read-Shortcuts $file
        $list = Vdf-Get $root 'shortcuts'
        foreach ($e in @($list)) { if (Is-Ours $e) { [void]$list.Remove($e) } }
        $new = New-ShortcutEntry $vr $gameArgs
        [void]$list.Add($new.Entry)
        Steam-Art $c $new.Id $true $vr
        Renumber $list
        Write-Shortcuts $file $root
    }
    return 'added'
}

function Steam-HasOurs {
    foreach ($c in Steam-Configs) {
        $file = Join-Path $c 'shortcuts.vdf'
        if (-not (Test-Path -LiteralPath $file)) { continue }
        try { foreach ($e in (Vdf-Get (Read-Shortcuts $file) 'shortcuts')) { if (Is-Ours $e) { return $true } } } catch { }
    }
    return $false
}

function Steam-Remove {
    foreach ($c in Steam-Configs) {
        $file = Join-Path $c 'shortcuts.vdf'
        if (-not (Test-Path -LiteralPath $file)) { continue }
        $root = Read-Shortcuts $file
        $list = Vdf-Get $root 'shortcuts'
        $removed = 0
        foreach ($e in @($list)) {
            if (Is-Ours $e) {
                Steam-Art $c ([uint32][BitConverter]::ToUInt32([BitConverter]::GetBytes([int32](Vdf-Get $e.V 'appid')), 0)) $false ''
                [void]$list.Remove($e); $removed++
            }
        }
        if ($removed) { Renumber $list; Write-Shortcuts $file $root }
    }
}

function Steam-Ready([bool] $quietAllowed) {
    if (-not (Steam-Running)) { return $true }
    if ($NoPrompt) {
        if ($quietAllowed -and $CloseSteam) { return (Steam-Close) }
        return $false
    }
    if (Ask-YesNo 'Steam is open, and it has to be closed for this.  Close Steam now (it opens again after)?') { return (Steam-Close) }
    return $false
}

# --- install ---------------------------------------------------------------------

# How DoomVR.bat starts the game, for the desktop shortcut and the Steam entry,
# which start the exe itself (no console window): the launcher it calls, its
# uzdoom.exe line, with the launcher's own folder (%~dp0..) as the game folder.
function Start-Args([string] $game) {
    $top = Join-Path $game 'DoomVR.bat'
    if (-not (Test-Path -LiteralPath $top)) { return $null }
    $call = Get-Content -LiteralPath $top | Where-Object { $_ -match '^call "%~dp0(launchers\\[^"]+\.bat)"' } | Select-Object -First 1
    if (-not $call) { return $null }
    $null = $call -match '^call "%~dp0(launchers\\[^"]+\.bat)"'
    $bat = Join-Path $game $Matches[1]
    if (-not (Test-Path -LiteralPath $bat)) { return $null }
    $line = Get-Content -LiteralPath $bat | Where-Object { $_ -match '^\.\\uzdoom\.exe ' } | Select-Object -First 1
    if (-not $line) { return $null }
    $a = $line.Substring('.\uzdoom.exe '.Length)
    $a = $a.Replace('%~dp0..\', $game + '\')
    $a = ($a -replace '\s+-file\s+%\*\s*$', '').Trim()
    return $a
}

function Write-Cmd([string] $path, [string] $title, [string] $target) {
    $lines = @(
        '@echo off',
        ('rem ' + $title),
        ('rem Made by ' + $ProductFile + ' ' + $Version + ' Setup.'),
        'rem No parenthesised blocks below: a path with "(x86)" in it ends them early.',
        ('if not exist "%~dp0game\' + $target + '" goto missing'),
        ('call "%~dp0game\' + $target + '" %*'),
        'goto :eof',
        '',
        ':missing',
        ('echo "%~dp0game\' + $target + '" is missing.  Run Setup.bat again from the ' + $ProductFile + ' download.'),
        'pause',
        'exit /b 1'
    )
    Set-Content -LiteralPath $path -Value $lines -Encoding ASCII
}

function Make-Shortcut([string] $lnk, [string] $target, [string] $arguments, [string] $workdir, [string] $what, [string] $icon) {
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath = $target
    $s.Arguments = $arguments
    $s.WorkingDirectory = $workdir
    $s.IconLocation = $(if ($icon -and (Test-Path -LiteralPath $icon)) { $icon + ',0' } else { $target + ',0' })
    $s.Description = $what
    $s.WindowStyle = 1
    $s.Save()
}

function VR-Folder-For([string] $doom) {
    $candidates = @()
    if ($doom) { $candidates += (Join-Path $doom 'VR') }
    $candidates += (Join-Path (VR-Home) $ProductFile)
    foreach ($vr in $candidates) {
        try {
            New-Item -ItemType Directory -Force -Path (Join-Path $vr 'game') | Out-Null
            $probe = Join-Path $vr 'write-test.tmp'
            Set-Content -LiteralPath $probe -Value 'x'
            Remove-Item -LiteralPath $probe -Force
            return $vr
        } catch {
            # Under Program Files a game folder is not writable without admin:
            # the VR folder goes in Documents (or Games) instead.
            Log ('  ' + $vr + ' cannot be written to; trying ' + (VR-Home) + '.')
        }
    }
    throw 'no folder could be written to for the VR install'
}

# DoomVR's own game search and set-up (game\setup\setup.ps1): finds every
# Doom-family game, copies its files in, fetches the free games and mods,
# makes the art and writes the launchers. Only adds; re-run any time.
function Run-GameSetup([string] $game) {
    $engine = Join-Path $game 'setup\setup.ps1'
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $engine, '-Install', $game)
    if ($NoPrompt) { $args2 += '-Unattended' }
    if ($NoDownload) { $args2 += '-NoDownload' }
    if ($Mirror) { $args2 += @('-Mirror', $Mirror) }
    if ($NoSearch) { $args2 += @('-NoSteam', '-NoGog') }
    if ($SteamRoot) { $args2 += @('-SteamRoot', $SteamRoot) }
    foreach ($r in $Roots) { $args2 += $r }
    Log 'Finding your Doom-family games and downloading the free games and mods.'
    Log $InstallNote
    # Each line passed on as it comes, so a tool or hub showing our output
    # can say what is being downloaded right now.
    & powershell.exe @args2 2>&1 | ForEach-Object { Add-Content -LiteralPath $LogFile -Value ('  ' + [string]$_) -Encoding UTF8; Say ('  ' + [string]$_) }
    $code = $LASTEXITCODE
    return ($code -eq 0)
}

function Do-Install {
    Banner 'Setup'
    if (-not $Package) { throw 'Run Setup.bat from the folder you unzipped (it holds files\game).' }
    Search-Roots
    Log ('Looking in ' + $Roots.Count + ' place(s) for DOOM + DOOM II.')
    foreach ($r in $Roots) { Add-Content -LiteralPath $LogFile -Value ('  ' + $r) -Encoding UTF8 }
    $doom = Find-Doom
    if ($doom) { Log ('DOOM + DOOM II: ' + $doom) } else { Log 'DOOM + DOOM II: not found.  Doom VR plays any Doom-family game you own, and the free ones.' }
    $Result['doom'] = $doom

    $vr = VR-Folder-For $doom
    $game = Join-Path $vr 'game'
    Log ('Installing to ' + $vr)
    $note = Join-Path $vr 'Your settings are kept here.txt'
    if (Test-Path -LiteralPath $note) { Remove-Item -LiteralPath $note -Force }

    # Doom VR: replaced on every run (an update), except the player's own.
    $src = Join-Path $Package 'game'
    foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File) {
        $rel = $f.FullName.Substring($src.Length + 1)
        $dst = Join-Path $game $rel
        $isPlayer = $false
        foreach ($p in $PlayerFiles) { if ($rel -ieq $p) { $isPlayer = $true } }
        if ($isPlayer -and (Test-Path -LiteralPath $dst)) { continue }	# never overwrite settings
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
    }

    $gamesOk = Run-GameSetup $game
    $ready = Test-Path -LiteralPath (Join-Path $game 'DoomVR.bat')

    Write-Cmd (Join-Path $vr $Launcher) 'Doom VR: Doom in VR. Switch Game (main menu) has every other game.' 'DoomVR.bat'
    Write-Cmd (Join-Path $vr $Flat) 'Doom VR on the monitor.' 'DoomVR - flat screen.bat'
    # The Modern VR look's own mods (Voxel Doom, NashGore, upscaled textures)
    # are on sites that refuse scripted downloads: this opens their pages and
    # takes each file as it lands in Downloads.
    Set-Content -LiteralPath (Join-Path $vr $GetLook) -Encoding ASCII -Value @(
        '@echo off',
        'rem Opens the download pages of the Modern VR look (Voxel Doom, NashGore and its voxel',
        'rem gore, upscaled textures); click Download on each and they are added as they arrive.',
        ('rem Made by ' + $ProductFile + ' ' + $Version + ' Setup.'),
        'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0game\setup\setup.ps1" -Install "%~dp0game" -LookOnly',
        'pause')
    # A game bought later: this looks again and adds it (nothing is replaced).
    Set-Content -LiteralPath (Join-Path $vr $FindGames) -Encoding ASCII -Value @(
        '@echo off',
        'rem Looks for Doom-family games again (ones bought since) and adds them.',
        ('rem Made by ' + $ProductFile + ' ' + $Version + ' Setup.'),
        'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0game\setup\setup.ps1" -Install "%~dp0game"',
        'pause')
    # Settings back to how they came: the first thing to try when something is off.
    Set-Content -LiteralPath (Join-Path $vr $ResetCmd) -Encoding ASCII -Value @(
        '@echo off',
        'rem Puts Doom VR''s settings back to how they came (the old ones are kept beside them, dated).',
        'rem Saved games, your weapon and hand fits and the Game Tome''s notes are not touched.',
        ('rem Made by ' + $ProductFile + ' ' + $Version + ' Setup.'),
        'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -ResetSettings',
        'pause')

    # The kit, so Uninstall and Collect logs are there without the download.
    New-Item -ItemType Directory -Force -Path (Join-Path $vr 'tools') | Out-Null
    Copy-Item -LiteralPath (Join-Path $Here 'setup.ps1') -Destination (Join-Path $vr 'tools\setup.ps1') -Force
    $ico = Join-Path $Here $IconFile
    if (Test-Path -LiteralPath $ico) { Copy-Item -LiteralPath $ico -Destination (Join-Path $vr ('tools\' + $IconFile)) -Force }
    foreach ($k in @('Uninstall.bat', 'Collect logs.bat')) {
        $s = Join-Path $Here $k
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination (Join-Path $vr $k) -Force }
    }
    foreach ($k in @('READ ME FIRST.txt')) {
        $s = Join-Path (Split-Path -Parent $Here) $k
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination (Join-Path $vr $k) -Force }
    }
    Set-Content -LiteralPath (Join-Path $vr 'version.txt') -Value ($Product + ' ' + $Version) -Encoding ASCII

    $startArgs = $null
    if ($ready) { $startArgs = Start-Args $game }
    if ($ready -and -not $NoShortcuts) {
        if ($startArgs) {
            Make-Shortcut (Join-Path (Desktop) $ShortcutName) (Join-Path $game $GameExe) $startArgs $game 'Doom in VR' (Join-Path $vr ('tools\' + $IconFile))
        } else {
            Make-Shortcut (Join-Path (Desktop) $ShortcutName) (Join-Path $vr $Launcher) '' $vr 'Doom in VR' (Join-Path $vr ('tools\' + $IconFile))
        }
        Log ('Desktop shortcut: ' + $ShortcutName)
    }

    # What Uninstall removes: everything in the VR folder Setup and the game
    # set-up wrote - Doom VR, the copies of your games' files, the mods, the
    # art, the launchers - but never your settings.
    $manifest = New-Object System.Collections.Generic.List[string]
    foreach ($f in Get-ChildItem -LiteralPath $vr -Recurse -File) {
        $rel = $f.FullName.Substring($vr.Length + 1)
        $isPlayer = $false
        foreach ($p in $PlayerFiles) { if ($rel -ieq ('game\' + $p)) { $isPlayer = $true } }
        # Any book's notes and closing page are the player's, and so is a book
        # they added themselves (a gametome\tome-<game> this release does not ship).
        if ($rel -like 'game\gametome\*' -and $f.Name -in 'ink.txt', 'place.txt', 'typed.txt') { $isPlayer = $true }
        if ($f.Name -like 'doomvr.ini.before-reset-*') { $isPlayer = $true }	# settings Reset settings set aside
        if ($rel -like 'game\gametome\tome-*' -and -not (Test-Path -LiteralPath (Join-Path $src $rel.Substring(5)))) { $isPlayer = $true }
        if ($isPlayer -or $rel -ieq 'install-manifest.txt' -or $rel -ieq 'setup-log.txt') { continue }
        $manifest.Add($rel)
    }
    $manifest.Add('setup-log.txt')
    $manifest.Add('install-manifest.txt')
    Set-Content -LiteralPath (Join-Path $vr 'install-manifest.txt') -Value (@(
        ('# ' + $Product + ' ' + $Version + ', installed ' + (Get-Date -Format 'yyyy-MM-dd HH:mm')),
        ('# doom: ' + $(if ($doom) { $doom } else { 'none' })),
        '# Uninstall removes exactly these files and folders (dir:).  Your settings (doomvr.ini and the',
        '# hand and weapon fits) are not in it; saved games are in Windows'' Saved Games folder.') + $manifest) -Encoding UTF8

    if (-not $ready) {
        Log ''
        Log 'Doom VR is installed, but no game was set up: see above, then run Setup.bat again.'
        $Result['result'] = 'no game set up'
        Copy-Item -LiteralPath $LogFile -Destination (Join-Path $vr 'setup-log.txt') -Force
        $Result['vrFolder'] = $vr
        Finish 1
    }
    if (-not $gamesOk) { Log '  (The game search reported a problem: see above.  What it found is set up.)' }

    # The Steam library, if Steam is here and the player wants it.
    $Result['steam'] = 'skipped'
    if (-not $NoSteam -and $startArgs -and (Steam-Configs).Count -gt 0) {
        $want = $false
        if ($NoPrompt) { $want = [bool]$AddToSteam }
        else { Say ''; $want = Ask-YesNo ('Add ' + $Title + ' to your Steam library?') }
        if ($want) {
            if (Steam-Ready $true) {
                $Result['steam'] = Steam-Add $vr $startArgs
                Log 'Added to your Steam library (and SteamVR''s).'
                Steam-Reopen
            } else {
                $Result['steam'] = 'skipped: Steam is open'
                Log 'Not added to Steam: Steam has to be closed.  Run Setup.bat again any time to add it.'
            }
        }
    }

    Log ''
    Log 'Done.  Start it from the desktop shortcut, or "Doom VR.cmd" in the VR folder.'
    Log 'Put your headset on first, with Virtual Desktop or SteamVR running.'
    $look = Look-Status $vr
    if ($look.missing.Count -gt 0) {
        Log ''
        Log ('The Modern VR look (Voxel Doom, NashGore, upscaled textures) needs ' + $look.missing.Count + ' one-click download(s):')
        Log ('  run "' + $GetLook + '" in the VR folder - it opens the pages and adds each file as it arrives.')
    }
    $Result['modernLook'] = $look
    Log 'Bought another Doom-family game later?  Run "Find my new games.cmd" in the VR folder.'
    Log 'Uninstall.bat and Collect logs.bat are in the VR folder too.'
    Copy-Item -LiteralPath $LogFile -Destination (Join-Path $vr 'setup-log.txt') -Force
    $Result['vrFolder'] = $vr
    $Result['result'] = 'installed'
    # No Explorer window at the end: the install is done and the desktop shortcut
    # starts the game. A folder popping up after a one-click install looks janky.
}

# --- uninstall -------------------------------------------------------------------

function Remove-Empty([string] $dir) {
    if (-not (Test-Path -LiteralPath $dir)) { return }
    foreach ($d in Get-ChildItem -LiteralPath $dir -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending) {
        if (-not (Get-ChildItem -LiteralPath $d.FullName -Force)) { Remove-Item -LiteralPath $d.FullName -Force }
    }
    if (-not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir -Force }
}

function Do-Uninstall {
    Banner 'Uninstall'
    Search-Roots
    $folders = Installed-VRFolders

    # The Steam library first, while the VR folder still says what is ours.
    $Result['steam'] = 'none'
    if (Steam-HasOurs) {
        if (Steam-Ready $true) {
            Steam-Remove
            $Result['steam'] = 'removed'
            Log 'Removed from your Steam library.'
            Steam-Reopen
        } else {
            $Result['steam'] = 'left: Steam is open'
            Log 'Still in your Steam library: Steam has to be closed.  Close it and run Uninstall again.'
        }
    }

    if ($folders.Count -eq 0) { Log 'Nothing else to uninstall: no VR folder was found.'; $Result['result'] = 'nothing installed'; return }
    foreach ($vr in $folders) {
        Log ('Removing ' + $vr)
        $game = Join-Path $vr 'game'
        $listed = Get-Content -LiteralPath (Join-Path $vr 'install-manifest.txt') | Where-Object { $_ -and -not $_.StartsWith('#') }
        foreach ($rel in $listed) {
            if ($rel.StartsWith('dir:')) {
                $p = Join-Path $vr $rel.Substring(4)
                if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
            } else {
                $p = Join-Path $vr $rel
                if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
            }
        }
        # What the game wrote that is not the player's: logs, crash reports.
        foreach ($pat in @('log-*.txt', 'logs\*.txt', 'crash*.txt', '*.dmp')) {
            Get-ChildItem -Path (Join-Path $game $pat) -File -ErrorAction SilentlyContinue | Remove-Item -Force
        }
        Remove-Empty $vr
        if (Test-Path -LiteralPath $vr) {
            if (@(Get-ChildItem -LiteralPath $vr -Recurse -File).Count) {
                Set-Content -LiteralPath (Join-Path $vr 'Your settings are kept here.txt') -Value @(
                    ($Product + ' was uninstalled.  Your settings (doomvr.ini, your hand and weapon fits'),
                    'and the Game Manual''s notes) are kept in the game folder here, and come back if you install it again.',
                    'Your saved games are in Windows'' Saved Games folder, untouched.',
                    'Delete this VR folder to remove the settings too.') -Encoding ASCII
                Log '  Your settings are kept there.'
            } else { Log '  Removed.' }
        } else { Log '  Removed.' }
    }
    foreach ($name in @($ShortcutName, 'DoomVR.lnk')) {
        $lnk = Join-Path (Desktop) $name
        if (Test-Path -LiteralPath $lnk) { Remove-Item -LiteralPath $lnk -Force; Log ('Removed the desktop shortcut ' + $name) }
    }
    Log ''
    Log ($Product + ' is uninstalled.  Your games themselves are untouched.')
    $Result['result'] = 'uninstalled'
}

# --- collect logs ------------------------------------------------------------------

function Do-Collect {
    Banner 'Collect logs'
    Search-Roots
    $stamp = Get-Date -Format 'yyyyMMdd-HHmm'
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('doomvr-logs-' + $stamp)
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory -Path $work | Out-Null

    $folders = Installed-VRFolders
    $n = 0
    foreach ($vr in $folders) {
        $dst = Join-Path $work ('vr-' + $n); $n++
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        foreach ($f in @('install-manifest.txt', 'setup-log.txt', 'version.txt')) {
            $p = Join-Path $vr $f
            if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dst }
        }
        $game = Join-Path $vr 'game'
        # The game's logs and settings; never a game file.
        foreach ($f in @(Get-ChildItem -Path (Join-Path $game 'log-*.txt') -File -ErrorAction SilentlyContinue) + @(Get-ChildItem -Path (Join-Path $game 'crash*.txt') -File -ErrorAction SilentlyContinue)) {
            Copy-Item -LiteralPath $f.FullName -Destination $dst
        }
        foreach ($f in $PlayerFiles + @('version.txt')) {
            $p = Join-Path $game $f
            if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dst }
        }
        # The Game Tome: its log, and in each book the player's pen marks and typed notes
        # (a test build's ticks are read against the book's test-boxes.txt).
        $tomes = Join-Path $game 'gametome'
        $p = Join-Path $tomes 'GameTomeLayer_log.txt'
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dst }
        foreach ($b in @(Get-ChildItem -LiteralPath $tomes -Directory -ErrorAction SilentlyContinue)) {
            foreach ($f in @('ink.txt', 'typed.txt', 'test-boxes.txt')) {
                $p = Join-Path $b.FullName $f
                if (-not (Test-Path -LiteralPath $p)) { continue }
                $bd = Join-Path $dst ('gametome\' + $b.Name)
                New-Item -ItemType Directory -Force -Path $bd | Out-Null
                Copy-Item -LiteralPath $p -Destination $bd
            }
        }
        Get-ChildItem -LiteralPath (Join-Path $game 'logs') -Filter '*.txt' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 5 | Copy-Item -Destination $dst
        Log ('Logs from ' + $vr)
    }
    foreach ($m in @('install', 'uninstall')) {
        $p = Join-Path ([System.IO.Path]::GetTempPath()) ('doomvr-' + $m + '.txt')
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $work }
    }

    # This PC: what a diagnosis needs, nothing more.
    $pc = @(($Product + ' ' + $Version + ' - this PC, ' + (Get-Date -Format 'yyyy-MM-dd HH:mm')), '')
    try { $os = Get-CimInstance Win32_OperatingSystem; $pc += ('Windows: ' + $os.Caption + ' ' + $os.Version) } catch { }
    try { foreach ($v in Get-CimInstance Win32_VideoController) { $pc += ('Graphics: ' + $v.Name + ', driver ' + $v.DriverVersion) } } catch { }
    foreach ($k in @('HKLM:\SOFTWARE\Khronos\OpenXR\1', 'HKCU:\SOFTWARE\Khronos\OpenXR\1')) {
        try { $v = Get-ItemProperty -LiteralPath $k -ErrorAction Stop; if ($v.ActiveRuntime) { $pc += ('OpenXR runtime (' + $k + '): ' + $v.ActiveRuntime) } } catch { }
    }
    foreach ($p in @('VirtualDesktop.Streamer', 'vrserver', 'vrcompositor', 'OVRServer_x64')) {
        if (Get-Process -Name $p -ErrorAction SilentlyContinue) { $pc += ('Running: ' + $p) }
    }
    $pc += ''
    $pc += ('VR folders found: ' + $folders.Count)
    foreach ($vr in $folders) { $pc += ('  ' + $vr) }
    $pc += ('In the Steam library: ' + (Steam-HasOurs))
    Set-Content -LiteralPath (Join-Path $work 'this-pc.txt') -Value $pc -Encoding UTF8

    $zip = Join-Path (Desktop) ('Doom-VR-logs-' + $stamp + '.zip')
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip
    Remove-Item -LiteralPath $work -Recurse -Force
    Log ''
    Log ('Made ' + $zip)
    Log 'Send that file to us (Discord or a GitHub issue) with a line on what happened.'
    $Result['zip'] = $zip
    if (-not $NoPrompt) { try { Start-Process explorer.exe -ArgumentList ('/select,"' + $zip + '"') } catch { } }
}

# --- status, for -Detect and -Json -------------------------------------------------

# The Modern VR look's own one-click downloads still missing (the game search
# writes game\manual-downloads.txt: look|file|what|page). For the installer: a
# button that runs "Get the Modern VR look.cmd" when any are missing.
function Look-Status([string] $vr) {
    $missing = @()
    if ($vr) {
        $list = Join-Path $vr 'game\manual-downloads.txt'
        if (Test-Path -LiteralPath $list) {
            foreach ($line in Get-Content -LiteralPath $list) {
                $f = $line -split '\|', 4
                if ($f.Count -eq 4 -and $f[0] -eq 'look') { $missing += [pscustomobject][ordered]@{ file = $f[1]; what = $f[2]; page = $f[3] } }
            }
        }
    }
    return [pscustomobject][ordered]@{
        ready = ($missing.Count -eq 0)
        missing = @($missing)
        command = $(if ($vr) { Join-Path $vr $GetLook } else { $null })
    }
}

function Game-Status {
    Search-Roots
    $doom = Find-Doom
    if (-not $doom) { $doom = Find-OwnWads }
    $vr = $null
    foreach ($f in Installed-VRFolders) { $vr = $f; break }
    $ver = $null
    if ($vr -and (Test-Path -LiteralPath (Join-Path $vr 'version.txt'))) { $ver = [string](Get-Content -LiteralPath (Join-Path $vr 'version.txt') -TotalCount 1) }
    return [pscustomobject][ordered]@{
        name = 'Doom'; title = $Title
        doomFound = [bool]$doom
        doomFolder = $doom
        installed = [bool]$vr
        vrFolder = $vr
        installedVersion = $ver
        launcher = $(if ($vr) { Join-Path $vr $Launcher } else { $null })
        flatLauncher = $(if ($vr) { Join-Path $vr $Flat } else { $null })
        gamesReady = $(if ($vr) { Test-Path -LiteralPath (Join-Path $vr 'game\DoomVR.bat') } else { $false })
        modernLook = (Look-Status $vr)
        installNote = $InstallNote
    }
}

function Do-Detect {
    Banner 'Find the game'
    Search-Roots
    Log ('Looking in ' + $Roots.Count + ' place(s):')
    foreach ($r in $Roots) { Log ('  ' + $r) }
    $s = Game-Status
    Log ('DOOM + DOOM II: ' + $(if ($s.doomFound) { $s.doomFolder } else { 'not found' }))
    Log ('Doom VR: ' + $(if ($s.installed) { $s.installedVersion + ' in ' + $s.vrFolder } else { 'not installed' }))
    $Result['steamLibrary'] = Steam-HasOurs
}

# Settings back to how they came, in every VR folder found: doomvr.ini is set
# aside (doomvr.ini.before-reset-<date>), and the game makes a fresh one with
# its defaults at its next start. Saved games (Windows' Saved Games folder),
# the fits (doomvr-weapons.txt, doomvr-holds.txt) and the books' notes stay.
function Do-Reset {
    Banner 'Reset settings'
    Search-Roots
    $done = @()
    foreach ($vr in Installed-VRFolders) {
        $ini = Join-Path $vr 'game\doomvr.ini'
        if (-not (Test-Path -LiteralPath $ini)) { Log ('No settings to reset in ' + $vr); continue }
        $kept = $ini + '.before-reset-' + (Get-Date -Format 'yyyy-MM-dd-HHmm')
        Move-Item -LiteralPath $ini -Destination $kept -Force
        Log ('Settings reset in ' + $vr + ' (the old ones: ' + (Split-Path -Leaf $kept) + ')')
        $done += $vr
    }
    if (-not $done.Count) { Log 'Doom VR is not installed here, or it has no settings yet.' }
    else { Log 'Saved games, your weapon and hand fits and the Game Tome''s notes are as they were.' }
    $Result['reset'] = @($done)
}

switch ($Mode) {
    'reset'     { Do-Reset }
    'detect'    { Do-Detect }
    'install'   { Do-Install }
    'uninstall' { Do-Uninstall }
    'collect'   { Do-Collect }
}
Finish 0
