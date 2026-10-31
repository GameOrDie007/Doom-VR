<#
    Collect logs.bat: everything a bug report needs, in one zip beside this
    folder's uzdoom.exe. Nothing is uploaded; the player sends the zip.

    In the zip:
      report.txt   the version, Windows, graphics cards and driver, the
                   active OpenXR runtime, what setup installed (file names and
                   sizes only), the VR settings, and the VR lines of the last
                   run
      the logs     log-doomvr*.txt (the last run and the one before),
                   log-doomvr-shutdown*.txt, log-setup.txt
      doomvr.ini   the settings
      CrashReport.zip, when the game wrote one

    No game data: WADs, PK3s and saves are listed by name, never copied.
    Logs are read while the game may still have them open.

    Written for Doom VR.
    Copyright (C) 2026 Game Or Die. All rights reserved. May be
    redistributed only as part of Doom VR, unchanged.
#>
param([string]$Install = "")

$ErrorActionPreference = "Stop"
if (-not $Install) { $Install = Split-Path -Parent $PSScriptRoot }
$Install = (Resolve-Path -LiteralPath $Install).ProviderPath
$exe = [IO.Path]::Combine($Install, "uzdoom.exe")
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "No uzdoom.exe in $Install. What is there:"
    Get-ChildItem -LiteralPath $Install | Select-Object -First 12 | ForEach-Object { "    " + $_.Name }
    Write-Host "Run Collect logs.bat from the DoomVR folder (extracted from the zip, not opened inside it)."
    exit 1
}

$stamp = Get-Date -Format "yyyyMMdd-HHmm"
$name = "DoomVR-logs-$stamp"
$work = [IO.Path]::Combine([IO.Path]::GetTempPath(), $name)
[IO.Directory]::CreateDirectory($work) | Out-Null

# A file the running game still has open: read it with sharing allowed.
function Grab([string]$rel) {
    $src = [IO.Path]::Combine($Install, $rel)
    if (-not (Test-Path -LiteralPath $src)) { return $false }
    $in = [IO.File]::Open($src, "Open", "Read", "ReadWrite")
    try {
        $out = [IO.File]::Create([IO.Path]::Combine($work, [IO.Path]::GetFileName($rel)))
        try { $in.CopyTo($out) } finally { $out.Dispose() }
    } finally { $in.Dispose() }
    return $true
}
function ReadShared([string]$path) {
    $in = [IO.File]::Open($path, "Open", "Read", "ReadWrite")
    try { (New-Object IO.StreamReader($in)).ReadToEnd() } finally { $in.Dispose() }
}

$r = New-Object Text.StringBuilder
function Line([string]$s = "") { [void]$r.AppendLine($s) }

Line "DoomVR report, $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
Line "Program: $((Get-Item -LiteralPath $exe).VersionInfo.ProductVersion)"
Line "Folder:  $Install"
Line ""
Line "==== Windows and graphics ===="
try {
    $os = Get-CimInstance Win32_OperatingSystem
    Line "$($os.Caption) $($os.Version) ($($os.OSArchitecture))"
    foreach ($v in Get-CimInstance Win32_VideoController) { Line "$($v.Name), driver $($v.DriverVersion)" }
} catch { Line "(could not read: $($_.Exception.Message))" }
Line ""
Line "==== OpenXR runtime ===="
foreach ($k in "HKLM:\SOFTWARE\Khronos\OpenXR\1", "HKCU:\SOFTWARE\Khronos\OpenXR\1") {
    try { $v = (Get-ItemProperty -LiteralPath $k -Name ActiveRuntime -ErrorAction Stop).ActiveRuntime; Line "$k : $v" } catch { }
}
if ($env:XR_RUNTIME_JSON) { Line "XR_RUNTIME_JSON: $env:XR_RUNTIME_JSON" }
Line ""
Line "==== What setup installed (names and sizes; nothing copied) ===="
foreach ($d in "wads", "kex", "mods", "doomce") {
    $p = [IO.Path]::Combine($Install, $d)
    if (-not (Test-Path -LiteralPath $p)) { Line "$d\ - none"; continue }
    $fs = @(Get-ChildItem -LiteralPath $p -File -Recurse -ErrorAction SilentlyContinue)
    Line ("{0}\ - {1} files" -f $d, $fs.Count)
    foreach ($f in $fs | Sort-Object FullName) { Line ("    {0,-48} {1,12:N0}" -f $f.FullName.Substring($p.Length + 1), $f.Length) }
}
$l = [IO.Path]::Combine($Install, "launchers")
$bats = @(Get-ChildItem -LiteralPath $l -Filter *.bat -File -ErrorAction SilentlyContinue)
Line ("launchers\ - {0} games" -f $bats.Count)
$covers = @(Get-ChildItem -LiteralPath ([IO.Path]::Combine($Install, "boxart")) -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in ".png", ".jpg" })
Line ("boxart\ - {0} covers of the player's own" -f $covers.Count)
Line ""
Line "==== VR settings (doomvr.ini) ===="
$ini = [IO.Path]::Combine($Install, "doomvr.ini")
if (Test-Path -LiteralPath $ini) {
    (ReadShared $ini) -split "`r?`n" | Where-Object { $_ -match '^vr_' } | Sort-Object -Unique | ForEach-Object { Line $_ }
} else { Line "no doomvr.ini - the game has not been run from this folder" }
Line ""
Line "==== The VR lines of the last run (log-doomvr.txt) ===="
$log = [IO.Path]::Combine($Install, "log-doomvr.txt")
if (Test-Path -LiteralPath $log) {
    $lines = (ReadShared $log) -split "`r?`n"
    $lines | Where-Object { $_ -match '^(DoomVR|pcvr-xr):|error|Error|FATAL|Execution could not continue' -and $_ -notmatch '^GL_EXTENSIONS' } |
        Select-Object -First 200 | ForEach-Object { Line $_ }
    Line ""
    Line "---- its last 40 lines ----"
    $lines | Select-Object -Last 40 | ForEach-Object { Line $_ }
} else { Line "no log-doomvr.txt yet" }

[IO.File]::WriteAllText([IO.Path]::Combine($work, "report.txt"), $r.ToString(), (New-Object Text.UTF8Encoding($false)))

$got = @("report.txt")
foreach ($f in "log-doomvr.txt", "log-doomvr.prev.txt", "log-doomvr-shutdown.txt", "log-doomvr-shutdown.prev.txt",
               "log-setup.txt", "doomvr.ini", "CrashReport.zip") {
    if (Grab $f) { $got += $f }
}

# The last runs the game kept (logs\, the newest 10).
$kept = [IO.Path]::Combine($Install, "logs")
if (Test-Path -LiteralPath $kept) {
    $old = @(Get-ChildItem -LiteralPath $kept -Filter "log-doomvr*.txt" -File)
    if ($old.Count) {
        $dst = [IO.Path]::Combine($work, "logs")
        [IO.Directory]::CreateDirectory($dst) | Out-Null
        foreach ($f in $old) { [IO.File]::Copy($f.FullName, [IO.Path]::Combine($dst, $f.Name)) }
        $got += "logs\ ($($old.Count) files)"
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Path]::Combine($Install, "$name.zip")
if (Test-Path -LiteralPath $zip) { [IO.File]::Delete($zip) }
[IO.Compression.ZipFile]::CreateFromDirectory($work, $zip)
# Our own temporary folder (its files and the logs\ copy in it), nothing else.
foreach ($f in Get-ChildItem -LiteralPath $work -File -Recurse) { [IO.File]::Delete($f.FullName) }
$sub = [IO.Path]::Combine($work, "logs")
if ([IO.Directory]::Exists($sub)) { [IO.Directory]::Delete($sub) }
[IO.Directory]::Delete($work)

Write-Host ""
Write-Host "Wrote $zip"
Write-Host ("  " + ($got -join ", "))
Write-Host ""
Write-Host "Send that zip with your report. Its file paths include your Windows user name."
