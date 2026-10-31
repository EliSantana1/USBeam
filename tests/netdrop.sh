#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Network-drop test (Linux, needs root). In a private network namespace, two programs connect with
# USBeam's connection settings (src/linux/netsock.h) and keep sending, like a game controller in
# use. Then every packet between them is dropped, as when Wi-Fi goes away. Passes if both ends
# notice within 35 s. (Without TCP_USER_TIMEOUT it takes up to ~15 minutes; the test stops at 90 s.)
set -u
cd "$(dirname "$0")/.." || exit 1
if [ "${1:-}" != "--inside" ]; then
    mkdir -p build
    gcc -O2 -Wall -Werror -o build/netdrop tests/netdrop.c || exit 1
    exec unshare --net "$0" --inside
fi
LIMIT=35
# Bring up the namespace's own loopback network.
if command -v ip >/dev/null; then ip link set lo up
else python3 -c "
import fcntl, socket, struct
s = socket.socket(); req = struct.pack('16sH', b'lo', 0)
flags = struct.unpack('16sH', fcntl.ioctl(s, 0x8913, req))[1]
fcntl.ioctl(s, 0x8914, struct.pack('16sH', b'lo', flags | 1))"
fi
LOG=$(mktemp -d)
timeout 120 build/netdrop server 127.0.0.1 3240 >"$LOG/server" 2>&1 &
sleep 1
timeout 120 build/netdrop client 127.0.0.1 3240 >"$LOG/client" 2>&1 &
sleep 3
grep -q connected "$LOG/server" && grep -q connected "$LOG/client" || { echo "FAIL: no connection"; cat "$LOG"/*; exit 1; }
echo "Connected. Now every packet is dropped, without any answer."
iptables -I INPUT -p tcp --dport 3240 -j DROP && iptables -I INPUT -p tcp --sport 3240 -j DROP || exit 1
for _ in $(seq 1 90); do
    grep -q RESULT "$LOG/server" && grep -q RESULT "$LOG/client" && break
    sleep 1
done
cat "$LOG/server" "$LOG/client"
ok=1
for side in server client; do
    t=$(awk -v s=$side '$1=="RESULT" && $2==s {print $3}' "$LOG/$side")
    if [ -z "$t" ]; then echo "FAIL: the $side didn't notice the dead network within 90 s"; ok=0
    elif awk -v t="$t" -v l=$LIMIT 'BEGIN{exit !(t > l)}'; then echo "FAIL: the $side took $t s (limit $LIMIT s)"; ok=0
    else echo "OK: the $side noticed after $t s"; fi
done
[ $ok = 1 ] && echo "PASS: network-drop test" && exit 0
exit 1
