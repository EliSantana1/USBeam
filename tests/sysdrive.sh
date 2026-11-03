#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# System-drive test (Linux, needs root): USBeam must treat the disk the running system lives on as
# untouchable, including through encryption/LVM-style stacking, and must NOT treat an ordinary
# drive that is merely mounted (for example under /mnt or /media) as a system drive.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
gcc -O2 -Wall -Werror -o build/sysdrive_test tests/sysdrive_test.c || exit 1
T=build/sysdrive_test
FAIL=0
expect() {  # expect <system|in-use-unknown|not-system> <block name> <what>
    got=$($T block "$2" | awk '{print $2}')
    if [ "$got" = "$1" ]; then echo "OK: $3 ($2): $got"; else echo "FAIL: $3 ($2): $got, expected $1"; FAIL=1; fi
}

# 1. The disk holding / (and its partition) must be a system drive. Found by device number, so it
#    works for btrfs subvolumes, device-mapper and similar too.
mm=$(findmnt -no MAJ:MIN / | head -n 1 | tr -d ' ')
if [ -n "$mm" ] && [ -e "/sys/dev/block/$mm" ]; then
    part=$(basename "$(readlink -f "/sys/dev/block/$mm")")
    expect system "$part" "the block device holding /"
    disk=$(lsblk -srno NAME "/dev/$part" 2>/dev/null | tail -n 1)
    [ -n "$disk" ] && [ "$disk" != "$part" ] && expect system "$disk" "the disk holding /"
elif findmnt -no FSTYPE / | grep -qE '^(overlay|tmpfs|9p|virtiofs|zfs|nfs)'; then
    echo "note: / is $(findmnt -no FSTYPE /), not on a local block device; skipping that part"
else
    echo "FAIL: couldn't find the block device holding / (MAJ:MIN '$mm')"; FAIL=1
fi

# 2. A spare disk image: not a system drive, also while mounted somewhere ordinary.
IMG=$(mktemp /tmp/usbeam-sysdrive.XXXXXX)
truncate -s 64M "$IMG"
LOOP=$(losetup -f --show "$IMG") || { echo "FAIL: no loop device"; exit 1; }
L=$(basename "$LOOP")
# shellcheck disable=SC2317  # (run by the EXIT trap)
cleanup() {
    umount -q /mnt/usbeam-test /srv/usbeam-test /opt/usbeam-test 2>/dev/null
    dmsetup remove usbeam-test 2>/dev/null
    losetup -d "$LOOP" 2>/dev/null; rm -f "$IMG"
    rmdir /mnt/usbeam-test /srv/usbeam-test /opt/usbeam-test 2>/dev/null
}
trap cleanup EXIT
mkfs.ext4 -q -F "$LOOP" || { echo "FAIL: mkfs"; exit 1; }
expect not-system "$L" "a spare drive, not mounted"
mkdir -p /mnt/usbeam-test; mount "$LOOP" /mnt/usbeam-test || { echo "FAIL: mount /mnt"; exit 1; }
expect not-system "$L" "a spare drive mounted under /mnt (an ordinary drive with files open)"
umount /mnt/usbeam-test

# 3. The same drive mounted where the system keeps its files: a system drive.
mkdir -p /srv/usbeam-test; mount "$LOOP" /srv/usbeam-test || { echo "FAIL: mount /srv"; exit 1; }
expect system "$L" "a drive mounted under /srv"
umount /srv/usbeam-test || { echo "FAIL: umount /srv"; exit 1; }
expect not-system "$L" "the same drive after unmounting it from /srv"

# 4. Stacked: a device-mapper device (as used by encryption and LVM) on top of the drive, mounted
#    at a system place: the drive underneath must be protected too.
if command -v dmsetup >/dev/null; then
    echo "0 $(blockdev --getsz "$LOOP") linear $LOOP 0" | dmsetup create usbeam-test || { echo "FAIL: dmsetup"; exit 1; }
    mkdir -p /opt/usbeam-test; mount /dev/mapper/usbeam-test /opt/usbeam-test || { echo "FAIL: mount /opt"; exit 1; }
    expect system "$L" "a drive under a device-mapper device mounted under /opt"
    umount /opt/usbeam-test || { echo "FAIL: umount /opt"; exit 1; }
    # Still held by the (now unmounted) device-mapper device, by something USBeam can't name:
    # left alone while that lasts, but not as a certain system drive.
    expect in-use-unknown "$L" "a drive held by something unknown (an unmounted device-mapper device)"
    dmsetup remove usbeam-test || { echo "FAIL: dm cleanup"; exit 1; }
    expect not-system "$L" "the same drive after unmounting"
else
    echo "note: dmsetup not installed; skipping the stacked test"
fi

# 5. What USBeam does with that, over time (each "check" is like USBeam looking again later).
#    A drive locked for a moment by something unknown (as while it's being mounted) is hidden only
#    while that lasts, never for good; a certain system drive stays protected even after unmounting.
check() {  # check <hidden|shown> <what>
    got=$($T check "$L" 2>/dev/null | awk '{print $2}')
    if [ "$got" = "$1" ]; then echo "OK: $2 ($L): $got"; else echo "FAIL: $2 ($L): $got, expected $1"; FAIL=1; fi
}
rm -rf /tmp/usbeam-sysdrive-test
python3 -c 'import os,sys,time; fd=os.open(sys.argv[1], os.O_RDONLY | os.O_EXCL); time.sleep(30)' "$LOOP" &
HOLD=$!; sleep 1
check hidden "a drive locked by something unknown, right now"
kill "$HOLD"; wait "$HOLD" 2>/dev/null
check shown "the same drive once the lock is gone (not remembered as a system drive)"
mkdir -p /mnt/usbeam-test; mount "$LOOP" /mnt/usbeam-test || { echo "FAIL: mount /mnt"; exit 1; }
check shown "the same drive, now mounted normally under /mnt"
umount /mnt/usbeam-test
mkdir -p /srv/usbeam-test; mount "$LOOP" /srv/usbeam-test || { echo "FAIL: mount /srv"; exit 1; }
check hidden "a drive mounted under /srv (certain)"
umount /srv/usbeam-test || { echo "FAIL: umount /srv"; exit 1; }
check hidden "the same drive after unmounting it from /srv (still protected until a restart)"
rm -rf /tmp/usbeam-sysdrive-test

[ $FAIL = 0 ] && { echo "PASS: system-drive test"; exit 0; }
echo "System-drive test FAILED"; exit 1
