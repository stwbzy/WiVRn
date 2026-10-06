#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
if (( $# == 0 )); then
  set -- vr
fi
exec /usr/bin/python3 "${root}/experimental/launcher/launcher.py" "$@"
