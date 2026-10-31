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
sudo apt-get install -y -q --no-install-recommends build-essential cmake ninja-build python3 python3-pip file patchelf \
    desktop-file-utils librsvg2-bin libgl1-mesa-dev libegl1 libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1-dev \
    libnss3 libdbus-1-3 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxcb-cursor0 libxcb-icccm4 \
    libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 \
    libxkbfile1 libxshmfence1 libxcomposite1 libxdamage1 libxrandr2 libxtst6 libasound2

QTVER=6.8.3
echo "== Qt (official $QTVER binaries: current long-term-support version)"
pip3 install --user -q aqtinstall
python3 -m aqt install-qt linux desktop "$QTVER" linux_gcc_64 -m qtwebengine qtwebchannel qtpositioning qtserialport qtimageformats \
    -O "$ROOT/build/Qt"
QTDIR=$ROOT/build/Qt/$QTVER/gcc_64
export PATH="$QTDIR/bin:$PATH" LD_LIBRARY_PATH="$QTDIR/lib"
[ -f "$QTDIR/lib/libQt6WebEngineCore.so.6" ] || { echo "ERROR: Qt WebEngine missing"; exit 1; }

echo "== engine (static: runs on any distribution)"
make linux
file build/usbeam

echo "== window app"
cmake -S packaging/appimage/app -B build/appimage-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$QTDIR"
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
# A PNG icon too: the AppImage tools want one for the file's own icon.
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
rsvg-convert -w 256 -h 256 assets/usbeam.svg -o "$APPDIR/usr/share/icons/hicolor/256x256/apps/usbeam.png"
install -Dm644 packaging/linux/usbeam.desktop "$APPDIR/usr/share/applications/usbeam.desktop"   # Exec=usbeam-app
# AppImageLauncher: USBeam installs itself, don't offer to "integrate" it.
echo "X-AppImage-Integrate=false" >> "$APPDIR/usr/share/applications/usbeam.desktop"
install -Dm644 LICENSE "$APPDIR/usr/share/licenses/usbeam/LICENSE"

echo "== bundle Qt"
cd build
curl -fsSL -o linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage
curl -fsSL -o linuxdeploy-plugin-qt https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage
curl -fsSL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage
chmod +x linuxdeploy linuxdeploy-plugin-qt appimagetool
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=$QTDIR/bin/qmake
export EXTRA_QT_MODULES="svg"
# Wayland support, if it was installed above.
PLAT=$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms
WL=""
for f in libqwayland-generic.so libqwayland-egl.so; do
    if [ -f "$PLAT/$f" ]; then WL="${WL:+$WL;}$f"; fi
done
if [ -n "$WL" ]; then export EXTRA_PLATFORM_PLUGINS="$WL"; fi
echo "Wayland plugins: ${WL:-none}"
[ -n "$WL" ] || { echo "ERROR: Qt's Wayland support is missing"; exit 1; }
./linuxdeploy --appdir "$APPDIR" --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/usbeam.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/usbeam.svg"

# Wayland helper plugins the Qt bundler doesn't pick up by itself (graphics, window decorations,
# window types), with the libraries they need.
QTPLUG=$("$QMAKE" -query QT_INSTALL_PLUGINS)
for d in wayland-graphics-integration-client wayland-decoration-client wayland-shell-integration; do
    if [ -d "$QTPLUG/$d" ]; then
        mkdir -p "$APPDIR/usr/plugins/$d"
        cp -n "$QTPLUG/$d"/*.so "$APPDIR/usr/plugins/$d/"
        ./linuxdeploy --appdir "$APPDIR" --deploy-deps-only "$APPDIR/usr/plugins/$d"
    fi
done
for f in platforms/libqwayland-generic.so wayland-shell-integration/libxdg-shell.so; do
    [ -f "$APPDIR/usr/plugins/$f" ] || { echo "ERROR: missing Wayland part $f"; exit 1; }
done
ls "$APPDIR/usr/plugins/wayland-graphics-integration-client" || echo "(no Wayland graphics helpers: software drawing)"

# Security modules the built-in web view loads at run time (for secure https:// connections, such
# as checking for updates). The bundler doesn't see them; without them the computer's own, newer
# copies get mixed with these older libraries and the window crashes.
NSSDIR=$(dirname "$(readlink -f "$APPDIR/usr/lib/libnss3.so")")
SYSNSS=$(dirname "$(ldconfig -p | awk '/libnss3.so / {print $NF; exit}')")
for f in libsoftokn3.so libfreebl3.so libfreeblpriv3.so libnssckbi.so libnssdbm3.so libsoftokn3.chk libfreebl3.chk libfreeblpriv3.chk libnssdbm3.chk; do
    src=$(find "$SYSNSS" "$SYSNSS/nss" -maxdepth 1 -name "$f" 2>/dev/null | head -n 1 || true)
    if [ -n "$src" ]; then cp -L "$src" "$NSSDIR/$f"; fi
done
for f in libsoftokn3.so libfreeblpriv3.so libnssckbi.so; do
    [ -f "$NSSDIR/$f" ] || { echo "ERROR: missing security module $f"; exit 1; }
done
./linuxdeploy --appdir "$APPDIR" --deploy-deps-only "$NSSDIR/libsoftokn3.so" --deploy-deps-only "$NSSDIR/libfreeblpriv3.so" --deploy-deps-only "$NSSDIR/libnssckbi.so"
find "$NSSDIR" -maxdepth 1 \( -name '*nss*' -o -name '*softokn*' -o -name '*freebl*' \) -exec ls -la {} +

# The AppImage's own icon: real files (not links), which newer appimagetool versions require.
rm -f "$APPDIR/usbeam.png" "$APPDIR/.DirIcon"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/usbeam.png" "$APPDIR/usbeam.png"
cp "$APPDIR/usbeam.png" "$APPDIR/.DirIcon"

echo "== AppImage"
OUT=USBeam-$VER-$ARCH.AppImage
ARCH=$ARCH ./appimagetool --no-appstream "$APPDIR" "$OUT"
chmod +x "$OUT"
ls -la "$OUT"
# sanity: the files setup.sh needs are inside
./"$OUT" --appimage-extract 'usr/lib/usbeam/*' >/dev/null
ls squashfs-root/usr/lib/usbeam
rm -rf squashfs-root
