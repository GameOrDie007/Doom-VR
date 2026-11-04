# Builds libvpx (the VP8/VP9 decoder for the video cutscenes - Legacy of Rust's intros
# play from .ivf files) as a static library, one prefix per ABI: build-quest\<abi>\vpx.
# libvpx builds with its own configure script and makefile, so this runs msys2's sh
# (C:\msys64) on tools\build-quest-vpx.sh with the NDK's clang. The source is
# libvpx-1.17.0 in the Android tools folder's src (see build-quest.ps1).
# build-quest.ps1 runs this for each ABI it builds; a library already in the prefix
# is not built again.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-quest-vpx.ps1 [-Abis arm64-v8a,x86_64]
param([string[]] $Abis = @('arm64-v8a', 'x86_64'))
$Abis = @($Abis -split ",") | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
$Tools = if ($env:ANDROID_TOOLS) { $env:ANDROID_TOOLS } else { [IO.Path]::GetFullPath((Join-Path $Repo '..\..\Android')) }
$ErrorActionPreference = 'Stop'
$Ndk = (Get-ChildItem "$Tools\sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$Src = Join-Path $Tools 'src\libvpx-1.17.0'
$Msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$Sh = Join-Path $Msys 'usr\bin\sh.exe'
if (-not (Test-Path $Src)) { throw "no source: $Src" }
if (-not (Test-Path $Sh)) { throw "no msys2 at $Msys (libvpx's build needs its sh and make)" }

function ToMsys([string] $p) { '/' + $p.Substring(0, 1).ToLower() + $p.Substring(2).Replace('\', '/') }

foreach ($abi in $Abis) {
    $prefix = Join-Path $Repo "build-quest\$abi\vpx"
    if (Test-Path (Join-Path $prefix 'lib\libvpx.a')) { "vpx $abi`: libvpx.a already built"; continue }
    "vpx $abi`: libvpx 1.17.0"
    $log = Join-Path $Repo "build-quest\$abi\vpx-build.log"
    $env:MSYSTEM = 'MSYS'; $env:CHERE_INVOKING = '1'; $env:MSYS2_PATH_TYPE = 'minimal'
    & $Sh -l -c ("sh '{0}' {1} '{2}' '{3}' '{4}'" -f (ToMsys (Join-Path $PSScriptRoot 'build-quest-vpx.sh')), $abi, (ToMsys $Ndk), (ToMsys $Src), (ToMsys $prefix)) 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
    Get-Content $log | Select-Object -Last 12
    if ($LASTEXITCODE) { throw "libvpx for $abi failed (see $log)" }
    if (-not (Test-Path (Join-Path $prefix 'lib\libvpx.a'))) { throw "libvpx for $abi made no libvpx.a" }
}
