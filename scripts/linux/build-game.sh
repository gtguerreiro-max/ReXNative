#!/usr/bin/env bash
# Builds a game's work folder (python -m rexnative prepare + project) for Linux, without ReXNative
# (its renderer is D3D12 only): ReXGlue's Xenos renderer on Vulkan draws it. Then lays out
# <copy>/linux/ to run, as scripts/linux/run-linux.sh expects it.
#   scripts/linux/build-game.sh <work folder> <copy> <sdk install>
# <work folder> may be on /mnt/<drive>: it is copied to <copy> (build/, run/ and archive/ left
# out), in the Linux file system, and built there. The project files must come from a ReXNative
# with the REXNATIVE option (rexnative/project.py): `python -m rexnative project <work>` writes them.
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
src=${1:?usage: build-game.sh <work folder> <copy> <sdk install>}
w=${2:?}; sdk=$(readlink -f "${3:?}")
mkdir -p "$w"
rsync -a --delete --exclude=/build/ --exclude=/run/ --exclude=/linux/ --exclude=/archive/ "$src/" "$w/"
cd "$w"
manifest=$(ls ./*_manifest.toml | head -1)
name=$(sed -n 's/^[[:space:]]*name[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)
# The flags of rexnative/project.py (FLAGS), less the Windows define.
flags='-march=x86-64-v3 -mtune=generic -mprefer-vector-width=128 -ffp-model=precise -fno-math-errno -fno-char8_t'
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" \
  "-DCMAKE_PREFIX_PATH=$sdk" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF "-Drexglue_DIR=$sdk/lib/cmake/rexglue"
cmake --build build -j "$(nproc)"
# linux/: the executable, the SDK's libraries the build staged beside it, the C++ runtime it was
# built with (lib/: a newer libstdc++ than the Deck's), the game's files (game/), the settings.
out=$w/linux
rm -rf "$out"; mkdir -p "$out/lib" "$out/logs"
cp -a "build/$name" build/*.so* "$out/"
cp -L "$(clang++ -print-file-name=libstdc++.so.6)" "$(clang++ -print-file-name=libgcc_s.so.1)" "$out/lib/"
cp -a game-data "$out/game"
[[ -d title-update/content ]] && cp -a title-update/content "$out/title-update"
cp "$here/run-linux.sh" "$out/"; chmod +x "$out/run-linux.sh"
cat > "$out/$name.toml" <<TOML
# $name: native Linux build (ReXGlue's Xenos renderer on Vulkan, no Proton, no ReXNative).
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
# The newest symbol versions it needs: they must be on the Deck (glibc 2.43, GLIBCXX from lib/).
for f in "$out/$name" "$out"/*.so*; do objdump -T "$f"; done | grep -oE 'GLIBC_2\.[0-9]+' | sort -V -u | tail -1
du -sh "$out"
