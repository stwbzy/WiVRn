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
BASE_NAME="WiVRn-fsr-spatial-${VERSION}-linux-${ARCH}"
BUILD_DIR="$WORK_DIR/build"
STAGE_DIR="$WORK_DIR/stage"
APPDIR="$WORK_DIR/AppDir"
TOOLS_DIR="$WORK_DIR/tools"

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
	-DWIVRN_WERROR=ON

cmake --build "$BUILD_DIR" --parallel 2
ctest --test-dir "$BUILD_DIR" --output-on-failure
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR" --strip

cp "$SOURCE_DIR/README.md" "$STAGE_DIR/README.md"
cp "$SOURCE_DIR/COPYING" "$STAGE_DIR/COPYING"
cp "$SOURCE_DIR/LICENSE-SPATIAL-UPSCALING" "$STAGE_DIR/LICENSE-SPATIAL-UPSCALING"

tar --create --gzip --file "$OUTPUT_DIR/${BASE_NAME}.tar.gz" \
	--owner=0 --group=0 --numeric-owner --sort=name \
	--directory "$STAGE_DIR" .

cp -a "$STAGE_DIR" "$APPDIR"

LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
test -x "$LINUXDEPLOY"
test -x "$APPIMAGETOOL"

APPIMAGE_EXTRACT_AND_RUN=1 "$LINUXDEPLOY" \
	--appdir "$APPDIR" \
	--executable "$APPDIR/usr/bin/wivrn-server" \
	--executable "$APPDIR/usr/bin/wivrnctl" \
	--deploy-deps-only "$APPDIR/usr" \
	--desktop-file "$SOURCE_DIR/packaging/portable-linux/io.github.wivrn.wivrn.fsr-spatial.desktop" \
	--icon-file "$SOURCE_DIR/images/wivrn.svg" \
	--icon-filename io.github.wivrn.wivrn \
	--custom-apprun "$SOURCE_DIR/packaging/portable-linux/AppRun"

ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" \
	"$APPDIR" "$OUTPUT_DIR/${BASE_NAME}.AppImage"
chmod +x "$OUTPUT_DIR/${BASE_NAME}.AppImage"
