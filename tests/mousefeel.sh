#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Mouse "feel" test with the real Linux kernel parts (needs root; meant for GitHub Actions).
# A virtual USB mouse (dummy_hcd + HID gadget) moves in a straight line at 1000 reports a second.
# USBeam shares it from a separate network namespace whose link has home-Wi-Fi-like delay
# (~2.7 ms each way, uneven), and USBeam on this side uses it. Measured on the mouse this side
# sees: reports per second, how even their timing is, and that the line arrives exactly straight
# and complete. Runs the old way (USBEAM_NOSTREAM=1) and the new way; passes only if the new way
# is smooth and the old way is clearly worse (so the test really sees the problem).
set -u
cd "$(dirname "$0")/.." || exit 1
# Everything shown is also kept, and handed to GitHub as one note at the end (readable without logging in).
OUT=$(mktemp)
exec > >(tee -a "$OUT") 2>&1
note() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::notice title=Mouse feel test::$(tail -n 80 "$OUT" | sed 's/%/%25/g; s/\r//g' | awk 'BEGIN{ORS="%0A"} {print}')"; }
trap note EXIT
trap 'echo "FAIL: the test took over 8 minutes"; exit 1' TERM
( sleep 480; kill -TERM $$ ) 2>/dev/null &
N=3000
fail() { echo "FAIL: $*"; cleanup; exit 1; }
G=/sys/kernel/config/usb_gadget/usbeamtest
cleanup() {
    pkill -f "build/usbeam use 10.77.0.2" 2>/dev/null; sleep 1
    ip netns pids usbeamhost 2>/dev/null | xargs -r kill 2>/dev/null; sleep 1
    ip netns del usbeamhost 2>/dev/null
    if [ -d $G ]; then echo "" > $G/UDC 2>/dev/null; rm -f $G/configs/c.1/hid.usb0; rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/hid.usb0 $G/strings/0x409 $G 2>/dev/null; fi
}

echo "== kernel parts"
for m in dummy_hcd libcomposite usb_f_hid usbip-host vhci-hcd usbhid; do
    modinfo -n $m >/dev/null 2>&1 && echo "have $m" || echo "missing $m"
done
apt-cache search --names-only "linux-(modules|headers).*$(uname -r)" 2>/dev/null | head -5
if ! modprobe dummy_hcd 2>/dev/null; then
    # Not shipped with this kernel: build the virtual USB controller from the kernel's own source.
    K=$(uname -r); V=$(echo "$K" | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')
    apt-get install -y -q "linux-headers-$K" >/dev/null 2>&1
    D=$(mktemp -d)
    curl -fsSL -o "$D/dummy_hcd.c" "https://raw.githubusercontent.com/torvalds/linux/v$V/drivers/usb/gadget/udc/dummy_hcd.c" || fail "couldn't get dummy_hcd.c for $V"
    echo "obj-m := dummy_hcd.o" > "$D/Makefile"
    make -s -C "/lib/modules/$K/build" M="$D" modules 2>&1 | tail -5
    modprobe udc-core 2>/dev/null; modprobe libcomposite
    insmod "$D/dummy_hcd.ko" || fail "couldn't load the virtual USB controller"
fi
for m in libcomposite usb_f_hid usbip-host vhci-hcd; do
    modprobe $m || fail "kernel module $m not available"
done
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

echo "== virtual mouse"
mkdir -p $G && cd $G || fail "no USB gadget support"
echo 0x1d6b > idVendor; echo 0x0104 > idProduct; echo 0x0200 > bcdUSB
mkdir -p strings/0x409; echo USBeam > strings/0x409/manufacturer; echo "Test Mouse" > strings/0x409/product; echo 1 > strings/0x409/serialnumber
mkdir -p configs/c.1/strings/0x409 functions/hid.usb0
echo 0 > functions/hid.usb0/subclass; echo 2 > functions/hid.usb0/protocol; echo 6 > functions/hid.usb0/report_length
# buttons (8 bits), X and Y (16 bits each, relative), wheel (8 bits)
printf '\x05\x01\x09\x02\xa1\x01\x09\x01\xa1\x00\x05\x09\x19\x01\x29\x08\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x05\x01\x09\x30\x09\x31\x16\x01\x80\x26\xff\x7f\x75\x10\x95\x02\x81\x06\x09\x38\x15\x81\x25\x7f\x75\x08\x95\x01\x81\x06\xc0\xc0' > functions/hid.usb0/report_desc
ln -s functions/hid.usb0 configs/c.1/
ls /sys/class/udc | head -1 > UDC || fail "no virtual USB controller"
cd - >/dev/null || exit 1
sleep 2
BUSID=""
for d in /sys/bus/usb/devices/*; do
    [ "$(cat "$d/idVendor" 2>/dev/null)" = 1d6b ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = 0104 ] && BUSID=$(basename "$d")
done
[ -n "$BUSID" ] || fail "the virtual mouse didn't appear"
echo "virtual mouse is $BUSID"

echo "== the virtual mouse plugged in directly (the goal)"
for _ in $(seq 1 15); do python3 tests/mousefeel.py find local && break; sleep 1; done
python3 tests/mousefeel.py measure "$N" "plugged in directly                " local > /tmp/mousefeel-local &
lm=$!; sleep 1
timeout 60 python3 tests/mousefeel.py move "$N" || echo "(the virtual mouse could not send all its reports within 60 s)"
wait $lm; cat /tmp/mousefeel-local

echo "== network with home-Wi-Fi-like delay"
ip netns add usbeamhost
ip link add usbeam0 type veth peer name usbeam1
ip link set usbeam1 netns usbeamhost
ip addr add 10.77.0.1/24 dev usbeam0; ip link set usbeam0 up
ip netns exec usbeamhost ip addr add 10.77.0.2/24 dev usbeam1
ip netns exec usbeamhost ip link set usbeam1 up; ip netns exec usbeamhost ip link set lo up
tc qdisc add dev usbeam0 root netem delay 2.7ms 1.5ms distribution normal rate 500mbit
ip netns exec usbeamhost tc qdisc add dev usbeam1 root netem delay 2.7ms 1.5ms distribution normal rate 500mbit
ping -c 20 -i 0.05 -q 10.77.0.2 | tail -1

echo "== USBeam sharing it (other side)"
LOG=$(mktemp -d)
(tail -f /dev/null | ip netns exec usbeamhost build/usbeam host >"$LOG/host" 2>&1) &   # (it quits when its input ends)
sleep 2

run() {   # $1 = label, $2 = 1 for the old way
    local label=$1 old=$2
    if [ "$old" = 1 ]; then USBEAM_NOSTREAM=1 build/usbeam use 10.77.0.2 "$BUSID" >"$LOG/use-$old" 2>&1 &
    else build/usbeam use 10.77.0.2 "$BUSID" >"$LOG/use-$old" 2>&1 & fi
    local up=0
    for _ in $(seq 1 30); do
        python3 tests/mousefeel.py find >/dev/null 2>&1 && { up=1; break; }
        sleep 1
    done
    [ $up = 1 ] || { cat "$LOG"/*; fail "$label: the mouse didn't arrive on this side"; }
    sleep 2
    python3 tests/mousefeel.py measure "$N" "$label" > "$LOG/result-$old" || { cat "$LOG/result-$old"; cat "$LOG"/*; fail "$label: measuring failed"; } &
    local m=$!
    sleep 1
    timeout 60 python3 tests/mousefeel.py move "$N" || echo "(the virtual mouse could not send all its reports within 60 s)"
    wait $m
    echo "--- what USBeam said on this side:"; tail -5 "$LOG/use-$old"
    cat "$LOG/result-$old"
    pkill -f "build/usbeam use 10.77.0.2"; sleep 3
}
run "old way (one report per round trip)" 1
run "new way (streamed)                 " 0

read -r old_rate old_p90 _ < <(awk '/^NUMBERS/ {print $2, $3, $4}' "$LOG/result-1")
read -r new_rate new_p90 new_ok < <(awk '/^NUMBERS/ {print $2, $3, $5}' "$LOG/result-0")
read -r loc_rate loc_p90 _ < <(awk '/^NUMBERS/ {print $2, $3, $5}' /tmp/mousefeel-local)
ok=1
awk -v r="$new_rate" -v l="$loc_rate" 'BEGIN{exit !(r >= 0.9 * l)}' && echo "OK: new way: $new_rate reports/s (plugged in directly: $loc_rate)" || { echo "FAIL: new way: only $new_rate reports/s (plugged in directly: $loc_rate)"; ok=0; }
awk -v p="$new_p90" -v l="$loc_p90" 'BEGIN{exit !(p <= l + 1)}' && echo "OK: new way: 90% of reports within $new_p90 ms (plugged in directly: $loc_p90)" || { echo "FAIL: new way: 90% of reports within $new_p90 ms (plugged in directly: $loc_p90)"; ok=0; }
[ "$new_ok" = 1 ] && echo "OK: new way: the line arrived complete and exactly straight" || { echo "FAIL: new way: movement lost or bent"; ok=0; }
awk -v o="$old_p90" -v n="$new_p90" 'BEGIN{exit !(o >= 2 * n)}' && echo "OK: the old way is clearly worse ($old_p90 ms, $old_rate reports/s): the test sees the problem" || { echo "FAIL: the old way wasn't clearly worse ($old_p90 ms)"; ok=0; }
cleanup
[ $ok = 1 ] && echo "PASS: mouse feel test" && exit 0
echo "--- logs"; cat "$LOG/host" "$LOG"/use-* | tail -40
exit 1
