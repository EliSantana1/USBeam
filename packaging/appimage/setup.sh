#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# USBeam setup for the AppImage. Runs as root through pkexec (the normal password prompt),
# started by the USBeam app itself:
#
#   setup.sh --install <folder with USBeam's files> <path of the AppImage>
#       installs or updates USBeam for all users of this computer:
#       /opt/usbeam (program + this script), the background service, its drivers at boot,
#       permission rules, menu entry, icon and start at sign-in.
#       The AppImage file moves to /opt/usbeam/USBeam.AppImage.
#
#   setup.sh --uninstall
#       undoes all of that, plus the firewall rules and data USBeam created. Leaves Linux's
#       own USB/IP drivers alone.
set -u
PREFIX=/opt/usbeam
UNIT=/etc/systemd/system/usbeam.service
MODS=/etc/modules-load.d/usbeam.conf
RULES=/etc/polkit-1/rules.d/49-usbeam.rules
APPS=/usr/local/share/applications/usbeam.desktop
AUTO=/etc/xdg/autostart/usbeam.desktop
ICON=/usr/local/share/icons/hicolor/scalable/apps/usbeam.svg

[ "$(id -u)" = 0 ] || { echo "needs admin rights" >&2; exit 2; }
command -v systemctl >/dev/null 2>&1 || { echo "no-systemd" >&2; exit 4; }

refresh_menus() {
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/local/share/applications 2>/dev/null
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t /usr/local/share/icons/hicolor 2>/dev/null
    return 0
}

case "${1:-}" in
--install)
    SRC="${2:?}"; IMG="${3:?}"
    # A USBeam package (for example the Arch package) is already installed: don't mix the two.
    if [ -f /usr/lib/systemd/system/usbeam.service ]; then echo "package-installed" >&2; exit 3; fi
    for f in usbeam usbeam.service usbeam.rules usbeam-modules.conf usbeam.desktop usbeam-autostart.desktop usbeam.svg version; do
        [ -f "$SRC/$f" ] || { echo "missing $f" >&2; exit 5; }
    done
    [ -f "$IMG" ] || { echo "missing AppImage" >&2; exit 5; }

    systemctl stop usbeam.service 2>/dev/null     # returns shared devices first (when updating)
    mkdir -p "$PREFIX" || exit 6
    install -m 755 "$SRC/usbeam" "$PREFIX/usbeam.new" && mv -f "$PREFIX/usbeam.new" "$PREFIX/usbeam" || exit 6
    install -m 755 "$0" "$PREFIX/setup.sh.new" && mv -f "$PREFIX/setup.sh.new" "$PREFIX/setup.sh" || exit 6
    if [ "$(readlink -f "$IMG")" != "$PREFIX/USBeam.AppImage" ]; then
        install -m 755 "$IMG" "$PREFIX/USBeam.AppImage.new" && mv -f "$PREFIX/USBeam.AppImage.new" "$PREFIX/USBeam.AppImage" || exit 6
        rm -f "$IMG"                                   # the downloaded file moved to /opt/usbeam
    fi
    chown -R root:root "$PREFIX"; chmod 755 "$PREFIX"

    install -D -m 644 "$SRC/usbeam.service" "$UNIT"
    install -D -m 644 "$SRC/usbeam-modules.conf" "$MODS"
    if [ -d /etc/polkit-1/rules.d ] || [ -d /usr/share/polkit-1 ]; then install -D -m 644 "$SRC/usbeam.rules" "$RULES"; fi
    install -D -m 644 "$SRC/usbeam.desktop" "$APPS"
    install -D -m 644 "$SRC/usbeam-autostart.desktop" "$AUTO"
    install -D -m 644 "$SRC/usbeam.svg" "$ICON"
    install -m 644 "$SRC/version" "$PREFIX/installed"
    command -v restorecon >/dev/null 2>&1 && restorecon -R "$PREFIX" "$UNIT" 2>/dev/null   # SELinux (Fedora)
    refresh_menus

    modprobe usbip-host 2>/dev/null; modprobe vhci-hcd 2>/dev/null
    systemctl daemon-reload
    systemctl enable usbeam.service >/dev/null 2>&1
    systemctl restart usbeam.service || exit 7
    echo "installed"
    ;;
--uninstall)
    systemctl disable --now usbeam.service >/dev/null 2>&1    # stops: returns shared devices, releases others
    [ -x "$PREFIX/usbeam" ] && "$PREFIX/usbeam" --cleanup >/dev/null 2>&1
    rm -f "$UNIT" "$MODS" "$RULES" "$APPS" "$AUTO" "$ICON"
    rm -rf /var/lib/usbeam /run/usbeam
    systemctl daemon-reload
    refresh_menus
    rm -rf "$PREFIX"                                   # including this script (the shell already has it open)
    echo "uninstalled"
    ;;
*)
    echo "usage: setup.sh --install <folder> <AppImage> | --uninstall" >&2
    exit 1
    ;;
esac
exit 0
