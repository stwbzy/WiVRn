#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
	echo "usage: $0 SOURCE_DIR WORK_DIR OUTPUT_DIR ARTIFACT_VERSION GIT_TAG" >&2
	exit 2
fi

SOURCE_DIR=$(realpath "$1")
WORK_DIR=$(realpath -m "$2")
OUTPUT_DIR=$(realpath -m "$3")
VERSION=$4
GIT_TAG=$5
ARCH=x86_64
BASE_NAME="WiVRn-fsr-runtime-${VERSION}-linux-${ARCH}"
BUILD_DIR="$WORK_DIR/build"
STAGE_DIR="$WORK_DIR/stage"
APPDIR="$WORK_DIR/AppDir"
TOOLS_DIR="$WORK_DIR/tools"

# Keep archive metadata stable when a caller does not provide a release
# timestamp.  SOURCE_DATE_EPOCH can be set to the source revision timestamp by
# the release job.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}
export SOURCE_DATE_EPOCH
umask 022

mkdir -p "$OUTPUT_DIR" "$TOOLS_DIR"
rm -rf "$BUILD_DIR" "$STAGE_DIR" "$APPDIR"

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
	-DGIT_TAG="$GIT_TAG" \
	-DWIVRN_BUILD_CLIENT=OFF \
	-DWIVRN_BUILD_DASHBOARD=OFF \
	-DWIVRN_BUILD_DISSECTOR=OFF \
	-DWIVRN_BUILD_SERVER=ON \
	-DWIVRN_BUILD_SERVER_LIBRARY=ON \
	-DWIVRN_BUILD_TEST=ON \
	-DWIVRN_BUILD_WIVRNCTL=ON \
	-DWIVRN_OPENXR_MANIFEST_TYPE=relative \
	-DWIVRN_USE_SYSTEM_BOOST=ON \
	-DWIVRN_USE_SYSTEM_OPENXR=OFF \
	-DWIVRN_WERROR=ON

cmake --build "$BUILD_DIR" --parallel 2
ctest --test-dir "$BUILD_DIR" --output-on-failure
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR" --strip

cp "$SOURCE_DIR/README.md" "$STAGE_DIR/README.md"

# WiVRn historically names its GPL notice COPYING.  Publish the portable
# artifacts with the conventional LICENSE name while accepting a checkout
# that already provides LICENSE.  Keep COPYING as well when it exists so the
# source and binary distributions retain the same notice.
if [ -f "$SOURCE_DIR/LICENSE" ]; then
	cp "$SOURCE_DIR/LICENSE" "$STAGE_DIR/LICENSE"
else
	cp "$SOURCE_DIR/COPYING" "$STAGE_DIR/LICENSE"
fi
if [ -f "$SOURCE_DIR/COPYING" ]; then
	cp "$SOURCE_DIR/COPYING" "$STAGE_DIR/COPYING"
fi
cp "$SOURCE_DIR/LICENSE-SGSR1-BSD-3-Clause" \
	"$STAGE_DIR/LICENSE-SGSR1-BSD-3-Clause"

tar --create --gzip --file "$OUTPUT_DIR/${BASE_NAME}.tar.gz" \
	--owner=0 --group=0 --numeric-owner --sort=name \
	--mtime="@${SOURCE_DATE_EPOCH}" \
	--directory "$STAGE_DIR" .

cp -a "$STAGE_DIR" "$APPDIR"

LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
test -x "$LINUXDEPLOY"
test -x "$APPIMAGETOOL"

# The server is mandatory for this artifact.  Dashboard and wivrnctl are
# optional CMake targets, so only hand them to linuxdeploy when installation
# produced the corresponding executable.
set -- "$LINUXDEPLOY" --appdir "$APPDIR"
for executable in wivrn-server wivrn-dashboard wivrnctl; do
	if [ -x "$APPDIR/usr/bin/$executable" ]; then
		set -- "$@" --executable "$APPDIR/usr/bin/$executable"
	fi
done
set -- "$@" \
	--deploy-deps-only "$APPDIR/usr" \
	--desktop-file "$SOURCE_DIR/packaging/portable-linux/io.github.wivrn.wivrn.fsr-runtime.desktop" \
	--icon-file "$SOURCE_DIR/images/wivrn.svg" \
	--icon-filename io.github.wivrn.wivrn \
	--custom-apprun "$SOURCE_DIR/packaging/portable-linux/AppRun"
APPIMAGE_EXTRACT_AND_RUN=1 "$@"

# appimagetool forwards its own timestamp options to mksquashfs.  Newer
# squashfs-tools reject those options when SOURCE_DATE_EPOCH is also present,
# so keep the deterministic timestamp for the tarball but clear it here.
env -u SOURCE_DATE_EPOCH ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 \
	"$APPIMAGETOOL" "$APPDIR" "$OUTPUT_DIR/${BASE_NAME}.AppImage"
chmod +x "$OUTPUT_DIR/${BASE_NAME}.AppImage"
