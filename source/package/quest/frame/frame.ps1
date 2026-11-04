# Copyright (c) 2026 Game Or Die.  All rights reserved.
# The Steam Frame route shared by every Game Or Die port's setup-quest.ps1: the Frame found on
# the home network, this PC paired with it once, our files copied over SSH, and the game put
# into the Frame's own library under its own name with its art (no "Devkit Game" label, no
# Lepton Development, no PC needed to play).  Windows' own OpenSSH only (ssh, scp, ssh-keygen,
# tar: Windows 10 1809 and later).
#
# Copy this file,
# library.sh and library.py into the port's files\frame\ folder and dot-source it:
#
#     $FrameSlug    = 'unreal-gold'                 # our folder on the Frame: ~/GameOrDie/<slug>
#     $FrameApkName = 'UnrealGoldVR.apk'            # the app's file name there
#     $AppName      = 'Unreal Gold VR'              # the name in the Frame's library
#     $AppId        = 'com.example.unrealgold'      # the Android package
#     . (Join-Path $PSScriptRoot 'frame\frame.ps1')
#
# A native Linux ARM64 build sets $FrameTool before dot-sourcing:
# '' runs it on SteamOS itself, 'steamlinuxruntime_steamrt4-arm64' in Valve's runtime. $FrameApkName
# is then its start script, and $FrameFiles (the game's folder) is ~/GameOrDie/<slug>-files,
# which Frame-Remove leaves, as the APK's own folder is left.
#
# The Setup provides Log, Say, Size-Text and an ordered $Result, as every hub Setup has; the
# kit brings its own Run-Native and Win-Arg when the Setup has none.  README.md beside this
# file says how a Setup's -SteamFrame install is put together from these pieces.

$DevkitPort = 32000
$PairToken = '900b919520e4cf601998a71eec318fec'
if (-not (Get-Variable FrameKeys -Scope Script -ErrorAction SilentlyContinue) -or -not $FrameKeys) { $FrameKeys = Join-Path $env:LOCALAPPDATA 'Game Or Die\frame' }
if (-not (Get-Variable Frame -Scope Script -ErrorAction SilentlyContinue)) { $Frame = '' }
$KeyPath = Join-Path $FrameKeys 'id_rsa'
$KnownHosts = Join-Path $FrameKeys 'known_hosts'
$LastFrame = Join-Path $FrameKeys 'last-frame.txt'
$SshExe = Join-Path $env:SystemRoot 'System32\OpenSSH\ssh.exe'
$ScpExe = Join-Path $env:SystemRoot 'System32\OpenSSH\scp.exe'
$KeygenExe = Join-Path $env:SystemRoot 'System32\OpenSSH\ssh-keygen.exe'
$TarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
$FrameKit = $PSScriptRoot  # library.sh, library.py and the art sit beside this file
$FrameAddress = ''
$FrameUser = 'steamos'
$FrameHome = ''
$FrameSteam = ''
$FrameUserId = ''
$FrameDir = ''             # our folder on the Frame: the app, the art, the library scripts
$FrameFiles = ''           # the app's own folder there (Android/data/<package>/files of its own Lepton context)
$FrameAppId = 0            # the shortcut's app id (Steam's rule: CRC32 of target and name, top bit set)
$FrameFacts = @{}
if (-not (Get-Variable FrameTool -Scope Script -ErrorAction SilentlyContinue)) { $FrameTool = 'lepton' }
$FrameLepton = ($FrameTool -eq 'lepton')

if (-not (Get-Command Win-Arg -ErrorAction SilentlyContinue)) {
    function Win-Arg([string] $a) {
        if ($a -eq '') { return '""' }
        if ($a -notmatch '[\s"]') { return $a }
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.Append('"')
        $bs = 0
        foreach ($ch in $a.ToCharArray()) {
            if ($ch -eq '\') { $bs++; continue }
            if ($ch -eq '"') { [void]$sb.Append('\' * ($bs * 2 + 1)); [void]$sb.Append('"'); $bs = 0; continue }
            if ($bs) { [void]$sb.Append('\' * $bs); $bs = 0 }
            [void]$sb.Append($ch)
        }
        if ($bs) { [void]$sb.Append('\' * ($bs * 2)) }
        [void]$sb.Append('"')
        return $sb.ToString()
    }
}
if (-not (Get-Command Run-Native -ErrorAction SilentlyContinue)) {
    function Run-Native([string] $exe, [string[]] $a, [int] $timeoutSec = 120) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = (($a | ForEach-Object { Win-Arg $_ }) -join ' ')
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.RedirectStandardInput = $true
        $psi.CreateNoWindow = $true
        $p = [System.Diagnostics.Process]::Start($psi)
        $p.StandardInput.Close()
        $o = $p.StandardOutput.ReadToEndAsync()
        $e = $p.StandardError.ReadToEndAsync()
        $exited = $p.WaitForExit($timeoutSec * 1000)
        if (-not $exited) { try { $p.Kill() } catch { } }
        $p.WaitForExit()
        return @{ Code = $(if ($exited) { $p.ExitCode } else { -1 }); Out = $o.Result; Err = $e.Result; TimedOut = (-not $exited) }
    }
}

function Test-TcpPort([string] $hostName, [int] $port, [int] $timeoutMs = 3000) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $ar = $c.BeginConnect($hostName, $port, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne($timeoutMs)) { return $false }
        $c.EndConnect($ar)
        return $c.Connected
    } catch { return $false } finally { $c.Close() }
}

function Invoke-Http([string] $method, [string] $url, [string] $body, [int] $timeoutSec = 15) {
    $req = [System.Net.HttpWebRequest]::Create($url)
    $req.Method = $method
    $req.Timeout = $timeoutSec * 1000
    $req.ReadWriteTimeout = $timeoutSec * 1000
    $req.Proxy = $null
    $req.KeepAlive = $false
    $req.ServicePoint.Expect100Continue = $false
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    # A GET carries no body (a body on a GET is refused before it is sent).
    if ($method -ne 'GET' -and $null -ne $body) {
        $bytes = $utf8.GetBytes($body)
        $req.ContentType = 'text/plain'
        $req.ContentLength = $bytes.Length
        $s = $req.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
    }
    $resp = $null
    try { $resp = $req.GetResponse() }
    catch [System.Net.WebException] { $resp = $_.Exception.Response; if ($null -eq $resp) { throw } }
    try {
        $reader = New-Object System.IO.StreamReader($resp.GetResponseStream(), $utf8)
        $text = $reader.ReadToEnd(); $reader.Close()
        return @{ Status = [int]$resp.StatusCode; Body = $text }
    } finally { $resp.Close() }
}

# The Frame on the home network: the one named, else the last one, else the one answering on
# the developer service's port (its name may not resolve, and its address changes).
function Find-Frame {
    $want = $Frame
    if (-not $want -and (Test-Path -LiteralPath $LastFrame)) { $want = (Get-Content -LiteralPath $LastFrame -ErrorAction SilentlyContinue | Select-Object -First 1); if ($want) { $want = $want.Trim() } }
    if ($want) {
        $addr = $want
        if ($want -notmatch '^\d+\.\d+\.\d+\.\d+$') {
            try {
                $ips = [System.Net.Dns]::GetHostAddresses($want)
                $v4 = @($ips | Where-Object { $_.AddressFamily -eq 'InterNetwork' })
                if ($v4.Count) { $addr = $v4[0].IPAddressToString } else { $addr = '' }
            } catch { $addr = '' }
        }
        if ($addr -and ((Test-TcpPort $addr $DevkitPort 2000) -or (Test-TcpPort $addr 22 2000))) { Log ('Steam Frame: ' + $want + ' = ' + $addr); return $addr }
        Log ('Steam Frame: ' + $want + ' does not answer' + $(if ($addr) { ' at ' + $addr } else { ' (the name does not resolve)' }))
        if ($Frame) { return '' }
    }
    Say '  Looking for a Steam Frame on the home network (its developer service, port 32000)...'
    $found = @()
    $nets = @()
    $homeNets = @{}    # networks this PC reaches the internet through
    $gateways = @{}
    try {
        foreach ($ni in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            if ($ni.OperationalStatus -ne 'Up' -or $ni.NetworkInterfaceType -eq 'Loopback') { continue }
            $props = $ni.GetIPProperties()
            $gw = @($props.GatewayAddresses | Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' -and $_.Address.ToString() -ne '0.0.0.0' } | ForEach-Object { $_.Address.ToString() })
            foreach ($g in $gw) { $gateways[$g] = $true }
            foreach ($u in $props.UnicastAddresses) {
                $ip = $u.Address
                if ($ip.AddressFamily -ne 'InterNetwork') { continue }
                $o = $ip.GetAddressBytes()
                if ($o[0] -eq 10 -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31) -or ($o[0] -eq 192 -and $o[1] -eq 168)) {
                    $net = $o[0].ToString() + '.' + $o[1] + '.' + $o[2]
                    $nets += $net
                    if ($gw.Count) { $homeNets[$net] = $true }
                }
            }
        }
    } catch { }
    foreach ($net in ($nets | Sort-Object -Unique)) {
        $clients = @()
        foreach ($h in 1..254) {
            $c = New-Object System.Net.Sockets.TcpClient
            $clients += @{ Client = $c; Ar = $c.BeginConnect(($net + '.' + $h), $DevkitPort, $null, $null); Host = ($net + '.' + $h) }
        }
        Start-Sleep -Milliseconds 1500
        foreach ($x in $clients) {
            try { if ($x.Ar.IsCompleted) { $x.Client.EndConnect($x.Ar); if ($x.Client.Connected) { $found += $x.Host } } } catch { }
            try { $x.Client.Close() } catch { }
        }
    }
    if ($found.Count -eq 1) { Log ('Steam Frame: found at ' + $found[0]); return $found[0] }
    if ($found.Count -gt 1) {
        # One Frame can answer twice: on the home Wi-Fi and on its own link to Valve's USB
        # adapter in this PC.  The same SSH host
        # key is the same Frame; then the home network's address, the one every PC can reach.
        $keys = @($found | ForEach-Object { Frame-HostKey $_ })
        if (-not ($keys -contains '') -and @($keys | Sort-Object -Unique).Count -eq 1) {
            $pick = $found[0]
            foreach ($a in $found) {
                $net = ($a -split '\.')[0..2] -join '.'
                if ($homeNets[$net] -and -not $gateways[$a]) { $pick = $a; break }
            }
            Log ('Steam Frame: found at ' + $pick + ' (also answers at ' + (($found | Where-Object { $_ -ne $pick }) -join ', ') + ')')
            return $pick
        }
        Log ('Several answer on port 32000 (' + ($found -join ', ') + '); name yours with -Frame <address>.')
        return ''
    }
    Log 'No Steam Frame found on the home network.'
    return ''
}

# A machine's SSH host key, read without signing in ('' when it gives none): tells one Frame
# on two networks from two Frames.  (Windows' ssh-keyscan cannot talk to the Frame's sshd.)
function Frame-HostKey([string] $addr) {
    $kh = Join-Path ([System.IO.Path]::GetTempPath()) ('god-frame-hostkey-' + [guid]::NewGuid().ToString('n'))
    try {
        Run-Native $SshExe @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', '-o', ('UserKnownHostsFile=' + ($kh -replace '\\', '/')),
            '-o', 'PubkeyAuthentication=no', '-o', 'PasswordAuthentication=no', '-o', 'KbdInteractiveAuthentication=no',
            '-o', 'ConnectTimeout=5', '-o', 'LogLevel=ERROR', ('probe@' + $addr), 'exit') 20 | Out-Null
        if (-not (Test-Path -LiteralPath $kh)) { return '' }
        return ((@(Get-Content -LiteralPath $kh) | Where-Object { $_ } | ForEach-Object { (($_ -split ' ')[1..2]) -join ' ' } | Sort-Object) -join '|')
    } catch { return '' } finally { Remove-Item -LiteralPath $kh -Force -ErrorAction SilentlyContinue }
}

function Ssh-Args {
    $kh = $KnownHosts -replace '\\', '/'
    return @('-i', $KeyPath, '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
             '-o', 'ConnectTimeout=10', '-o', ('UserKnownHostsFile="' + $kh + '"'), '-o', 'LogLevel=ERROR')
}
function Ssh([string] $cmd, [int] $timeoutSec = 120) {
    return Run-Native $SshExe (@(Ssh-Args) + @(($FrameUser + '@' + $FrameAddress), $cmd)) $timeoutSec
}
function Scp-To([string] $local, [string] $remote, [int] $timeoutSec = 600) {
    $r = Run-Native $ScpExe (@('-p', '-q') + @(Ssh-Args) + @($local, ($FrameUser + '@' + $FrameAddress + ':' + $remote))) $timeoutSec
    if ($r.Code -ne 0) { Log ('  scp to the Frame failed: ' + $r.Err.Trim()) }
    return ($r.Code -eq 0)
}
function Scp-From([string] $remote, [string] $local) {
    $r = Run-Native $ScpExe (@('-p', '-q') + @(Ssh-Args) + @(($FrameUser + '@' + $FrameAddress + ':' + $remote), $local)) 600
    return ($r.Code -eq 0)
}
function Test-Paired {
    $r = Ssh 'echo paired' 25
    return (($r.Code -eq 0) -and ($r.Out -match 'paired'))
}

# Pairs this PC with the Frame once: an RSA key of our own (the Frame refuses ed25519),
# registered through the Frame's developer service while the player accepts on the headset.
# $true when SSH works.
function Pair-Frame {
    foreach ($exe in @($SshExe, $ScpExe, $KeygenExe)) {
        if (-not (Test-Path -LiteralPath $exe)) { throw ('Windows'' OpenSSH client is missing (' + $exe + ').  Settings, System, Optional features: add "OpenSSH Client", then run this again.') }
    }
    New-Item -ItemType Directory -Force -Path $FrameKeys | Out-Null
    if (-not ((Test-Path -LiteralPath $KeyPath) -and (Test-Path -LiteralPath ($KeyPath + '.pub')))) {
        Remove-Item -LiteralPath $KeyPath, ($KeyPath + '.pub') -Force -ErrorAction SilentlyContinue
        $kg = Run-Native $KeygenExe @('-q', '-t', 'rsa', '-b', '2048', '-N', '', '-C', ('devkit-client:GameOrDie@' + $env:COMPUTERNAME), '-f', $KeyPath) 60
        if ($kg.Code -ne 0 -or -not (Test-Path -LiteralPath ($KeyPath + '.pub'))) { throw ('could not make a key for the Frame: ' + $kg.Err.Trim()) }
        Log ('  a key for the Frame made: ' + $KeyPath)
    }
    $pub = ([System.IO.File]::ReadAllText($KeyPath + '.pub')).Trim()
    if ($pub -notmatch '^ssh-rsa \S+') { throw ('the key file does not look right: ' + $KeyPath + '.pub') }
    try {
        $r = Invoke-Http 'GET' ('http://' + $FrameAddress + ':' + $DevkitPort + '/login-name') $null 15
        if ($r.Status -eq 200 -and $r.Body.Trim() -match '^[a-z_][a-z0-9_.-]*$') { $script:FrameUser = $r.Body.Trim() }
    } catch { }
    if (Test-Paired) { Log ('  paired with the Frame already (as ' + $FrameUser + ')'); return $true }
    if (-not (Test-TcpPort $FrameAddress $DevkitPort 3000)) {
        Log '  The Frame''s developer service does not answer: on the Frame, Settings, System, Developer Mode on.'
        return $false
    }
    Say ''
    Say '  Pairing this PC with the Frame (once).' 'Yellow'
    Say '  On the Frame: Settings, Developer, Pair new host; then Accept when it asks.'
    $Result['pairing'] = $true
    $ok = $false
    try {
        $r = Invoke-Http 'POST' ('http://' + $FrameAddress + ':' + $DevkitPort + '/register') ($pub + ' ' + $PairToken + "`n") 130
        Log ('  the Frame answered ' + $r.Status + ': ' + $r.Body.Trim())
        $ok = ($r.Status -ge 200 -and $r.Status -lt 300)
    } catch { Log ('  the pairing request failed: ' + $_.Exception.Message) }
    for ($i = 1; $i -le 5; $i++) {
        if (Test-Paired) { Log ('  paired: SSH to the Frame works (as ' + $FrameUser + ')'); return $true }
        if ($i -lt 5) { Start-Sleep -Seconds 4 }
    }
    if ($ok) { Log '  the Frame accepted the key, but SSH does not work yet.  Run this again.' }
    else { Log '  not paired.  Was Accept chosen on the Frame?  Run this again.' }
    return $false
}

# CRC32 as Steam uses it for a shortcut's app id.
$CrcTable = $null
function Crc32([byte[]] $bytes) {
    if (-not $script:CrcTable) {
        $script:CrcTable = New-Object long[] 256
        for ($n = 0; $n -lt 256; $n++) {
            $c = [long]$n
            for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = 0xEDB88320L -bxor ($c -shr 1) } else { $c = $c -shr 1 } }
            $script:CrcTable[$n] = $c
        }
    }
    $crc = 0xFFFFFFFFL
    foreach ($b in $bytes) { $crc = $script:CrcTable[($crc -bxor $b) -band 0xFF] -bxor ($crc -shr 8) }
    return (($crc -bxor 0xFFFFFFFFL) -band 0xFFFFFFFFL)
}

# The shortcut's app id for an app at this path with this library name.
function Frame-ShortcutId([string] $apkPath, [string] $name) {
    $exe = '"' + $apkPath + '"'
    return ((Crc32 ([System.Text.Encoding]::UTF8.GetBytes($exe + $name))) -bor 0x80000000L)
}

# What the Frame has: the home folder, Steam's folder and user, Lepton, Python; our folders
# and the app id follow from them.  $false when Steam is not where a Frame keeps it.
function Frame-Facts {
    $cmd = 'echo "HOME=$HOME"; S=$HOME/.local/share/Steam; [ -d "$S" ] || S=$(readlink -f $HOME/.steam/steam 2>/dev/null); echo "STEAM=$S"; ' +
           'for u in "$S"/userdata/*/; do u=${u%/}; n=${u##*/}; case "$n" in 0|ac|anonymous) continue;; esac; echo "USER=$n $(stat -c %Y "$u/config/localconfig.vdf" 2>/dev/null || echo 0)"; done; ' +
           '[ -f "$S/steamapps/appmanifest_3029110.acf" ] && echo LEPTON=1; command -v python3 >/dev/null 2>&1 && echo PY=1; command -v systemd-run >/dev/null 2>&1 && echo SYSTEMD=1; ' +
           '[ -x /opt/steamvr/bin/linuxarm64/vrcmd ] && echo VRCMD=1; echo "MODEL=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)"; echo "FREE=$(df -k $HOME | tail -1 | awk ''{print $4}'')"'
    $r = Ssh $cmd 60
    if ($r.Code -ne 0) { Log ('  could not read the Frame: ' + $r.Err.Trim()); return $false }
    $facts = @{ Users = @() }
    foreach ($line in ($r.Out -split "`r?`n")) {
        if ($line -match '^USER=(\d+) (\d+)$') { $facts.Users += @{ Id = $Matches[1]; Time = [long]$Matches[2] }; continue }
        if ($line -match '^([A-Z]+)=(.*)$') { $facts[$Matches[1]] = $Matches[2] }
    }
    $script:FrameHome = $facts['HOME']
    $script:FrameSteam = $facts['STEAM']
    if (-not $FrameHome -or -not $FrameSteam -or $FrameSteam -notmatch '/') { Log '  Steam was not found on the Frame.'; return $false }
    $u = $facts.Users | Sort-Object { $_.Time } -Descending | Select-Object -First 1
    if ($u) { $script:FrameUserId = $u.Id }
    $script:FrameDir = $FrameHome + '/GameOrDie/' + $FrameSlug
    $script:FrameAppId = Frame-ShortcutId ($FrameDir + '/' + $FrameApkName) $AppName
    $script:FrameFiles = if ($FrameLepton) { $FrameSteam + '/steamapps/compatdata/' + $FrameAppId + '/external/Android/data/' + $AppId + '/files' } else { $FrameHome + '/GameOrDie/' + $FrameSlug + '-files' }
    $script:FrameFacts = $facts
    Log ('  the Frame: Steam at ' + $FrameSteam + ', user ' + $(if ($FrameUserId) { $FrameUserId } else { 'none' }) + ', Lepton ' + $(if ($facts['LEPTON']) { 'installed' } else { 'NOT installed' }) + ', ' + (Size-Text ([long]$facts['FREE'] * 1KB)) + ' free')
    $Result['headset'] = [ordered]@{ id = $FrameAddress; make = 'Valve'; model = $facts['MODEL']; via = 'ssh'; steam = $FrameSteam; user = $FrameUserId; lepton = [bool]$facts['LEPTON']; appid = $FrameAppId }
    return $true
}

# Found, paired and read; remembered for next time.  $false when any step fails.
function Frame-Connect {
    $script:FrameAddress = Find-Frame
    if (-not $FrameAddress) { return $false }
    if (-not (Pair-Frame)) { return $false }
    if (-not (Frame-Facts)) { return $false }
    New-Item -ItemType Directory -Force -Path $FrameKeys | Out-Null
    [System.IO.File]::WriteAllText($LastFrame, $FrameAddress + "`r`n")
    return $true
}

# What an install needs on the Frame before anything is copied; throws with the fix.
function Frame-Ready([long] $bytesNeeded) {
    if (-not $FrameUserId) { throw 'no Steam user has signed in on the Frame yet; sign in there and run this again.' }
    if ($FrameLepton -and -not $FrameFacts['LEPTON']) {
        Say ''
        Say '  Lepton (the Frame''s Android layer, app 3029110) is not installed on the Frame.' 'Yellow'
        Say '  On the Frame: Store, search Lepton, install it; then run this again.'
        throw 'Lepton is not installed on the Frame'
    }
    if (-not $FrameFacts['PY']) { throw 'the Frame has no python3, which the library entry needs' }
    $free = [long]$FrameFacts['FREE'] * 1KB
    if ($free -lt ($bytesNeeded + 50MB)) { throw ('the Frame needs ' + (Size-Text ($bytesNeeded + 50MB)) + ' free and has ' + (Size-Text $free) + '.') }
}

# The player's next step when the Frame could not be reached or paired.
function Frame-Help {
    Say ''
    Say '  The Steam Frame could not be reached.' 'Yellow'
    Say '  On the Frame, once: Settings, System, Enable Developer Mode; Settings, Developer,'
    Say '  Set User Password; then Settings, Developer, Pair new host, and run this again.'
    Say '  The Frame and this PC must be on the same home network.'
}

function File-Sha256([string] $path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return (($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $fs.Close(); $sha.Dispose() }
}

# Our own files into our folder on the Frame (the app, the art, the library scripts), each
# copied only when the Frame's copy differs.  Returns @{ Copied; ArtChanged }.
function Frame-CopyOurs([string] $apk, [string] $artDir) {
    Ssh ('mkdir -p "' + $FrameDir + '" "' + $FrameFiles + '"') 30 | Out-Null
    $ours = @(@{ Local = $apk; Name = $FrameApkName })
    foreach ($f in @('library.py', 'library.sh')) { $ours += @{ Local = (Join-Path $FrameKit $f); Name = $f } }
    foreach ($f in @('cover.jpg', 'wide.jpg', 'hero.jpg', 'logo.png', 'icon.png')) {
        $p = Join-Path $artDir $f
        if (Test-Path -LiteralPath $p) { $ours += @{ Local = $p; Name = $f } }
    }
    $sums = @{}
    $out = (Ssh ('cd "' + $FrameDir + '" && sha256sum ' + (($ours | ForEach-Object { $_.Name }) -join ' ') + ' 2>/dev/null') 60).Out
    foreach ($line in ($out -split "`r?`n")) { if ($line -match '^([0-9a-f]{64})\s+\*?(.+)$') { $sums[$Matches[2].Trim()] = $Matches[1] } }
    $copied = 0; $art = $false
    foreach ($o in $ours) {
        if ($sums[$o.Name] -eq (File-Sha256 $o.Local)) { continue }
        Say ('    ' + $o.Name)
        if (-not (Scp-To $o.Local ($FrameDir + '/' + $o.Name))) { throw ('copying ' + $o.Name + ' to the Frame failed') }
        $copied++
        if ($o.Name -ne $FrameApkName -and $o.Name -notmatch '\.(py|sh)$') { $art = $true }   # Steam reads the art at start
    }
    if (-not $FrameLepton) { Ssh ('chmod +x "' + $FrameDir + '/' + $FrameApkName + '"') 30 | Out-Null }   # the start script
    Log ('  ' + $(if ($copied) { [string]$copied + ' of our files copied to ' + $FrameDir } else { 'our files on the Frame are up to date' }))
    # Which version is on the Frame, for -Detect (the installer offers an update by it).
    if ((Get-Variable Version -ErrorAction SilentlyContinue) -and $Version) {
        $said = ((Get-Variable Product -ValueOnly -ErrorAction SilentlyContinue) + ' ' + $Version).Trim().Replace("'", '')
        Ssh ("printf '%s\n' '" + $said + "' > `"" + $FrameDir + "/version.txt`"") 30 | Out-Null
    }
    return @{ Copied = $copied; ArtChanged = $art }
}

# A whole folder (the game's data, as the Quest Setup put it together) into a folder on the
# Frame: only files that are new or differ (by SHA-256) travel, in one tar.  Files on the
# Frame that the folder does not have are left alone (saves, settings).  Returns the count.
function Frame-SyncFolder([string] $localDir, [string] $remoteDir, [string] $what = 'the game') {
    $localDir = (Resolve-Path -LiteralPath $localDir).ProviderPath.TrimEnd('\')
    $files = @{}
    foreach ($f in Get-ChildItem -LiteralPath $localDir -Recurse -File) { $files[$f.FullName.Substring($localDir.Length + 1) -replace '\\', '/'] = $f }
    return (Frame-SyncFiles $files $localDir $remoteDir $what)
}

# The same for a list of files: relative path (with /) -> the file under $localBase, when the
# game's folder on this PC holds more than goes to the headset.  $onlyIfMissing: relative paths
# (any case) of files the app adds its own lines to once they are there, copied only when the
# Frame has none (Unreal's UMenu.int and inis).  Returns the count.
function Frame-SyncFiles([hashtable] $files, [string] $localBase, [string] $remoteDir, [string] $what = 'the game', [string[]] $onlyIfMissing = @()) {
    $localDir = (Resolve-Path -LiteralPath $localBase).ProviderPath.TrimEnd('\')
    $keep = @{}
    foreach ($k in $onlyIfMissing) { $keep[$k.ToLowerInvariant()] = $true }
    $theirs = @{}
    $out = (Ssh ('mkdir -p "' + $remoteDir + '" && cd "' + $remoteDir + '" && find . -type f -print0 | xargs -0 -r sha256sum') 900).Out
    foreach ($line in ($out -split "`r?`n")) { if ($line -match '^([0-9a-f]{64})\s+\*?\./(.+)$') { $theirs[$Matches[2]] = $Matches[1] } }
    $send = New-Object System.Collections.Generic.List[string]
    $bytes = 0L
    foreach ($rel in ($files.Keys | Sort-Object)) {
        $f = $files[$rel]
        if ($theirs.ContainsKey($rel) -and ($keep[$rel.ToLowerInvariant()] -or $theirs[$rel] -eq (File-Sha256 $f.FullName))) { continue }
        $send.Add($rel); $bytes += $f.Length
    }
    if ($send.Count -eq 0) { Log ('  ' + $what + ' is on the Frame already'); return 0 }
    Say ('  Copying ' + $what + ' to the Frame (' + $send.Count + ' files, ' + (Size-Text $bytes) + ')...')
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('gameordie-frame-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $list = Join-Path $tmp 'files.txt'
        [System.IO.File]::WriteAllLines($list, $send, (New-Object System.Text.UTF8Encoding($false)))
        $tar = Join-Path $tmp 'data.tar'
        $t = Run-Native $TarExe @('-c', '-f', $tar, '-C', $localDir, '-T', $list) 1800
        if ($t.Code -ne 0) { throw ('packing ' + $what + ' failed: ' + $t.Err.Trim()) }
        $remoteTar = $FrameDir + '/upload.tar'
        if (-not (Scp-To $tar $remoteTar 3600)) { throw ('copying ' + $what + ' to the Frame failed') }
        $x = Ssh ('tar -x -f "' + $remoteTar + '" -C "' + $remoteDir + '" && rm -f "' + $remoteTar + '" && echo UNPACKED') 900
        if ($x.Out -notmatch 'UNPACKED') { throw ('unpacking ' + $what + ' on the Frame failed: ' + $x.Err.Trim()) }
    } finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    Log ('  ' + $send.Count + ' files of ' + $what + ' copied (' + (Size-Text $bytes) + ')')
    return $send.Count
}

# The player's own (saved games, settings: files or folders relative to $remoteBase) copied
# from the Frame into $localDir, in one tar; what is not there is skipped.  Returns the number
# of files copied.
function Frame-Backup([string] $remoteBase, [string[]] $items, [string] $localDir) {
    $names = ($items | ForEach-Object { "'" + $_.Replace("'", '') + "'" }) -join ' '
    $remoteTar = $FrameDir + '/backup.tar'
    $r = Ssh ('mkdir -p "' + $FrameDir + '" && cd "' + $remoteBase + '" 2>/dev/null || exit 0; L=; for i in ' + $names + '; do [ -e "$i" ] && L="$L $i"; done; ' +
              '[ -n "$L" ] || exit 0; tar -cf "' + $remoteTar + '" $L && echo PACKED') 300
    if ($r.Out -notmatch 'PACKED') { return 0 }
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('gameordie-frame-saves-' + [guid]::NewGuid().ToString('N') + '.tar')
    try {
        if (-not (Scp-From $remoteTar $tmp)) { Log '  could not copy the saves from the Frame'; return 0 }
        New-Item -ItemType Directory -Force -Path $localDir | Out-Null
        $x = Run-Native $TarExe @('-x', '-f', $tmp, '-C', $localDir) 600
        if ($x.Code -ne 0) { Log ('  could not unpack the saves: ' + $x.Err.Trim()); return 0 }
        return @(Get-ChildItem -LiteralPath $localDir -Recurse -File).Count
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        Ssh ('rm -f "' + $remoteTar + '"') 30 | Out-Null
    }
}

# Lepton's own record of the game's last two library launches, copied into $localDir: its launch
# log, and the logcat it dumps when the app exits (Android's crash with its Java stack is in
# logcat-crash.log, seen on Unreal Gold).  A crash before the game opens its own log
# shows only here.  Returns the number of files copied.
function Frame-LeptonLogs([string] $localDir) {
    if (-not $FrameLepton) { return 0 }
    $id = 'steamlaunch-' + $FrameAppId
    return Frame-Backup ($FrameSteam + '/logs') @(('lepton-' + $id + '.log'), ('lepton-' + $id + '.prev.log'), ('lepton-logcats/' + $id)) $localDir
}

# The library entry, made or removed on the Frame by library.sh there, from a job of its own
# (stopping Steam would end a job started from our SSH session).  Waits for its last word.
function Frame-Library([string] $what) {
    $conf = Join-Path ([System.IO.Path]::GetTempPath()) ('gameordie-' + $FrameSlug + '-library.conf')
    $text = "STEAM='" + $FrameSteam + "'`nUSERID='" + $FrameUserId + "'`nAPPID='" + $FrameAppId + "'`nNAME='" + $AppName.Replace("'", '') + "'`nEXE='`"" + $FrameDir + '/' + $FrameApkName + "`"'`nTOOL='" + $(if ($FrameTool) { $FrameTool } else { '-' }) + "'`n"
    [System.IO.File]::WriteAllText($conf, $text, (New-Object System.Text.UTF8Encoding($false)))
    if (-not (Scp-To $conf ($FrameDir + '/library.conf'))) { throw 'could not write the library settings to the Frame' }
    Remove-Item -LiteralPath $conf -Force -ErrorAction SilentlyContinue
    $log = $FrameDir + '/library.log'
    Ssh ('rm -f "' + $log + '"') 30 | Out-Null
    $unit = 'gameordie-' + $FrameSlug + '-' + (Get-Date -Format 'yyyyMMddHHmmss')
    $r = Ssh ('systemd-run --user --collect --quiet --unit ' + $unit + ' /bin/bash "' + $FrameDir + '/library.sh" ' + $what + ' && echo STARTED') 30
    if ($r.Out -notmatch 'STARTED') {
        Log ('  (no user job on the Frame: ' + $r.Err.Trim() + '; starting the job plainly)')
        $r = Ssh ('setsid -f /bin/bash "' + $FrameDir + '/library.sh" ' + $what + ' </dev/null >/dev/null 2>&1 && echo STARTED') 30
        if ($r.Out -notmatch 'STARTED') { throw ('the library job did not start on the Frame: ' + $r.Err.Trim()) }
    }
    $deadline = (Get-Date).AddSeconds(150)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $t = (Ssh ('cat "' + $log + '" 2>/dev/null') 30).Out
        if ($t -match '=== .*(DONE|FAILED)\s*$') {
            foreach ($line in ($t -split "`r?`n")) { if ($line.Trim()) { Log ('    ' + $line) } }
            return ($t -match 'DONE\s*$')
        }
    }
    Log '  the library job on the Frame gave no answer in time'
    return $false
}

# Whether the game's library entry is there (Steam may be running).
function Frame-HasEntry {
    $r = Ssh ('test -f "' + $FrameDir + '/library.py" && python3 "' + $FrameDir + '/library.py" has --steam "' + $FrameSteam + '" --user ' + $FrameUserId + ' --appid ' + $FrameAppId) 60
    return ($r.Out -match '(?m)^yes')
}

# The library entry made when it is missing or its art changed.  Steam restarts on the Frame
# for a few seconds only then.
function Frame-EnsureEntry([bool] $artChanged) {
    if ((Frame-HasEntry) -and -not $artChanged) { Log ('  the library entry is there already (app id ' + $FrameAppId + ')'); return }
    Say '  Adding the game to the Frame''s library (Steam restarts there, a few seconds)...'
    if (-not (Frame-Library 'add')) { throw 'the library entry was not made.  Run "Collect Steam Frame logs.bat" and send us the zip.' }
    Log ('  the library entry made (app id ' + $FrameAppId + ')')
}

# The refresh rate SteamVR gives this game only (its own section, set live with SteamVR's
# own tool; the player's global setting is never touched).
function Frame-SetRefresh([int] $hz) {
    if ($hz -le 0) { return }
    if (-not $FrameFacts['VRCMD']) { Log '  SteamVR''s tool (vrcmd) is not on the Frame; the refresh rate was not set'; return }
    $r = Ssh ('/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-float steam.app.' + $FrameAppId + '.preferredRefreshRate ' + $hz + ' 2>&1') 60
    Log ('  SteamVR refresh rate for the game: ' + $hz + ' Hz (' + ($r.Out + $r.Err).Trim() + ')')
}

# Starts the game through the Frame's Steam, as the library would.
function Frame-SteamAge {
    $r = Ssh 'p=$(pgrep -u "$USER" -x steam | head -1); [ -n "$p" ] && ps -o etimes= -p $p' 25
    if ($r.Out.Trim() -match '^\d+$') { return [int]$r.Out.Trim() }
    return -1
}

# Setup (and a Steam crash) restarts Steam on the Frame, and for the first seconds after that it
# has not loaded its library: a start asked then is refused with "Game configuration
# unavailable" (a player's report).  So: wait until Steam has been up 30 s, ask,
# and look in Steam's own log for the game's process; once more if it never appeared.
function Frame-Start {
    $runId = ([uint64]$FrameAppId -shl 32) -bor 0x02000000
    $steamLog = $FrameSteam + '/logs/console_log.txt'
    $waited = $false
    for ($i = 0; $i -lt 45; $i++) {
        $age = Frame-SteamAge
        if ($age -ge 30) { break }
        if (-not $waited) { Log 'waiting for Steam on the Frame to finish starting'; $waited = $true }
        Start-Sleep -Seconds 2
    }
    $asked = $false
    for ($try = 1; $try -le 2; $try++) {
        $mark = 1 + [int](($(Ssh ('wc -l < "' + $steamLog + '" 2>/dev/null || echo 0') 30).Out) -replace '\D', '0')
        $r = Ssh ('steam -ifrunning steam://rungameid/' + $runId + ' >/dev/null 2>&1 && echo asked') 30
        Log ('started: ' + $(if ($r.Out -match 'asked') { 'Steam was asked to run the game' } else { 'Steam did not answer: ' + $r.Err.Trim() }))
        if ($r.Out -notmatch 'asked') { return $false }
        $asked = $true
        for ($j = 0; $j -lt 10; $j++) {
            Start-Sleep -Seconds 2
            $c = Ssh ('tail -n +' + $mark + ' "' + $steamLog + '" 2>/dev/null | grep -c "Game process added : AppID ' + $FrameAppId + '"') 30
            if ($c.Out.Trim() -match '^[1-9]') { Log '  Steam started the game'; return $true }
            if (-not (Ssh ('test -f "' + $steamLog + '" && echo yes') 30).Out.Contains('yes')) { return $true }   # no log to check: take Steam's word
            if (Frame-GameRunning) { return $true }
        }
        $why = (Ssh ('tail -n +' + $mark + ' "' + $steamLog + '" 2>/dev/null | grep -i "' + $FrameAppId + '" | tail -n 5 | cut -c1-200') 30).Out.Trim()
        Log ('  Steam did not start the game' + $(if ($why) { ': ' + $why } else { '' }) + $(if ($try -eq 1) { '; trying once more' } else { '' }))
        if ($try -eq 1) { Start-Sleep -Seconds 8 }
    }
    return $false
}

# What a failed start needs: the library entry and compatibility-tool mapping as Steam has them,
# and Steam's own log, for the Collect zip.  Nothing with an account name or a secret in it.
function Frame-SteamDiag {
    $conf = $FrameSteam + '/config/config.vdf'
    $cmd = 'S="' + $FrameSteam + '"; echo "== os"; head -n 3 /etc/os-release; echo "== steam running for (s)"; p=$(pgrep -u "$USER" -x steam | head -1); [ -n "$p" ] && ps -o etimes= -p $p; ' +
        'echo "== library users"; ls "$S/userdata"; for u in "$S"/userdata/*/; do echo "-- $u"; ls -l "${u}config/shortcuts.vdf" 2>&1; grep -a -c "' + $AppName.Replace('"', '') + '" "${u}config/shortcuts.vdf" 2>&1; done; ' +
        'echo "== entry made for user ' + $FrameUserId + '"; python3 "' + $FrameDir + '/library.py" has --steam "$S" --user ' + $FrameUserId + ' --appid ' + $FrameAppId + ' 2>&1; ' +
        'echo "== tool mapping"; grep -a -A4 "\"' + $FrameAppId + '\"" "' + $conf + '" | head -n 8; ' +
        'echo "== tools on the Frame"; ls "$S/steamapps/common" 2>&1 | grep -i -E "lepton|proton|runtime"; ' +
        'echo "== steam console_log (last 150, filtered)"; tail -n 150 "$S/logs/console_log.txt" 2>&1 | grep -v -i -E "account|steamid|persona|email|password|token" | cut -c1-240; ' +
        'echo "== steam compat_log (last 60)"; tail -n 60 "$S/logs/compat_log.txt" 2>&1 | cut -c1-240'
    return (Ssh $cmd 90).Out
}

# Whether the game is running on the Frame (copying saves while it runs would lose them).
function Frame-GameRunning {
    $r = Ssh ("pgrep -f '[" + $FrameApkName.Substring(0, 1) + ']' + $FrameApkName.Substring(1) + "' >/dev/null && echo RUNNING") 30
    return ($r.Out -match 'RUNNING')
}

# The library entry and our folder taken off the Frame; the app's own folder (saves,
# settings) stays, so a later install picks them up.
function Frame-Remove {
    if ((Ssh ('test -f "' + $FrameDir + '/' + $FrameApkName + '" && echo yes') 30).Out.Trim() -eq 'yes' -and $FrameUserId) {
        Say '  Removing the game from the Frame''s library (Steam restarts there, a few seconds)...'
        if (-not (Frame-Library 'remove')) { Log '  the library entry may still be there' }
    }
    Ssh ('rm -rf "' + $FrameDir + '"') 60 | Out-Null
    Log ('  ' + $AppName + ' removed from the Frame (' + $FrameDir + ').')
}

# -Detect on the Frame: nothing changed; what is there is read.
function Frame-Detect {
    $info = [ordered]@{ address = $null; paired = $false; installed = $false; entry = $false; appid = $null; files = $null }
    $script:FrameAddress = Find-Frame
    if (-not $FrameAddress) { return $info }
    $info['address'] = $FrameAddress
    if (-not (Test-Path -LiteralPath $KeyPath)) { Log 'Steam Frame: not paired yet (no key on this PC)'; return $info }
    try {
        $r = Invoke-Http 'GET' ('http://' + $FrameAddress + ':' + $DevkitPort + '/login-name') $null 10
        if ($r.Status -eq 200 -and $r.Body.Trim() -match '^[a-z_][a-z0-9_.-]*$') { $script:FrameUser = $r.Body.Trim() }
    } catch { }
    if (-not (Test-Paired)) { Log 'Steam Frame: not paired (SSH does not work)'; return $info }
    $info['paired'] = $true
    if (-not (Frame-Facts)) { return $info }
    $out = (Ssh ('test -f "' + $FrameDir + '/' + $FrameApkName + '" && echo APK=1; cat "' + $FrameDir + '/version.txt" 2>/dev/null') 30).Out
    $info['installed'] = ($out -match 'APK=1')
    $said = @($out -split "`r?`n" | Where-Object { $_.Trim() -and $_ -ne 'APK=1' })
    if ($info['installed'] -and $said.Count) { $info['version'] = $said[0].Trim(); $Result['installed'] = $said[0].Trim() }
    $info['entry'] = (Frame-HasEntry)
    $info['appid'] = $FrameAppId
    $info['files'] = $FrameFiles
    Log ('Steam Frame: ' + $FrameAddress + ', paired; the app ' + $(if ($info['installed']) { 'installed' } else { 'not installed' }) + ', the library entry ' + $(if ($info['entry']) { 'there' } else { 'not there' }))
    return $info
}
