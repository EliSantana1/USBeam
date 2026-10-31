#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Upgrade / second-launch / uninstall test for the Linux AppImage, run by GitHub Actions as a normal
# user with sudo. The password prompt (pkexec) is replaced by sudo for the test.
#
#   tests/linux-upgrade.sh <previous release AppImage> <new AppImage>
#
# After every step it checks that exactly one USBeam app and one AppImage runtime are running, and
# that nothing from an installer or an older copy was left behind.
set -u
OLD=$(readlink -f "$1"); NEW=$(readlink -f "$2")
NEWVER=$(sed -n 's/.*USBEAM_VERSION "\(.*\)".*/\1/p' "$(dirname "$0")/../src/common/version.h")
LOG=$(mktemp -d); FAIL=0
export USBEAM_TEST=1 QT_QPA_PLATFORM=xcb QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu"
# No screen here: a virtual X11 screen (the AppImage brings Qt's Wayland and X11 parts only).
unset WAYLAND_DISPLAY
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
export DISPLAY=:99
sleep 2

say()  { echo; echo "== $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }
# The app: inside an AppImage it runs as "AppRun" (a link), so find it by its program file.
apps()     { for p in $(pgrep -u "$(id -u)"); do e=$(readlink "/proc/$p/exe" 2>/dev/null) || continue
             case "$e" in */usr/bin/usbeam-app|*/usr/bin/usbeam-app" (deleted)") echo "$p";; esac; done; }
# AppImage runtimes: their program is the AppImage file itself.
runtimes() { for p in $(pgrep -u "$(id -u)"); do e=$(readlink "/proc/$p/exe" 2>/dev/null) || continue
             case "$e" in *.AppImage|*.AppImage" (deleted)") echo "$p $e";; esac; done; }
start()    { setsid "$@" </dev/null >>"$LOG/app.log" 2>&1 & }
show()     { echo "USBeam apps:"; for p in $(apps); do echo "  $p $(readlink /proc/$p/exe)"; done
             echo "AppImage runtimes:"; runtimes | sed 's/^/  /'; }
wait_for() { local what=$1 t=$2; shift 2; for _ in $(seq 1 "$t"); do "$@" && return 0; sleep 1; done; fail "$what (waited $t s)"; return 1; }
installed() { [ "$(cat /opt/usbeam/installed 2>/dev/null)" = "$1" ]; }
one_app()   { [ "$(apps | wc -l)" = 1 ]; }

# What must be true while USBeam runs: one app, one runtime (the installed copy, not a replaced one).
check_clean() {
    sleep 15                          # anything left over has had time to end
    show
    [ "$(apps | wc -l)" = 1 ] || fail "$(apps | wc -l) USBeam apps running, expected 1"
    local r; r=$(runtimes)
    [ "$(echo "$r" | grep -c .)" = 1 ] || fail "$(echo "$r" | grep -c .) AppImage runtimes running, expected 1"
    echo "$r" | grep -qx "[0-9]* /opt/usbeam/USBeam.AppImage" || fail "the running AppImage isn't the installed copy"
    ls "$HOME/.cache/USBeam/"*.AppImage >/dev/null 2>&1 && fail "a downloaded update was left in the cache: $(ls "$HOME/.cache/USBeam/")"
    systemctl is-active --quiet usbeam.service || fail "the USBeam service isn't running"
}

# The password prompt: sudo instead (no one to type a password here).
sudo tee /usr/local/bin/pkexec >/dev/null <<'EOF'
#!/bin/sh
exec sudo -n "$@"
EOF
sudo chmod 755 /usr/local/bin/pkexec

say "1. Install the previous release ($(basename "$OLD"))"
mkdir -p "$HOME/Downloads"; cp "$OLD" "$HOME/Downloads/USBeam-old.AppImage"; chmod +x "$HOME/Downloads/USBeam-old.AppImage"
start "$HOME/Downloads/USBeam-old.AppImage" --install
wait_for "the previous release didn't install" 180 test -f /opt/usbeam/installed
wait_for "the previous release's app didn't start" 60 one_app
show

say "2. Update it to $NEWVER the way the previous release does"
# The previous release starts the downloaded file and then closes; the installer it starts gets the
# old app's open files (its AppImage runtime's pipe and folder). Recreate that: start the new file
# with copies of those files, then close the old app.
old_app=$(apps | head -n 1)
mkdir -p "$HOME/.cache/USBeam"; cp "$NEW" "$HOME/.cache/USBeam/USBeam-$NEWVER.AppImage"; chmod +x "$HOME/.cache/USBeam/USBeam-$NEWVER.AppImage"
inherit=""; n=10
for fd in /proc/"$old_app"/fd/*; do
    t=$(readlink "$fd") || continue
    case "$t" in pipe:*|/tmp/.mount_*) inherit="$inherit exec $n<$fd;"; n=$((n + 1));; esac
done
echo "Passing on $((n - 10)) of the old app's open files"
setsid bash -c "$inherit exec \"\$0\" --install" "$HOME/.cache/USBeam/USBeam-$NEWVER.AppImage" </dev/null >>"$LOG/app.log" 2>&1 &
sleep 2; [ -n "$old_app" ] && kill "$old_app"
wait_for "$NEWVER didn't install" 180 installed "$NEWVER"
wait_for "the app didn't start after the update" 60 one_app
check_clean

for n in 1 2; do
    say "3.$n. Update again from inside the running app (same code path as its Update button)"
    before=$(apps)
    cp "$NEW" "$HOME/.cache/USBeam/USBeam-test.AppImage"; chmod +x "$HOME/.cache/USBeam/USBeam-test.AppImage"
    /opt/usbeam/USBeam.AppImage --test-update "$HOME/.cache/USBeam/USBeam-test.AppImage" >>"$LOG/app.log" 2>&1 || fail "the running app didn't take the update request"
    wait_for "the old app didn't close for the update" 60 bash -c "! kill -0 $before 2>/dev/null"
    wait_for "the app didn't start again after the update" 120 one_app
    check_clean
done

say "4. Start the app four more times (three at once): still one copy"
for _ in 1 2 3; do start /opt/usbeam/USBeam.AppImage; done
sleep 2; start /opt/usbeam/USBeam.AppImage --tray
check_clean

say "5. Uninstall from inside the app"
/opt/usbeam/USBeam.AppImage --test-uninstall >>"$LOG/app.log" 2>&1 || fail "the running app didn't take the uninstall request"
no_app() { [ -z "$(apps)" ]; }
wait_for "the app is still running after uninstalling" 60 no_app
sleep 10
show
[ -z "$(runtimes)" ] || fail "AppImage runtimes still running after uninstalling"
for f in /opt/usbeam /etc/systemd/system/usbeam.service /etc/modules-load.d/usbeam.conf /etc/polkit-1/rules.d/49-usbeam.rules \
         /usr/local/share/applications/usbeam.desktop /etc/xdg/autostart/usbeam.desktop \
         /usr/local/share/icons/hicolor/scalable/apps/usbeam.svg /var/lib/usbeam /run/usbeam "$HOME/.cache/USBeam"; do
    sudo test -e "$f" && fail "left behind after uninstalling: $f"
done
systemctl is-active --quiet usbeam.service && fail "the USBeam service still runs after uninstalling"
pgrep -x usbeam >/dev/null && fail "the USBeam engine still runs after uninstalling"

echo; echo "== app output"; cat "$LOG/app.log"
[ $FAIL = 0 ] && { echo; echo "PASS: Linux upgrade test"; exit 0; }
echo; echo "Linux upgrade test FAILED"; exit 1
