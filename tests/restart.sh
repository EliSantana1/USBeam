#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Restart test with the real Linux kernel parts (needs root; meant for GitHub Actions).
# Some devices restart themselves while the PC they were handed to sets them up (a Bluetooth chip
# reloading its firmware drops off USB and comes back a moment later). That must not count as the
# user unplugging it. A virtual USB device (dummy_hcd + HID gadget) is shared by USBeam from a
# separate network namespace and used on this side, then "restarts" (disconnects and reconnects):
#   1. right after being handed over: kept for this side, not used by its own side meanwhile,
#      and connected here again
#   2. right after being handed over, but never connected again: back to its own side after 15 s
#   3. long after being handed over (the user replugging it): back to its own side, with the
#      countdown, as always
#   4. right after being handed over, but gone for longer than a restart takes: an unplug, as always
#   5. let go of normally (usbip-host resets it then), six times: never taken for an unplug, back
#      on its own side each time, and off usbip-host's list
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=$(mktemp)
exec > >(tee -a "$OUT") 2>&1
note() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::notice title=Restart test::$(tail -n 140 "$OUT" | sed 's/%/%25/g; s/\r//g' | awk 'BEGIN{ORS="%0A"} {print}')"; }
# At the end: the time limit below and the background jobs stop too (nothing keeps running after it).
# (Each job first, then what it started: stopped the other way round, the time limit would fire.)
done_bg() {
    local j kids
    for j in ${WD:-} ${HOSTJOB:-}; do
        kids=$(pgrep -P "$j")
        kill "$j" 2>/dev/null
        [ -n "$kids" ] && kill $kids 2>/dev/null
    done
    return 0
}
trap 'note; done_bg' EXIT
trap 'echo "FAIL: the test took over 8 minutes"; exit 1' TERM
{ sleep 480; kill -TERM $$; } >/dev/null 2>&1 &
WD=$!
G=/sys/kernel/config/usb_gadget/usbeamrestart
LOG=$(mktemp -d)
fails=0
check() { if eval "$1"; then echo "OK: $2"; else echo "FAIL: $2"; fails=$((fails + 1)); fi; }
stop_use() { pkill -f "build/usbeam use 10.78.0.2" 2>/dev/null; sleep 2; }
cleanup() {
    stop_use
    ip netns pids usbeamrs 2>/dev/null | xargs -r kill 2>/dev/null; sleep 1
    [ -n "${HOSTJOB:-}" ] && { pkill -P "$HOSTJOB"; kill "$HOSTJOB"; } 2>/dev/null
    ip netns del usbeamrs 2>/dev/null
    if [ -d $G ]; then echo "" > $G/UDC 2>/dev/null; rm -f $G/configs/c.1/hid.usb0; rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/hid.usb0 $G/strings/0x409 $G 2>/dev/null; fi
}
fail() {
    echo "FAIL: $*"; tail -n 20 "$LOG/host" 2>/dev/null
    echo "--- driver: $(readlink "/sys/bus/usb/devices/${BUSID:-x}/driver" 2>/dev/null); usbip-host table: $(tr '\n' ' ' < /sys/bus/usb/drivers/usbip-host/match_busid 2>/dev/null)"
    dmesg | tail -n 25
    cleanup; exit 1
}

echo "== kernel parts"
if [ ! -d /sys/module/dummy_hcd ] && ! modprobe dummy_hcd 2>/dev/null; then
    # Not shipped with this kernel: build the virtual USB controller from the kernel's own source.
    K=$(uname -r); V=$(echo "$K" | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')
    apt-get install -y -q "linux-headers-$K" >/dev/null 2>&1
    D=$(mktemp -d)
    curl -fsSL --retry 5 --retry-delay 3 -o "$D/dummy_hcd.c" "https://raw.githubusercontent.com/torvalds/linux/v$V/drivers/usb/gadget/udc/dummy_hcd.c" || fail "couldn't get dummy_hcd.c for $V"
    echo "obj-m := dummy_hcd.o" > "$D/Makefile"
    make -s -C "/lib/modules/$K/build" M="$D" modules 2>&1 | tail -5
    modprobe udc-core 2>/dev/null; modprobe libcomposite
    insmod "$D/dummy_hcd.ko" || fail "couldn't load the virtual USB controller"
fi
for m in libcomposite usb_f_hid usbip-host vhci-hcd; do
    modprobe $m || fail "kernel module $m not available"
done
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

echo "== virtual device"
uses_before=$(dmesg | grep -c "USBeam Test Mouse\] on usb-dummy_hcd")
mkdir -p $G && cd $G || fail "no USB gadget support"
echo 0x1d6b > idVendor; echo 0x0104 > idProduct; echo 0x0200 > bcdUSB
mkdir -p strings/0x409; echo USBeam > strings/0x409/manufacturer; echo "Test Mouse" > strings/0x409/product; echo 1 > strings/0x409/serialnumber
mkdir -p configs/c.1/strings/0x409 functions/hid.usb0
echo 0 > functions/hid.usb0/subclass; echo 2 > functions/hid.usb0/protocol; echo 6 > functions/hid.usb0/report_length
printf '\x05\x01\x09\x02\xa1\x01\x09\x01\xa1\x00\x05\x09\x19\x01\x29\x08\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x05\x01\x09\x30\x09\x31\x16\x01\x80\x26\xff\x7f\x75\x10\x95\x02\x81\x06\x09\x38\x15\x81\x25\x7f\x75\x08\x95\x01\x81\x06\xc0\xc0' > functions/hid.usb0/report_desc
ln -s functions/hid.usb0 configs/c.1/
UDC=$(ls /sys/class/udc | head -1)
echo "$UDC" > UDC || fail "no virtual USB controller"
cd - >/dev/null || exit 1
sleep 2
BUSID=""
for d in /sys/bus/usb/devices/*; do
    [ "$(cat "$d/idVendor" 2>/dev/null)" = 1d6b ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = 0104 ] && BUSID=$(basename "$d")
done
[ -n "$BUSID" ] || fail "the virtual device didn't appear"
echo "virtual device is $BUSID"
restart_device() { echo "" > $G/UDC; sleep "$1"; echo "$UDC" > $G/UDC; }   # $1 = seconds off the bus
driver() { basename "$(readlink "/sys/bus/usb/devices/$BUSID/driver" 2>/dev/null)" 2>/dev/null; }
here() { python3 tests/mousefeel.py find local >/dev/null 2>&1; }      # working on its own side
there() { python3 tests/mousefeel.py find >/dev/null 2>&1; }           # arrived on the using side
wait_for() { for _ in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
in_log() { grep -q "$1" "$LOG/host"; }
local_uses() { dmesg | grep -c "USBeam Test Mouse\] on usb-dummy_hcd"; }   # times its own side's driver took it

wait_for here 15
seen_locally() { [ "$(local_uses)" -gt "$uses_before" ]; }
check seen_locally "the test can see when its own side's driver takes it"

echo "== USBeam sharing it (other side)"
ip netns add usbeamrs
ip link add usbeamr0 type veth peer name usbeamr1
ip link set usbeamr1 netns usbeamrs
ip addr add 10.78.0.1/24 dev usbeamr0; ip link set usbeamr0 up
ip netns exec usbeamrs ip addr add 10.78.0.2/24 dev usbeamr1
ip netns exec usbeamrs ip link set usbeamr1 up; ip netns exec usbeamrs ip link set lo up
(tail -f /dev/null | ip netns exec usbeamrs build/usbeam host >"$LOG/host" 2>&1) &   # (it quits when its input ends)
HOSTJOB=$!
sleep 2
use() { build/usbeam use 10.78.0.2 "$BUSID" >>"$LOG/use" 2>&1 & wait_for there 30 || fail "the device didn't arrive on the using side"; }

echo "== 1. restarts right after being handed over"
use
uses=$(local_uses)
no_new_local_use() { [ "$(local_uses)" = "$uses" ]; }
restart_device 1
wait_for 'in_log "back after restarting"' 10
check 'in_log "restarted itself while"' "the restart isn't taken for an unplug"
check 'in_log "back after restarting"' "it's kept for the PC using it"
check '[ "$(driver)" = usbip-host ] && ! here && no_new_local_use' "meanwhile its own side doesn't use it, not even for a moment"
stop_use; use
check 'in_log "is now in use by 10.78.0.1 again"' "the PC using it connects it again"
check 'there' "and it works there"

echo "== 2. restarts right after being handed over, never connected again"
stop_use; use
restart_device 1
stop_use
wait_for 'in_log "connect it again: it works on this PC again"' 25
check 'in_log "connect it again: it works on this PC again"' "after 15 s it goes back to its own side"
check 'wait_for here 10' "and works there"

echo "== 3. replugged long after being handed over"
use
sleep 12
restart_device 1
wait_for 'in_log "plugged back in and works on this PC"' 10
check 'in_log "plugged back in and works on this PC"' "a replug brings it back to its own side, as always"
check 'wait_for here 10' "and works there"
stop_use

echo "== 4. gone for longer than a restart right after being handed over"
sleep 12      # (the countdown from step 3)
replugs() { grep -c "plugged back in and works on this PC" "$LOG/host"; }
before=$(replugs)
returned_again() { [ "$(replugs)" -gt "$before" ]; }
use
restart_device 8
wait_for returned_again 10
check returned_again "an unplug after all: back to its own side, as always"
check 'wait_for here 10' "and works there"

echo "== 5. let go of normally, six times: back to its own side every time"
# usbip-host resets the device when the other side lets go; during a reset Linux shows its USB
# address as 0 for a moment. That must never be taken for an unplug.
sleep 12      # (the countdown from step 4)
unplugs() { grep -c "unplugged while" "$LOG/host"; }
released() { grep -c "released by 10.78.0.1" "$LOG/host"; }
u0=$(unplugs)
let_go_seen() { [ "$(released)" -gt "$r0" ]; }
no_new_unplug() { [ "$(unplugs)" = "$u0" ]; }
for n in $(seq 1 6); do
    r0=$(released)
    use
    sleep 2
    stop_use
    wait_for let_go_seen 10 || fail "round $n: the let-go wasn't noticed"
    wait_for here 10 || fail "round $n: it didn't work on its own side again"
done
check no_new_unplug "a normal let-go is never taken for an unplug"
check '! tr " " "\n" < /sys/bus/usb/drivers/usbip-host/match_busid | grep -qx "$BUSID"' "and it's off usbip-host's list"

echo "--- what USBeam said"
cat "$LOG/host" | grep -v -E "listed devices" | tail -40
cleanup
[ $fails = 0 ] && echo "PASS: restart test" && exit 0
exit 1
