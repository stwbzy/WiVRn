#!/usr/bin/env bash
set -euo pipefail

if [[ ! -t 0 ]]; then
    printf 'Run this visual control from an interactive terminal (hello_xr monitors stdin).\n' >&2
    exit 1
fi

# Use a process-local runtime selection, never the global active_runtime.json.
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
manifest=${WIVRN_CONTROL_RUNTIME:-/var/lib/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json}
if [[ ! -r "$manifest" ]]; then
    printf 'Missing runtime manifest: %s\n' "$manifest" >&2
    exit 1
fi
if ! command -v hello_xr >/dev/null; then
    printf 'Install the Khronos OpenXR hello_xr sample before running this control.\n' >&2
    exit 1
fi
if [[ ! -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wivrn/comp_ipc" ]]; then
    printf 'WiVRn IPC unavailable. Start the server and connect the headset first.\n' >&2
    exit 1
fi
logs="$root/diagnostics/stereo-control"
mkdir -p -- "$logs"
log="$logs/$(date -u +%Y%m%dT%H%M%SZ)-$$.log"
printf 'Stereo Vulkan control. Log: %s\n' "$log"
printf 'Expect world-space cubes with binocular depth and headset tracking.\n'
printf 'Stop with Ctrl+C in this terminal.\n'
export XR_RUNTIME_JSON="$manifest"
export XR_LOADER_DEBUG=all
# Exclude the locally installed implicit upscaler from the baseline control.
export DISABLE_XR_APILAYER_NOV3_xess_upscaler=1
unset XR_ENABLE_API_LAYERS
stdbuf -oL -eL hello_xr -g Vulkan2 -vc Stereo -s Local -bm Opaque -v "$@" 2>&1 | tee "$log"
