# Builds the music decoders the engine's ZMusic links on Android, as static
# libraries, one prefix per ABI: build-quest\<abi>\audio.
#   libsndfile 1.2.2 (OGG Vorbis, FLAC, Opus - the IDKFA soundtrack and most
#   music packs are OGG; the PC loads the system's libsndfile at run time,
#   which no headset has) with libogg, libvorbis, flac and opus under it,
#   and mpg123 1.32 (MP3). Their release sources are in the Android tools
#   folder's src (see build-quest.ps1 for where that is).
# build-quest.ps1 runs this for each ABI it builds; a library already in the
# prefix is not built again.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-quest-audio.ps1 [-Abis arm64-v8a,x86_64]
param([string[]] $Abis = @('arm64-v8a', 'x86_64'))
$Abis = @($Abis -split ",") | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
$Tools = if ($env:ANDROID_TOOLS) { $env:ANDROID_TOOLS } else { [IO.Path]::GetFullPath((Join-Path $Repo '..\..\Android')) }
$ErrorActionPreference = 'Stop'
$Sdk = Join-Path $Tools 'sdk'
$Cmake = "$Sdk\cmake\3.22.1\bin"
$Ndk = (Get-ChildItem "$Sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$Src = Join-Path $Tools 'src'

# In link order's reverse: each one finds the ones before it in the prefix.
$Libs = @(
    @{ name = 'libogg-1.3.5'; made = 'libogg.a'
       args = @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF', '-DINSTALL_DOCS=OFF') },
    @{ name = 'libvorbis-1.3.7'; made = 'libvorbis.a'
       args = @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF') },
    @{ name = 'flac-1.4.3'; made = 'libFLAC.a'
       args = @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_CXXLIBS=OFF', '-DBUILD_PROGRAMS=OFF', '-DBUILD_EXAMPLES=OFF',
                '-DBUILD_TESTING=OFF', '-DBUILD_DOCS=OFF', '-DINSTALL_MANPAGES=OFF', '-DWITH_OGG=ON') },
    @{ name = 'opus-1.5.2'; made = 'libopus.a'
       args = @('-DOPUS_BUILD_SHARED_LIBRARY=OFF', '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_BUILD_TESTING=OFF') },
    @{ name = 'libsndfile-1.2.2'; made = 'libsndfile.a'
       args = @('-DBUILD_SHARED_LIBS=OFF', '-DENABLE_EXTERNAL_LIBS=ON', '-DENABLE_MPEG=OFF', '-DBUILD_PROGRAMS=OFF',
                '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=OFF', '-DENABLE_CPACK=OFF', '-DENABLE_PACKAGE_CONFIG=ON') },
    @{ name = 'mpg123-1.32.10'; sub = 'ports\cmake'; made = 'libmpg123.a'
       args = @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_PROGRAMS=OFF', '-DBUILD_LIBOUT123=OFF') }
)

foreach ($abi in $Abis) {
    $prefix = Join-Path $Repo "build-quest\$abi\audio"
    $prefixSlash = $prefix.Replace('\', '/')
    foreach ($lib in $Libs) {
        if (Test-Path (Join-Path $prefix "lib\$($lib.made)")) { continue }
        $srcDir = Join-Path $Src $lib.name
        if ($lib.sub) { $srcDir = Join-Path $srcDir $lib.sub }
        if (-not (Test-Path $srcDir)) { throw "no source: $srcDir" }
        $build = Join-Path $Repo "build-quest\$abi\audio-build\$($lib.name)"
        "audio $abi`: $($lib.name)"
        & "$Cmake\cmake.exe" -G Ninja "-DCMAKE_MAKE_PROGRAM=$Cmake\ninja.exe" "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" `
            "-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_INSTALL_PREFIX=$prefixSlash" "-DCMAKE_PREFIX_PATH=$prefixSlash" "-DCMAKE_FIND_ROOT_PATH=$prefixSlash" `
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_IGNORE_PREFIX_PATH='C:/msys64/mingw64;C:/msys64/usr' `
            @($lib.args) -S $srcDir -B $build | Out-Null
        if ($LASTEXITCODE) { throw "configure $($lib.name) for $abi failed" }
        & "$Cmake\ninja.exe" -C $build install | Out-Null
        if ($LASTEXITCODE) { throw "$($lib.name) for $abi failed" }
        if (-not (Test-Path (Join-Path $prefix "lib\$($lib.made)"))) { throw "$($lib.name) for $abi made no $($lib.made)" }
    }
    "audio $abi`: " + ((Get-ChildItem (Join-Path $prefix 'lib') -Filter *.a | ForEach-Object { $_.Name }) -join ' ')
}
