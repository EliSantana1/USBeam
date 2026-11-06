# SPDX-License-Identifier: GPL-3.0-or-later
# Helper for tests/mousefeel.sh.
#   find              exit 0 when the shared test mouse exists on this side (behind vhci)
#   move N            the virtual mouse sends N reports, 1 ms apart: X +2, Y +1 each
#   measure N label   records the shared mouse until N reports arrived (or 30 s), prints numbers
import glob, os, select, struct, sys, time

def shared_mouse(local=False):   # the test mouse as USBeam delivers it (behind vhci), or plugged in directly
    for d in glob.glob("/sys/class/input/event*"):
        try:
            if open(d + "/device/name").read().strip() != "USBeam Test Mouse": continue
            if ("vhci_hcd" in os.path.realpath(d)) != local: return "/dev/input/" + os.path.basename(d)
        except OSError: pass
    return None

cmd = sys.argv[1]
if cmd == "find": sys.exit(0 if shared_mouse(len(sys.argv) > 2) else 1)

if cmd == "move":
    n = int(sys.argv[2]); fd = os.open("/dev/hidg0", os.O_WRONLY)
    rep = struct.pack("<BhhB", 0, 2, 1, 0); t = t0 = time.monotonic(); w = []
    for i in range(n):
        a = time.monotonic()
        os.write(fd, rep)          # waits while the previous report hasn't been picked up
        w.append((time.monotonic() - a) * 1000)
        t += 0.001
        d = t - time.monotonic()
        if d > 0: time.sleep(d)
    w.sort()
    print(f"  (virtual mouse: {n / (time.monotonic() - t0):.0f} reports/s sent; waiting to send one: median {w[len(w) // 2]:.2f} ms, p90 {w[int(0.9 * len(w))]:.2f} ms)")
    sys.exit(0)

if cmd == "measure":
    n, label = int(sys.argv[2]), sys.argv[3]
    fd = os.open(shared_mouse(len(sys.argv) > 4), os.O_RDONLY | os.O_NONBLOCK)
    times, dx, dy, cur = [], 0, 0, [0, 0]
    end = time.monotonic() + 60
    while len(times) < n and time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r: continue
        data = os.read(fd, 24 * 512)
        for i in range(0, len(data) - 23, 24):
            s, us, t, c, v = struct.unpack("qqHHi", data[i:i + 24])
            if t == 2 and c == 0: cur[0] += v
            elif t == 2 and c == 1: cur[1] += v
            elif t == 0 and c == 0 and cur != [0, 0]:
                times.append(s + us / 1e6)
                if cur != [2, 1]: print("report with unexpected movement:", cur)
                dx += cur[0]; dy += cur[1]; cur = [0, 0]
    k = len(times)
    if k < 10: print(f"{label}: only {k} reports arrived"); sys.exit(1)
    gaps = sorted((b - a) * 1000 for a, b in zip(times, times[1:]))
    rate = (k - 1) / (times[-1] - times[0])
    q = lambda f: gaps[int(f * (len(gaps) - 1))]
    straight = int(k == n and dx == 2 * n and dy == n)
    print(f"{label}: {rate:.0f} reports/s, gap ms median {q(0.5):.2f} p90 {q(0.9):.2f} p99 {q(0.99):.2f} max {gaps[-1]:.2f}; "
          f"arrived {k}/{n}, total move {dx},{dy} (expected {2 * n},{n}), gaps over 20 ms: {sum(g > 20 for g in gaps)}")
    print(f"NUMBERS {rate:.0f} {q(0.9):.2f} {q(0.99):.2f} {straight}")
