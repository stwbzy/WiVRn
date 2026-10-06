#!/usr/bin/env bash
set -euo pipefail

# Temporary permissions only: do not change persistent overrides or active_runtime.json.
manifest=/var/lib/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json
if [[ ! -r "$manifest" ]]; then
    printf 'Missing WiVRn Flatpak runtime: %s\n' "$manifest" >&2
    exit 1
fi
if flatpak ps --columns=application | grep -Fxq org.vinegarhq.Sober; then
    printf 'Close the existing Sober instance before this diagnostic launch.\n' >&2
    exit 1
fi
exec flatpak run \
    --filesystem=/var/lib/flatpak/app/io.github.wivrn.wivrn:ro \
    --filesystem=xdg-run/wivrn:ro \
    --env="XR_RUNTIME_JSON=$manifest" \
    --env=XR_LOADER_DEBUG=all \
    org.vinegarhq.Sober "$@"
