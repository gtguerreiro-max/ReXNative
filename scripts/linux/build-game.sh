#!/usr/bin/env bash
# Builds a game's work folder (python -m rexnative prepare + project) for Linux, then lays out
# <copy>/linux/ to run, as scripts/linux/run-linux.sh expects it.
#   scripts/linux/build-game.sh <work folder> <copy> <sdk install> [--rexnative <vkn out>]
# Without --rexnative, ReXGlue's Xenos renderer on Vulkan draws the game. With it, ReXNative does
# (experimental): through vkd3d-proton and DXVK Native's DXGI from build-vkd3d-dxvk.sh's <out>,
# with WineHQ vkd3d's D3DCompile (pacman vkd3d) for its own shaders. Then the SDK source tree is
# the one build-sdk.sh built (<sdk install>/../../..), and $REXNATIVE_FIDELITYFX holds the
# FidelityFX SDK's headers (Kits/FidelityFX/*/include).
# <work folder> may be on /mnt/<drive>: it is copied to <copy> (build/, run/ and archive/ left
# out), in the Linux file system, and built there. The project files must come from a ReXNative
# with the REXNATIVE option (rexnative/project.py): `python -m rexnative project <work>` writes them.
# GAME_DATA=<folder> puts a link to it in linux/game instead of a copy of game-data/.
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
src=${1:?usage: build-game.sh <work folder> <copy> <sdk install> [--rexnative <vkn out>]}
w=${2:?}; sdk=$(readlink -f "${3:?}")
vkn=
if [[ ${4:-} == --rexnative ]]; then vkn=$(readlink -f "${5:?}"); fi
mkdir -p "$w"
rsync -a --delete --exclude=/build/ --exclude=/run/ --exclude=/linux/ --exclude=/archive/ "$src/" "$w/"
cd "$w"
manifest=$(ls ./*_manifest.toml | head -1)
name=$(sed -n 's/^[[:space:]]*name[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)
# The flags of rexnative/project.py (FLAGS), less the Windows define.
flags='-march=x86-64-v3 -mtune=generic -mprefer-vector-width=128 -ffp-model=precise -fno-math-errno -fno-char8_t'
rn=(-DREXNATIVE=OFF)
if [[ -n $vkn ]]; then
  rn=(-DREXNATIVE=ON "-DREXNATIVE_DIR=$(dirname "$(dirname "$here")")"
      "-DREXNATIVE_REXGLUE_SDK_SRC=$(dirname "$(dirname "$(dirname "$sdk")")")"
      "-DREXNATIVE_FIDELITYFX_SDK=${REXNATIVE_FIDELITYFX:?set REXNATIVE_FIDELITYFX}"
      "-DREXNATIVE_DXVK_NATIVE_INCLUDE=$vkn/include/d3d12-overlay;$vkn/include/dxvk")
fi
# Before configuring, which writes the new header list: a precompiled header of the generated
# code made before spin_pause.h joined it goes, as Ninja would not compile that code again.
pch=build/CMakeFiles/${name}_recompiled.dir/cmake_pch.hxx
if [[ -n $vkn && -f $pch ]] && ! grep -q spin_pause.h "$pch"; then rm -f "$pch"*; fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" \
  "-DCMAKE_PREFIX_PATH=$sdk" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
  "-Drexglue_DIR=$sdk/lib/cmake/rexglue" "${rn[@]}"
# The generated sources are large: one job per 1.5 GB of memory.
jobs=$(awk '/MemTotal/{print int($2/1500000)}' /proc/meminfo)
(( jobs < 1 )) && jobs=1
(( jobs > $(nproc) )) && jobs=$(nproc)
cmake --build build -j "$jobs"
# linux/: the executable, the SDK's libraries the build staged beside it, the C++ runtime it was
# built with (lib/: a newer libstdc++ than the Deck's), the game's files (game/), the settings.
out=$w/linux
rm -rf "$out"; mkdir -p "$out/lib" "$out/logs"
cp -a "build/$name" build/*.so* "$out/"
cp -L "$(clang++ -print-file-name=libstdc++.so.6)" "$(clang++ -print-file-name=libgcc_s.so.1)" "$out/lib/"
if [[ -n $vkn ]]; then
  # D3D12 (vkd3d-proton), DXGI (DXVK Native), D3DCompile (WineHQ vkd3d's libvkd3d-utils).
  cp -a "$vkn/lib/libvkd3d-proton-d3d12.so" "$vkn/lib/libvkd3d-proton-d3d12core.so" "$out/lib/"
  cp -L "$vkn/lib/libdxvk_dxgi.so.0" "$out/lib/"
  ln -sf libdxvk_dxgi.so.0 "$out/lib/libdxvk_dxgi.so"
  for l in libvkd3d-utils.so.1 libvkd3d-shader.so.1 libvkd3d.so.1 libSPIRV-Tools-shared.so; do
    cp -L "/usr/lib/$l" "$out/lib/$l"
  done
fi
if [[ -n ${GAME_DATA:-} ]]; then ln -s "$GAME_DATA" "$out/game"; else cp -a game-data "$out/game"; fi
[[ -d title-update/content ]] && cp -a title-update/content "$out/title-update"
cp "$here/run-linux.sh" "$out/"; chmod +x "$out/run-linux.sh"
cat > "$out/$name.toml" <<TOML
# $name: native Linux build, no Proton.
# The Steam Deck's panel, full screen.
[ui]
window_width = 1280
window_height = 800
fullscreen = true
resolution = "720p"

[kernel]
license_mask = 1

[runtime]
title_memory_mb = 0

[gpu]
driver = "vulkan"
TOML
if [[ -n $vkn ]]; then
  printf '\n# ReXNative draws it (D3D12 on vkd3d-proton); no FSR or DLSS on Linux.\n[rexnative]\nenabled = true\nupscaler = false\nframe_latency = 2\n' >> "$out/$name.toml"
fi
# The newest symbol versions it needs: they must be on the Deck (glibc 2.43, GLIBCXX from lib/).
for f in "$out/$name" "$out"/*.so* "$out"/lib/*; do
  [[ -L $f ]] || objdump -T "$f" 2>/dev/null
done | grep -oE 'GLIBC_2\.[0-9]+' | sort -V -u | tail -1
du -shL "$out" 2>/dev/null | tail -1
