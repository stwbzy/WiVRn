#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "${HERE}/../.." && pwd -P)"
SOURCE="${ROOT}/experimental/mocktail-vr"
BUILD="${ROOT}/build-roblox-launcher"
DEPS="${ROOT}/build-roblox-launcher-deps"
PIN=9a1ccbb5cebea7bb578bfb198d2a3246734e6a40
JOBS="${WIVRN_LAUNCHER_BUILD_JOBS:-4}"
EXTRA_PREFIX="${WIVRN_LAUNCHER_EXTRA_PREFIX:-}"

[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid build job count' >&2; exit 2; }
if [[ ! -d "${SOURCE}/.git" ]]; then
  git clone --no-checkout https://github.com/komaruworld/mocktail.git "${SOURCE}"
  git -C "${SOURCE}" checkout --detach "${PIN}"
fi
[[ "$(git -C "${SOURCE}" rev-parse HEAD)" == "${PIN}" ]] || {
  echo 'Unexpected upstream revision; refusing to build' >&2; exit 2;
}
[[ -z "$(git -C "${SOURCE}" status --porcelain --untracked-files=normal)" ]] || {
  echo 'Upstream checkout must remain pristine; use the CMake overlay' >&2; exit 2;
}
git -C "${SOURCE}" submodule update --init --recursive --depth 1

mkdir -p -- "${DEPS}/archives" "${DEPS}/root"
while read -r name checksum url; do
  [[ -n "${name}" && "${name}" != \#* ]] || continue
  archive="${DEPS}/archives/${url##*/}"
  if [[ ! -f "${archive}" ]]; then
    curl --fail --location --retry 2 --connect-timeout 15 \
      --output "${archive}.part" "${url}"
    printf '%s  %s\n' "${checksum}" "${archive}.part" | sha256sum --check --status
    mv -- "${archive}.part" "${archive}"
  fi
  printf '%s  %s\n' "${checksum}" "${archive}" | sha256sum --check --status
  # Only the dependency prefix is populated; no package installation or sudo.
  tar --extract --file "${archive}" --directory "${DEPS}/root" usr
done < "${HERE}/native-dependencies.tsv"

while IFS= read -r -d '' link; do
  destination="$(readlink -- "${link}")"
  ln -sfn -- "${DEPS}/root${destination}" "${link}"
done < <(find "${DEPS}/root/usr" -type l -lname '/usr/*' -print0)

# Some distro .pc files omit prefix or hard-code /usr. Relocate only our copies.
find "${DEPS}/root/usr/lib/pkgconfig" -type f -name '*.pc' \
  -exec sed -i \
    -e "s|^libdir=/usr/lib$|libdir=${DEPS}/root/usr/lib|" \
    -e "s|^includedir=/usr/include|includedir=${DEPS}/root/usr/include|" {} +

export PKG_CONFIG_PATH="${DEPS}/root/usr/lib/pkgconfig:${DEPS}/root/usr/share/pkgconfig"
cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja \
  -U '*UTF8PROC*' \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${DEPS}/root/usr${EXTRA_PREFIX:+;${EXTRA_PREFIX}}" \
  -DPKG_CONFIG_ARGN=--define-prefix \
  -DCMAKE_PROJECT_Mocktail_INCLUDE="${HERE}/overlay.cmake" \
  -DMOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER=OFF \
  -DMOCKTAIL_ENABLE_VR=ON \
  -DWIVRN_LAUNCHER_LOCAL_VR_CONTRACT=ON \
  -DBUILD_TESTING=OFF "$@"
cmake --build "${BUILD}" --parallel "${JOBS}"
