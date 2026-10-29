#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds USBeam-<version>-x86_64.AppImage. Meant for GitHub Actions on Ubuntu 22.04 (an older
# base, so the result runs on most current distributions). Output: build/USBeam-*.AppImage
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
VER=$(sed -n 's/.*USBEAM_VERSION "\(.*\)".*/\1/p' src/common/version.h)
ARCH=x86_64

echo "== tools"
sudo apt-get update -q
sudo apt-get install -y -q --no-install-recommends build-essential cmake ninja-build python3 file patchelf desktop-file-utils \
    qt6-base-dev qt6-base-dev-tools qt6-webengine-dev qt6-webengine-dev-tools libqt6webenginecore6-bin \
    qt6-wayland libqt6svg6 qt6-image-formats-plugins libgl1-mesa-dev libxkbcommon-dev libfontconfig1-dev

echo "== engine (static: runs on any distribution)"
make linux
file build/usbeam

echo "== window app"
cmake -S packaging/appimage/app -B build/appimage-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build/appimage-app

echo "== AppDir"
APPDIR=$ROOT/build/AppDir
rm -rf "$APPDIR"
DESTDIR=$APPDIR cmake --install build/appimage-app
L=$APPDIR/usr/lib/usbeam
install -Dm755 build/usbeam "$L/usbeam"
install -Dm755 packaging/appimage/setup.sh "$L/setup.sh"
install -Dm644 packaging/appimage/usbeam.service "$L/usbeam.service"
install -Dm644 packaging/linux/usbeam.rules "$L/usbeam.rules"
install -Dm644 packaging/linux/usbeam-modules.conf "$L/usbeam-modules.conf"
install -Dm644 packaging/appimage/usbeam.desktop "$L/usbeam.desktop"
install -Dm644 packaging/appimage/usbeam-autostart.desktop "$L/usbeam-autostart.desktop"
install -Dm644 assets/usbeam.svg "$L/usbeam.svg"
install -Dm644 assets/usbeam.svg "$APPDIR/usr/share/icons/hicolor/scalable/apps/usbeam.svg"
install -Dm644 packaging/linux/usbeam.desktop "$APPDIR/usr/share/applications/usbeam.desktop"   # Exec=usbeam-app
install -Dm644 LICENSE "$APPDIR/usr/share/licenses/usbeam/LICENSE"

echo "== bundle Qt"
cd build
curl -fsSL -o linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage
curl -fsSL -o linuxdeploy-plugin-qt https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage
curl -fsSL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage
chmod +x linuxdeploy linuxdeploy-plugin-qt appimagetool
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=/usr/lib/qt6/bin/qmake
[ -x "$QMAKE" ] || QMAKE=$(command -v qmake6)
export EXTRA_QT_MODULES="svg"
export EXTRA_QT_PLUGINS="svg"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
./linuxdeploy --appdir "$APPDIR" --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/usbeam.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/usbeam.svg"

echo "== AppImage"
OUT=USBeam-$VER-$ARCH.AppImage
ARCH=$ARCH ./appimagetool --no-appstream "$APPDIR" "$OUT"
chmod +x "$OUT"
ls -la "$OUT"
# sanity: the files setup.sh needs are inside
./"$OUT" --appimage-extract 'usr/lib/usbeam/*' >/dev/null
ls squashfs-root/usr/lib/usbeam
rm -rf squashfs-root
