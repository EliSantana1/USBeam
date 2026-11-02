#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Sleep test (Linux): USBeam's sleep handling against a stand-in for systemd-logind on a private bus.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
gcc -O2 -Wall -Werror -pthread -o build/sleep_test tests/sleep_test.c || exit 1
D=$(mktemp -d)
cat > "$D/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$D/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow own="*"/><allow send_type="method_call"/><allow send_type="signal"/>
<allow send_type="method_return"/><allow send_type="error"/><allow send_destination="*"/>
<allow receive_type="method_call"/><allow receive_type="method_return"/><allow receive_type="error"/><allow receive_type="signal"/></policy></busconfig>
CONF
dbus-daemon --config-file="$D/bus.conf" --fork --print-pid > "$D/pid" || { echo "FAIL: dbus-daemon"; exit 1; }
sleep 0.3
USBEAM_TEST=1 USBEAM_TEST_DBUS="$D/bus" timeout 60 build/sleep_test; rc=$?
kill "$(cat "$D/pid")" 2>/dev/null; rm -rf "$D"
exit $rc
