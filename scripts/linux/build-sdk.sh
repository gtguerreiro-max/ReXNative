#!/usr/bin/env bash
# Builds the ReXGlue SDK for Linux (x86-64), Release, installed into <sdk>/out/install/linux-amd64.
# Run it on Linux or in WSL2 (tested: Arch Linux in WSL2, clang 23, CMake 4.4), on a copy of the
# SDK tree inside the Linux file system (a build over /mnt/<drive> is many times slower):
#   scripts/linux/build-sdk.sh <sdk source tree> [VMA include dir]
# Needs clang, cmake, ninja, the Vulkan headers and VMA (vk_mem_alloc.h), and the development
# packages the SDK's Linux CI installs (GTK 3, X11, Wayland, xkbcommon, libdecor, ALSA,
# PulseAudio, PipeWire).
# The C code gets glibc-compat.h: the build then runs on an older glibc than the one it was made
# on (the Steam Deck's), see that file.
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
sdk=$(readlink -f "${1:?usage: build-sdk.sh <sdk source tree> [VMA include dir]}")
vma=${2:-}
args=(-DREXGLUE_BUILD_TESTS=OFF "-DCMAKE_C_FLAGS=-march=x86-64-v2 -include $here/glibc-compat.h"
      "-DCMAKE_INSTALL_PREFIX=$sdk/out/install/linux-amd64")
[[ -n $vma ]] && args+=("-DREXGLUE_VMA_INCLUDE_DIR=$vma")
cd "$sdk"
cmake --preset linux-amd64 "${args[@]}"
cmake --build out/build/linux-amd64 --config Release --target install -j "$(nproc)"
echo "installed: $sdk/out/install/linux-amd64"
