#!/usr/bin/env bash
# Runs a game's native Linux build, without Proton: drawn by ReXGlue's Xenos renderer on Vulkan,
# or by ReXNative through vkd3d-proton and DXVK Native's DXGI (their libraries in lib/). The SDK's
# shared libraries are beside it, the libstdc++ it was built with in lib/.
#   run-linux.sh [game flags...]
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$here"
exe=$(find "$here" -maxdepth 1 -type f -executable ! -name "*.sh" ! -name "*.so*" | head -1)
[[ -n $exe ]] || { echo "no game executable in $here" >&2; exit 1; }
name=$(basename "$exe")
export LD_LIBRARY_PATH="$here:$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# DXVK Native's DXGI makes its swap chain on the game's SDL3 window.
export DXVK_WSI_DRIVER=SDL3 DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-warn} VKD3D_DEBUG=${VKD3D_DEBUG:-warn}
mkdir -p "$here/logs"
extra=()
[[ -f $here/try.flags ]] && read -r -a extra < "$here/try.flags"
exec "$exe" "--log_file=$name" "${extra[@]}" "$@" > "$here/logs/stdout.txt" 2>&1
