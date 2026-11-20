#!/usr/bin/env bash
# Runs a ReXGlue game's native Linux build (no Proton, no ReXNative: ReXGlue's Xenos renderer on
# Vulkan). The SDK's shared libraries and the libstdc++ it was built with are in lib/ beside it.
#   run-linux.sh [game flags...]
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
cd "$here"
exe=$(find "$here" -maxdepth 1 -type f -executable ! -name "*.sh" ! -name "*.so*" | head -1)
[[ -n $exe ]] || { echo "no game executable in $here" >&2; exit 1; }
name=$(basename "$exe")
export LD_LIBRARY_PATH="$here:$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$here/logs"
extra=()
[[ -f $here/try.flags ]] && read -r -a extra < "$here/try.flags"
exec "$exe" "--log_file=$name" "${extra[@]}" "$@"
