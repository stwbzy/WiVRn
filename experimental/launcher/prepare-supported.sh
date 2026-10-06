#!/usr/bin/env bash
set -Eeuo pipefail
umask 077
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "${HERE}/../.." && pwd -P)"
SOURCE="${ROOT}/experimental/mocktail-vr"
VERSION=2.738.1397
ID=3092-5f0704edd9064f566ee3d6df2bd2fabbcc709f03
HASH=bbe00ae306cc251c4ea55b7a932d9c524ecb0d6d9203c2a6161bcf0fae792742
if [[ -n "${WIVRN_ANDROID_BUILD_TOOLS:-}" ]]; then
  export PATH="${WIVRN_ANDROID_BUILD_TOOLS}:${PATH:-/usr/bin:/bin}"
fi
stage="${ROOT}/build-roblox-launcher-reference"
bundle="${stage}/com.roblox.client-3092-bbe00ae306cc-bbe00ae306cc"

if [[ ! -f "${bundle}/base.apk" ]]; then
  bash "${SOURCE}/scripts/fetch_roblox_apks.sh" --source apk-pure \
    --version "${VERSION}" --staging-root "${stage}"
fi
printf '%s  %s\n' "${HASH}" "${bundle}/base.apk" | sha256sum --check --status
python3 "${HERE}/launcher.py" import-apks "${bundle}/base.apk" "${bundle}/base.apk"
python3 "${HERE}/launcher.py" canary "${ID}"
