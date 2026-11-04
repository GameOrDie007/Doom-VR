# Builds the standalone Android app (Quest 3; the Steam Frame through Lepton):
# for each ABI the libraries the engine needs (platform\quest\jni: SDL2, OpenAL
# Soft), pcvr-xr for Android, then the engine itself as libmain.so with the
# engine's own CMake and the NDK, all copied into platform\quest\app\libs; the
# engine's own files (its pk3s and soundfont) into the APK's assets; then the
# APK with Gradle.
#
# Needs the host tools from a native build first (zipdir, lemon, re2c), which
# the engine's cross build imports: build-hosttools\ImportExecutables.cmake,
# made by  cmake -S . -B build-hosttools -G "Visual Studio 18 2026" -A x64
#          cmake --build build-hosttools --config Release --target zipdir lemon re2c
#
# -Release: the headsets only (arm64), signed with the Game Or Die key named by
# $env:GOD_KEYSTORE (default keys\keystore.properties in the Android tools folder,
# never in the repository).
# The Android tools folder (sdk, jdk17, src with the libraries' release sources,
# keys) is $env:ANDROID_TOOLS, or a folder named Android two levels above the
# repository. pcvr-xr's checkout is beside this repository (or -PcvrXr <folder>).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-quest.ps1 [-NoX64] [-Release] [-NativeOnly] [-EngineOnly]
param([switch] $NoX64, [switch] $Release, [switch] $NativeOnly, [switch] $EngineOnly, [string] $PcvrXr = '')
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
$Tools = if ($env:ANDROID_TOOLS) { $env:ANDROID_TOOLS } else { [IO.Path]::GetFullPath((Join-Path $Repo '..\..\Android')) }
if (-not $PcvrXr) { $PcvrXr = Join-Path (Split-Path -Parent $Repo) 'pcvr-xr' }
if ($Release) {
    $NoX64 = $true
    if (-not $env:GOD_KEYSTORE) { $env:GOD_KEYSTORE = Join-Path $Tools 'keys\keystore.properties' }
    if (-not (Test-Path -LiteralPath $env:GOD_KEYSTORE)) { throw "no release key: $env:GOD_KEYSTORE" }
}
$ErrorActionPreference = 'Stop'
$Sdk = Join-Path $Tools 'sdk'
$Cmake = "$Sdk\cmake\3.22.1\bin"
$Ndk = (Get-ChildItem "$Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$env:JAVA_HOME = Join-Path $Tools 'jdk17'
$Quest = Join-Path $Repo 'platform\quest'
$HostTools = Join-Path $Repo 'build-hosttools\ImportExecutables.cmake'
if (-not (Test-Path $HostTools)) { throw "no host tools: $HostTools (see the top of this script)" }
$Src = (Join-Path $Tools 'src').Replace('\', '/')

function Configure($srcDir, $buildDir, $abi, $extra, $always = $false) {
    if (-not $always -and (Test-Path (Join-Path $buildDir 'build.ninja'))) { return }
    & "$Cmake\cmake.exe" -G Ninja "-DCMAKE_MAKE_PROGRAM=$Cmake\ninja.exe" "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" `
        "-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_IGNORE_PREFIX_PATH='C:/msys64/mingw64;C:/msys64/usr' `
        @extra -S $srcDir -B $buildDir | Out-Null
    if ($LASTEXITCODE) { throw "configure $srcDir for $abi failed" }
}

$abis = @('arm64-v8a')
if (-not $NoX64) { $abis += 'x86_64' }
foreach ($abi in $abis) {
    $libs = Join-Path $Quest "app\libs\$abi"
    New-Item -ItemType Directory -Force $libs | Out-Null
    $root = Join-Path $Repo "build-quest\$abi"
    $deps = Join-Path $root 'deps'
    $xr = Join-Path $PcvrXr "build-android-$abi"
    $eng = Join-Path $root 'engine'
    $loader = Join-Path $Tools "src\openxr\aar\jni\$abi\libopenxr_loader.so"

    if (-not $EngineOnly) {
        # 1. SDL2 + OpenAL Soft, from their release sources.
        Configure (Join-Path $Quest 'jni') $deps $abi @("-DEXT_SRC=$Src")
        & "$Cmake\ninja.exe" -C $deps
        if ($LASTEXITCODE) { throw "deps $abi failed" }
        # 2. pcvr-xr, our OpenXR layer, for Android.
        Configure $PcvrXr $xr $abi @("-DPCVRXR_ANDROID_LOADER=$($loader.Replace('\','/'))")
        & "$Cmake\ninja.exe" -C $xr
        if ($LASTEXITCODE) { throw "pcvr-xr $abi failed" }
        # 2b. The music decoders ZMusic links (libsndfile: OGG, FLAC, Opus; mpg123: MP3), static.
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-quest-audio.ps1') -Abis $abi
        if ($LASTEXITCODE) { throw "audio libraries $abi failed" }
        # 2c. libvpx, the VP8/VP9 decoder for the video cutscenes, static.
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-quest-vpx.ps1') -Abis $abi
        if ($LASTEXITCODE) { throw "libvpx $abi failed" }
    }
    $audio = (Join-Path $root 'audio').Replace('\', '/')
    if (-not (Test-Path "$audio/lib/libsndfile.a")) { throw "no audio libraries for $abi (tools\build-quest-audio.ps1)" }
    $vpx = (Join-Path $root 'vpx').Replace('\', '/')
    if (-not (Test-Path "$vpx/lib/libvpx.a")) { throw "no libvpx for $abi (tools\build-quest-vpx.ps1)" }
    # 3. The engine: UZDoom + Doom VR, the GLES renderer, no Vulkan, no software renderer.
    # Configured every time (cheap), so a changed option here reaches an existing build folder.
    Configure $Repo $eng $abi @(
        '-DFORCE_CROSSCOMPILE=ON', "-DIMPORT_EXECUTABLES=$($HostTools.Replace('\','/'))",
        '-DHAVE_VULKAN=OFF', '-DHAVE_GLES2=ON', '-DZDOOM_ENABLE_SWR=OFF', '-DNO_OPENMP=ON',
        '-DSEND_ANON_STATS=OFF', '-DUSE_UPDATER=OFF', '-DFORCE_INTERNAL_BZIP2=ON', '-DDYN_OPENAL=ON',
        "-DSDL2_INCLUDE_DIR=$($deps.Replace('\','/'))/sdl2-include", "-DSDL2_LIBRARY=$($deps.Replace('\','/'))/SDL2/libSDL2.so",
        # The music decoders linked in, not loaded at run time (a headset has no libsndfile):
        # libsndfile and mpg123 through their own CMake packages, which bring ogg/vorbis/flac/opus with them.
        '-DDYN_SNDFILE=OFF', '-DDYN_MPG123=OFF', '-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON',
        "-DCMAKE_PREFIX_PATH=$audio;$vpx", "-DCMAKE_FIND_ROOT_PATH=$audio;$vpx",
        '-DDOOMVR_PCVRXR=ON', "-DPCVRXR_DIR=$($PcvrXr.Replace('\','/'))",
        "-DPCVRXR_ANDROID_LIB=$($xr.Replace('\','/'))/libpcvrxr.a", "-DPCVRXR_ANDROID_LOADER=$($loader.Replace('\','/'))") $true
    & "$Cmake\ninja.exe" -C $eng
    if ($LASTEXITCODE) { throw "engine $abi failed" }

    $triple = if ($abi -eq 'x86_64') { 'x86_64-linux-android' } else { 'aarch64-linux-android' }
    # ZMusic is linked into libmain.so on Android (its CMake makes it static there), so three libraries.
    Copy-Item (Join-Path $deps 'SDL2\libSDL2.so'), (Join-Path $deps 'openal\libopenal.so'), (Join-Path $eng 'libmain.so') $libs -Force
    if ($Release) {
        # The headsets get the libraries without their debug symbols; the
        # build folder keeps the full ones for reading a crash report's addresses.
        $strip = "$Ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
        foreach ($so in @('libSDL2.so', 'libopenal.so', 'libmain.so')) {
            & $strip --strip-unneeded (Join-Path $libs $so)
            if ($LASTEXITCODE) { throw "strip $so failed" }
        }
    }
    Copy-Item "$Ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\$triple\libc++_shared.so" $libs -Force
    Copy-Item $loader $libs -Force

    # The Game Tome's layer (Game Or Die's own, closed; built in its own repository by
    # vr\android\build-layer-android.cmd): in the APK's libraries, loaded by the OpenXR
    # loader from assets\openxr\1\api_layers\explicit.d\GameTomeLayer.json, never by the game.
    # $env:GAMETOME_ANDROID names its build folder (default: the Game Tome repository beside the tools).
    $tomeBuild = if ($env:GAMETOME_ANDROID) { $env:GAMETOME_ANDROID } else { Join-Path (Split-Path -Parent $Tools) 'Game Tome\vr\android\build' }
    $tomeSo = Join-Path $tomeBuild "$abi\libGameTomeLayer.so"
    if (Test-Path $tomeSo) {
        Copy-Item $tomeSo $libs -Force
        $manifestDir = Join-Path $Quest 'app\src\main\assets\openxr\1\api_layers\explicit.d'
        New-Item -ItemType Directory -Force $manifestDir | Out-Null
        Copy-Item (Join-Path $tomeBuild "$abi\GameTomeLayer.json") $manifestDir -Force
    } else {
        Write-Warning "no Game Tome layer for $abi at $tomeSo - the headset build has no book"
        Remove-Item (Join-Path $libs 'libGameTomeLayer.so') -ErrorAction SilentlyContinue
    }

    # 4. The engine's own files, into the APK's assets (the same for every ABI).
    $assets = Join-Path $Quest 'app\src\main\assets\engine'
    New-Item -ItemType Directory -Force $assets, (Join-Path $assets 'soundfonts'), (Join-Path $assets 'fm_banks') | Out-Null
    foreach ($pk3 in @('uzdoom.pk3', 'game_support.pk3', 'game_widescreen_gfx.pk3', 'lights.pk3', 'brightmaps.pk3')) {
        $f = Join-Path $eng $pk3
        if (-not (Test-Path $f)) { throw "the engine build made no $pk3" }
        Copy-Item $f $assets -Force
    }
    Copy-Item (Join-Path $Repo 'soundfont\uzdoom.sf2') (Join-Path $assets 'soundfonts') -Force
    Copy-Item (Join-Path $Repo 'fm_banks\*.wopl'), (Join-Path $Repo 'fm_banks\*.wopn') (Join-Path $assets 'fm_banks') -Force
    $ver = (& git -C $Repo describe --tags --always --dirty 2>$null)
    if (-not $ver) { $ver = (Get-Date -Format 'yyyyMMdd-HHmmss') }
    Set-Content -LiteralPath (Join-Path $assets 'engine-version.txt') -Value $ver -Encoding ASCII -NoNewline
}
if ($NativeOnly) { return }
Set-Content -LiteralPath (Join-Path $Quest 'local.properties') -Value ('sdk.dir=' + ($Sdk -replace '\\', '/' -replace ':', '\:')) -Encoding ASCII
Push-Location $Quest
try {
    # Gradle's notes go to stderr, which PowerShell 5.1 turns into errors: judge it by its exit code.
    $ErrorActionPreference = 'Continue'
    & (Join-Path $Quest 'gradlew.bat') --no-daemon -q $(if ($Release) { 'assembleRelease' } else { 'assembleDebug' }) 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^Note: ' }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw 'gradle failed' }
} finally { Pop-Location }
$apk = Join-Path $Quest $(if ($Release) { 'app\build\outputs\apk\release\app-release.apk' } else { 'app\build\outputs\apk\debug\app-debug.apk' })
'APK: {0} ({1:N1} MB)' -f $apk, ((Get-Item $apk).Length / 1MB)
