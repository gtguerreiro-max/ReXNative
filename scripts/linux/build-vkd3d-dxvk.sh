#!/usr/bin/env bash
# Builds what ReXNative needs to draw on Linux without Wine, into <out> (default ~/vkn/out):
#   lib/libvkd3d-proton-d3d12.so, libvkd3d-proton-d3d12core.so   vkd3d-proton 3.1, native
#                                                                 (with vkd3d-proton-native-
#                                                                 adapter-parent.patch)
#   lib/libdxvk_dxgi.so.0                                         DXVK Native's DXGI (SDL3 WSI),
#                                                                 which makes the swap chain
#   include/dxvk                                                  DXVK Native's Windows and
#                                                                 DirectX headers
#   include/d3d12-overlay                                         mingw-w64's newer d3d12.h
#   scripts/linux/build-vkd3d-dxvk.sh [out]
# Needs git, meson, ninja, clang, glslang, the widl of Wine (pacman: meson wine), the mingw-w64
# headers (mingw-w64-headers) and SDL3's (sdl3). The C code gets glibc-compat.h, as the SDK's.
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
out=$(readlink -f "${1:-$HOME/vkn/out}")
src=$(dirname "$out")
mkdir -p "$src" "$out"
cd "$src"
[ -d vkd3d-proton ] || git clone -q --recurse-submodules --depth 1 --branch v3.1 \
  https://github.com/HansKristian-Work/vkd3d-proton.git vkd3d-proton
[ -d dxvk ] || git clone -q --recurse-submodules --depth 1 --branch v3.1.1 \
  https://github.com/doitsujin/dxvk.git dxvk
# Native vkd3d-proton ignores the DXGI adapter it gets; DXVK's DXGI asks the swap chain for it.
if git -C vkd3d-proton apply --check "$here/vkd3d-proton-native-adapter-parent.patch" 2>/dev/null; then
  git -C vkd3d-proton apply "$here/vkd3d-proton-native-adapter-parent.patch"
fi
export CC=clang CXX=clang++ CFLAGS="-include $here/glibc-compat.h"
rm -rf build-vkd3d build-dxvk
(cd vkd3d-proton && meson setup --buildtype release --prefix "$out" --libdir lib --bindir bin \
  "$src/build-vkd3d")
ninja -C build-vkd3d install
(cd dxvk && meson setup --buildtype release --prefix "$out" --libdir lib --bindir bin \
  -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false -Dnative_sdl3=enabled \
  -Dnative_sdl2=disabled -Dnative_glfw=disabled --force-fallback-for=libdisplay-info \
  "$src/build-dxvk")
ninja -C build-dxvk install
mkdir -p "$out/include/d3d12-overlay"
cp /usr/x86_64-w64-mingw32/include/d3d12.h /usr/x86_64-w64-mingw32/include/d3d12sdklayers.h \
  "$out/include/d3d12-overlay/"
ls "$out/lib"
