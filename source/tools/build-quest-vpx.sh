#!/bin/sh
# Builds libvpx (VP8/VP9 decoder: the video cutscenes, Legacy of Rust's intros among
# them) as a static library for one Android ABI, into build-quest/<abi>/vpx.
# Run by tools\build-quest-vpx.ps1 under msys2's sh (libvpx's own build is a
# configure script and a makefile): sh build-quest-vpx.sh <abi> <ndk> <src> <prefix>
# The decoder only, in C with NEON on arm64 (no assembler needed); the x86_64 build
# (the emulator) is plain C.
set -e
abi="$1"; ndk="$2"; src="$3"; prefix="$4"
bin="$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin"
case "$abi" in
  arm64-v8a) target=arm64-android-gcc; triple=aarch64-linux-android29; extra="" ;;
  x86_64)    target=x86_64-android-gcc; triple=x86_64-linux-android29
             extra="--disable-mmx --disable-sse --disable-sse2 --disable-sse3 --disable-ssse3 --disable-sse4_1 --disable-avx --disable-avx2 --disable-avx512" ;;
  *) echo "unknown ABI $abi"; exit 1 ;;
esac
build="$prefix-build"
rm -rf "$build"; mkdir -p "$build"; cd "$build"
export CC="$bin/$triple-clang.cmd" CXX="$bin/$triple-clang++.cmd" LD="$bin/$triple-clang.cmd"
export AR="$bin/llvm-ar.exe" AS="$bin/$triple-clang.cmd" NM="$bin/llvm-nm.exe" STRIP="$bin/llvm-strip.exe" RANLIB="$bin/llvm-ranlib.exe"
sh "$src/configure" --target=$target --prefix="$prefix" --enable-pic \
  --disable-vp8-encoder --disable-vp9-encoder --enable-vp8-decoder --enable-vp9-decoder \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-webm-io --disable-libyuv \
  --disable-install-bins --disable-install-srcs \
  --disable-shared --enable-static --disable-postproc $extra
make -j8 libvpx.a
make install
ls -la "$prefix/lib"
