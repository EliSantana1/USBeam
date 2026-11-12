// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// btkeys.h - a Bluetooth adapter's pairings go with it between Linux PCs (Linux only; included by
// app_linux.c after pair.h).
//
// A PC doesn't keep Bluetooth pairings in the adapter: BlueZ keeps them per adapter address in
// /var/lib/bluetooth/<adapter>/<device>/info, and reads them when an adapter appears. So:
//   - Before a PC uses a Bluetooth adapter from another Linux PC (Use, Give To), it asks that PC
//     for the adapter's pairings and puts them in place, then connects the adapter.
//   - When it lets go of the adapter, it sends the pairings back, with whatever was paired, changed
//     or removed meanwhile, and removes its copy. Until then the PC the adapter is plugged into holds
//     the adapter back (up to BT_WAIT s), so it starts with the new pairings, not the old ones.
//   - If they can't be sent back right away (network gone, PC switched off), it keeps trying.
// The PC the adapter is plugged into holds the one true copy; only the PC that had the adapter can
// send pairings back, and only for that adapter.
// Only between paired PCs: every request answers a challenge with the pair key, and the pairings
// travel encrypted (XChaCha20-Poly1305, with a key made from the pair key and that challenge).
// Windows keeps pairings its own way and runs one Bluetooth adapter at a time, so Windows PCs
// never take part: their adapters simply don't bring pairings along.

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>

#define OP_REQ_BT 0x8FE6          // USBeam extension: a Bluetooth adapter's pairings (paired PCs only)
#define OP_REP_BT 0x0FE6
#define BT_MAXDEV 64              // paired devices per adapter
#define BT_MAXINFO 16384          // size of one device's pairing file
#define BT_MAXBLOB (32 + BT_MAXDEV * (17 + 4 + BT_MAXINFO))
#define BT_MAXSESS 16
#define BT_NOTE_LINUX "its Bluetooth pairings go with it to Linux PCs paired with this one (not to Windows PCs: Windows can't take them)"
enum { BTC_GET = 1, BTC_PUT = 2, BTC_LEAVE = 3 };   // LEAVE: its own PC takes it back: disconnect what is connected through it
enum { BTS_NONE = 0, BTS_HERE = 1, BTS_SEND = 2 };   // pairings put in place here / to be sent back

static char g_bt_store[64] = "/var/lib/bluetooth";
static int g_bt_hostdirty;        // which PC has which adapter's pairings changed: save the settings

// This PC using another PC's adapter: whose pairings it holds, and whether they still go back.
typedef struct { int state; char pc[40], busid[32], addr[18]; unsigned long long next_try; } BtSess;
static BtSess g_bts[BT_MAXSESS];
static pthread_mutex_t g_bt_mx = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------- small helpers
static int bt_valid_addr(const char *a) {        // "AA:BB:CC:DD:EE:FF", upper case
    if (strlen(a) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (a[i] != ':') return 0; }
        else if (!((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'A' && a[i] <= 'F'))) return 0;
    }
    return 1;
}
static int bt_valid_busid(const char *b) {      // "1-11.4": digits, then '-', then digits and dots
    const char *d = strchr(b, '-');
    if (!d || d == b || !d[1] || strlen(b) > 31) return 0;
    for (const char *c = b; *c; c++) if (!isdigit((unsigned char)*c) && !(c == d) && !(c > d + 1 && *c == '.' && c[-1] != '.' && c[1])) return 0;
    return 1;
}
static int bt_have_bluez(void) { struct stat st; return stat(g_bt_store, &st) == 0 && S_ISDIR(st.st_mode); }

// A Bluetooth adapter: an interface of class e0 (wireless), subclass 01 (RF), protocol 01 (Bluetooth).
static int is_bt_adapter(const char *busid) {
    unsigned char ifc[32][3]; int n = dev_ifaces(busid, ifc, 32);
    for (int i = 0; i < n; i++) if (ifc[i][0] == 0xe0 && ifc[i][1] == 0x01 && ifc[i][2] == 0x01) return 1;
    return 0;
}

// The address of the adapter plugged in at busid (it must be working on this PC). 0 = found.
struct bt_devinfo { uint16_t dev_id; char name[8]; uint8_t bdaddr[6]; uint8_t rest[256]; };
#define BT_HCIGETDEVINFO _IOR('H', 211, int)
static int bt_adapter_addr(const char *busid, char out[18]) {
    char pat[PATH_MAX]; glob_t g;
    snprintf(pat, sizeof pat, USBDEV "/%s/%s:*/bluetooth/hci*", busid, busid);
    if (glob(pat, 0, NULL, &g) != 0) return -1;
    int idx = -1;
    for (size_t i = 0; i < g.gl_pathc && idx < 0; i++) {
        const char *b = strrchr(g.gl_pathv[i], '/');
        if (b && !strncmp(b + 1, "hci", 3) && isdigit((unsigned char)b[4])) idx = atoi(b + 4);
    }
    globfree(&g);
    if (idx < 0) return -1;
    int s = socket(31 /* AF_BLUETOOTH */, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
    if (s < 0) return -1;
    struct bt_devinfo di; memset(&di, 0, sizeof di); di.dev_id = (uint16_t)idx;
    int r = ioctl(s, BT_HCIGETDEVINFO, &di);
    close(s);
    if (r != 0) return -1;
    const uint8_t *a = di.bdaddr;
    if (!(a[0] | a[1] | a[2] | a[3] | a[4] | a[5])) return -1;
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return 0;
}

// ---------------------------------------------------------------- devices connected through it
// The devices connected through the adapter when it leaves a PC are connected again where it goes
// (and back), so a headset follows the adapter instead of waiting to be connected by hand.
#define BT_MAXCONN 8
typedef struct {
    char addr[18];                          // the adapter
    char conn[BT_MAXCONN][18]; int nconn;   // connected through it, as last seen on this PC
    char want[BT_MAXCONN][18]; int nwant;   // to connect again here (received with its pairings)
    unsigned long long want_until, next_try;
    unsigned long long keep_until;          // disconnected for a move: conn stays as it was until then
    int busy;                               // a connect attempt is running (bt_connect_main)
} BtConn;
static BtConn g_btc[BT_MAXSESS];
static BtConn *bt_conn(const char *addr, int create) {   // call with g_bt_mx held
    for (int i = 0; i < BT_MAXSESS; i++) if (g_btc[i].addr[0] && !strcmp(g_btc[i].addr, addr)) return &g_btc[i];
    if (!create) return NULL;
    for (int i = 0; i < BT_MAXSESS; i++) if (!g_btc[i].addr[0]) { memset(&g_btc[i], 0, sizeof g_btc[i]); snprintf(g_btc[i].addr, 18, "%s", addr); return &g_btc[i]; }
    return NULL;
}

// ---------------------------------------------------------------- the pairings, packed
// "USBEAMBT1" + adapter address + how many devices are connected (1) + their addresses (17 each),
// then per paired device: its address (17), length (4), its info file.
static void bt_put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t bt_get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int bt_pack(const char *addr, sb_t *out) {
    sb_add(out, "USBEAMBT1", 9);
    sb_add(out, addr, 17);
    char conn[BT_MAXCONN][18]; int nc = 0;
    pthread_mutex_lock(&g_bt_mx);
    BtConn *c = bt_conn(addr, 0);
    if (c) { nc = c->nconn; memcpy(conn, c->conn, sizeof conn); }
    pthread_mutex_unlock(&g_bt_mx);
    uint8_t nb = (uint8_t)nc;
    sb_add(out, (const char *)&nb, 1);
    for (int i = 0; i < nc; i++) sb_add(out, conn[i], 17);
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/%s", g_bt_store, addr);
    DIR *d = opendir(dir);
    if (!d) return 0;                                 // no pairings yet
    struct dirent *e; int n = 0;
    while ((e = readdir(d)) && n < BT_MAXDEV) {
        if (!bt_valid_addr(e->d_name)) continue;
        char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s/info", dir, e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        char *buf = malloc(BT_MAXINFO + 1); ssize_t len = buf ? read(fd, buf, BT_MAXINFO + 1) : -1;
        close(fd);
        if (len > 0 && len <= BT_MAXINFO && !memchr(buf, 0, (size_t)len)) {
            uint8_t l[4]; bt_put32(l, (uint32_t)len);
            sb_add(out, e->d_name, 17); sb_add(out, (const char *)l, 4); sb_add(out, buf, (size_t)len);
            n++;
        }
        free(buf);
    }
    closedir(d);
    return 0;
}

static int bt_write_file(const char *path, const char *data, size_t n) {   // replaced in one step
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.usbeam-new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = (size_t)write(fd, data, n) == n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}
static void bt_rmdir_all(const char *dir) {        // one device's folder: files only, no deeper folders
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

// Checks a packed set and, if it's for `addr`, makes this PC's pairings for that adapter exactly
// that set. *changed = 1 if anything differed. 0 = done.
static int bt_apply(const char *addr, const uint8_t *b, size_t n, int *changed) {
    if (changed) *changed = 0;
    if (n < 27 || memcmp(b, "USBEAMBT1", 9) || memcmp(b + 9, addr, 17) || !bt_valid_addr(addr)) return -1;
    // Check everything first: nothing is written unless the whole set is sound.
    int nc = b[26];
    if (nc > BT_MAXCONN || n < 27 + (size_t)nc * 17) return -1;
    char conn[BT_MAXCONN][18];
    for (int i = 0; i < nc; i++) { memcpy(conn[i], b + 27 + i * 17, 17); conn[i][17] = 0; if (!bt_valid_addr(conn[i])) return -1; }
    char devs[BT_MAXDEV][18]; const uint8_t *data[BT_MAXDEV]; uint32_t lens[BT_MAXDEV]; int nd = 0;
    size_t off = 27 + (size_t)nc * 17;
    while (off < n) {
        if (nd >= BT_MAXDEV || n - off < 21) return -1;
        memcpy(devs[nd], b + off, 17); devs[nd][17] = 0;
        uint32_t l = bt_get32(b + off + 17);
        if (!bt_valid_addr(devs[nd]) || l == 0 || l > BT_MAXINFO || n - off - 21 < l || memchr(b + off + 21, 0, l)) return -1;
        for (int k = 0; k < nd; k++) if (!strcmp(devs[k], devs[nd])) return -1;
        data[nd] = b + off + 21; lens[nd] = l;
        off += 21 + l; nd++;
    }
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/%s", g_bt_store, addr);
    mkdir(dir, 0700);
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    int ch = 0;
    for (int i = 0; i < nd; i++) {
        char dd[PATH_MAX], f[PATH_MAX];
        snprintf(dd, sizeof dd, "%s/%s", dir, devs[i]);
        snprintf(f, sizeof f, "%s/info", dd);
        // Unchanged: leave it alone.
        int fd = open(f, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            char *cur = malloc(BT_MAXINFO + 1); ssize_t cl = cur ? read(fd, cur, BT_MAXINFO + 1) : -1;
            close(fd);
            int same = cl == (ssize_t)lens[i] && !memcmp(cur, data[i], lens[i]);
            free(cur);
            if (same) continue;
        }
        mkdir(dd, 0700);
        if (lstat(dd, &st) != 0 || !S_ISDIR(st.st_mode) || bt_write_file(f, (const char *)data[i], lens[i]) != 0) return -1;
        ch = 1;
    }
    // Devices no longer paired (removed on the other PC).
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!bt_valid_addr(e->d_name)) continue;
            int keep = 0;
            for (int i = 0; i < nd && !keep; i++) if (!strcmp(devs[i], e->d_name)) keep = 1;
            if (keep) continue;
            char dd[PATH_MAX]; snprintf(dd, sizeof dd, "%s/%s", dir, e->d_name);
            bt_rmdir_all(dd);
            ch = 1;
        }
        closedir(d);
    }
    if (changed) *changed = ch;
    // The devices that were connected through it: connect them again here once it's working.
    pthread_mutex_lock(&g_bt_mx);
    BtConn *c = bt_conn(addr, 1);
    if (c) {
        c->nwant = 0;
        for (int i = 0; i < nc; i++) for (int k = 0; k < nd; k++) if (!strcmp(conn[i], devs[k])) { snprintf(c->want[c->nwant++], 18, "%s", conn[i]); break; }
        c->want_until = now_ms() + 90000; c->next_try = 0;
    }
    pthread_mutex_unlock(&g_bt_mx);
    return 0;
}
static void bt_forget(const char *addr) {       // remove this PC's copy of an adapter's pairings
    if (!bt_valid_addr(addr)) return;
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/%s", g_bt_store, addr);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) bt_rmdir_all(p); else unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

// ---------------------------------------------------------------- connected devices, through BlueZ
// The adapter with this address, if it's working on this PC: its hci number, else -1.
static char g_bt_class[64] = "/sys/class/bluetooth";
static int bt_hci_of(const char *addr) {
    DIR *d = opendir(g_bt_class);
    if (!d) return -1;
    struct dirent *e; int found = -1;
    int s = socket(31 /* AF_BLUETOOTH */, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
    while (s >= 0 && found < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "hci", 3) || !isdigit((unsigned char)e->d_name[3]) || strchr(e->d_name, ':')) continue;
        struct bt_devinfo di; memset(&di, 0, sizeof di); di.dev_id = (uint16_t)atoi(e->d_name + 3);
        if (ioctl(s, BT_HCIGETDEVINFO, &di) != 0) continue;
        char a[18]; const uint8_t *x = di.bdaddr;
        snprintf(a, sizeof a, "%02X:%02X:%02X:%02X:%02X:%02X", x[5], x[4], x[3], x[2], x[1], x[0]);
        if (!strcmp(a, addr)) found = di.dev_id;
    }
    if (s >= 0) close(s);
    closedir(d);
    return found;
}
// Runs busctl (systemd's D-Bus tool) with these arguments; its output in out. 0 = it succeeded.
static int bt_busctl(const char *const args[], char *out, size_t n, int timeout_s) {
    int pfd[2];
    if (pipe(pfd)) return -1;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC); fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        const char *argv[16]; int k = 0;
        argv[k++] = "busctl";
        for (int i = 0; args[i] && k < 15; i++) argv[k++] = args[i];
        argv[k] = NULL;
        execv("/usr/bin/busctl", (char *const *)argv);
        execv("/bin/busctl", (char *const *)argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t got = 0; if (out && n) out[0] = 0;
    struct pollfd pf = { pfd[0], POLLIN, 0 };
    unsigned long long end = now_ms() + (unsigned long long)timeout_s * 1000;
    for (;;) {
        unsigned long long now = now_ms();
        if (now >= end) break;
        if (poll(&pf, 1, (int)(end - now)) <= 0) break;
        char buf[256]; ssize_t r = read(pfd[0], buf, sizeof buf);
        if (r <= 0) break;
        if (out && got + 1 < n) { size_t k = (size_t)r; if (k > n - 1 - got) k = n - 1 - got; memcpy(out + got, buf, k); got += k; out[got] = 0; }
    }
    close(pfd[0]);
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); return -1; }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
static void bt_dev_path(char out[64], int hci, const char *dev) {
    char d[18]; snprintf(d, sizeof d, "%s", dev);
    for (char *c = d; *c; c++) if (*c == ':') *c = '_';
    snprintf(out, 64, "/org/bluez/hci%d/dev_%s", hci, d);
}
static int bt_is_connected(int hci, const char *dev) {
    char path[64], out[64];
    bt_dev_path(path, hci, dev);
    const char *a[] = { "--system", "get-property", "org.bluez", path, "org.bluez.Device1", "Connected", NULL };
    return bt_busctl(a, out, sizeof out, 3) == 0 && !strncmp(out, "b true", 6);
}

// A connect attempt runs on its own, so nothing else waits for the device to answer.
typedef struct { char addr[18], dev[18]; int hci; } BtConnJob;
static void *bt_connect_main(void *arg) {
    BtConnJob j = *(BtConnJob *)arg; free(arg);
    char path[64];
    bt_dev_path(path, j.hci, j.dev);
    const char *a[] = { "--system", "--timeout=12", "call", "org.bluez", path, "org.bluez.Device1", "Connect", NULL };
    int ok = bt_busctl(a, NULL, 0, 15) == 0 || bt_is_connected(j.hci, j.dev);
    pthread_mutex_lock(&g_bt_mx);
    BtConn *c = bt_conn(j.addr, 0);
    if (c) {
        c->busy = 0;
        if (ok) for (int i = 0; i < c->nwant; i++) if (!strcmp(c->want[i], j.dev)) { c->want[i][0] = 0; break; }
    }
    pthread_mutex_unlock(&g_bt_mx);
    if (ok) logm("Bluetooth device %s connected again through the adapter that came here", j.dev);
    return NULL;
}

// What is connected through the adapter right now (it must be working here): remembered for it.
static void bt_snapshot(const char *addr, int hci, char devs[BT_MAXCONN][18], int conn[BT_MAXCONN], int *ndp) {
    int nd = 0;
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/%s", g_bt_store, addr);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && nd < BT_MAXCONN && (e = readdir(d))) if (bt_valid_addr(e->d_name)) snprintf(devs[nd++], 18, "%s", e->d_name);
    if (d) closedir(d);
    for (int k = 0; k < nd; k++) conn[k] = bt_is_connected(hci, devs[k]);
    pthread_mutex_lock(&g_bt_mx);
    BtConn *c = bt_conn(addr, 1);
    if (c && now_ms() >= c->keep_until) { c->nconn = 0; for (int k = 0; k < nd; k++) if (conn[k]) snprintf(c->conn[c->nconn++], 18, "%s", devs[k]); }
    pthread_mutex_unlock(&g_bt_mx);
    *ndp = nd;
}
static void bt_snapshot_now(const char *addr) {
    int hci = bt_hci_of(addr);
    if (hci < 0) return;
    char devs[BT_MAXCONN][18]; int conn[BT_MAXCONN] = {0}, nd = 0;
    bt_snapshot(addr, hci, devs, conn, &nd);
}

// The adapter is about to move to another PC: disconnect what is connected through it, properly.
// Otherwise those devices only notice it's gone after their own timeout (often 20 s), and until
// then they don't answer the other PC. Which ones were connected is kept (keep_until), so they are
// connected again where it goes. Takes at most about 4 s. Returns how many were disconnected.
static int bt_leave(const char *addr) {
    int hci = bt_hci_of(addr);
    if (hci < 0) return 0;                       // not working on this PC
    char devs[BT_MAXCONN][18]; int conn[BT_MAXCONN] = {0}, nd = 0;
    bt_snapshot(addr, hci, devs, conn, &nd);
    char list[BT_MAXCONN][18]; int n = 0;
    pthread_mutex_lock(&g_bt_mx);
    BtConn *c = bt_conn(addr, 1);
    if (c) { c->keep_until = now_ms() + 60000; for (int k = 0; k < c->nconn; k++) snprintf(list[n++], 18, "%s", c->conn[k]); }
    pthread_mutex_unlock(&g_bt_mx);
    unsigned long long end = now_ms() + 4000;
    int done = 0;
    for (int k = 0; k < n; k++) {
        unsigned long long now = now_ms();
        if (now >= end) break;
        char path[64]; bt_dev_path(path, hci, list[k]);
        const char *a[] = { "--system", "--timeout=3", "call", "org.bluez", path, "org.bluez.Device1", "Disconnect", NULL };
        if (bt_busctl(a, NULL, 0, (int)((end - now + 999) / 1000)) == 0) done++;
    }
    if (n) logm("Bluetooth: disconnected %d of %d device(s) connected through adapter %s before it moves (so they connect again at once where it goes)", done, n, addr);
    return done;
}

// Every 2 s (every half second while something is waiting to be connected again), for the
// adapters USBeam moves (on either side): remember what is connected through them, and connect
// again what was connected where the adapter came from.
static void bt_conn_tick(void) {
    static unsigned long long next;
    unsigned long long now = now_ms();
    if (now < next) return;
    char addrs[BT_MAXSESS * 2][18]; int na = 0;
    host_lock();
    for (int i = 0; i < MAXH && na < BT_MAXSESS; i++) if (g_h[i].busid[0] && bt_valid_addr(g_h[i].btaddr)) snprintf(addrs[na++], 18, "%s", g_h[i].btaddr);
    host_unlock();
    pthread_mutex_lock(&g_bt_mx);
    for (int i = 0; i < BT_MAXSESS && na < BT_MAXSESS * 2; i++) if (g_bts[i].state == BTS_HERE) snprintf(addrs[na++], 18, "%s", g_bts[i].addr);
    int waiting = 0;
    for (int i = 0; i < BT_MAXSESS && na < BT_MAXSESS * 2; i++) if (g_btc[i].addr[0] && g_btc[i].nwant) { snprintf(addrs[na++], 18, "%s", g_btc[i].addr); waiting = 1; }
    pthread_mutex_unlock(&g_bt_mx);
    next = now + (waiting ? 500 : 2000);
    for (int i = 0; i < na; i++) {
        int dup = 0;
        for (int k = 0; k < i && !dup; k++) if (!strcmp(addrs[k], addrs[i])) dup = 1;
        if (dup) continue;
        int hci = bt_hci_of(addrs[i]);
        if (hci < 0) continue;                    // not working on this PC: keep what was last seen
        // What is connected through it now.
        char devs[BT_MAXCONN][18]; int nd = 0, conn[BT_MAXCONN] = {0};
        bt_snapshot(addrs[i], hci, devs, conn, &nd);
        pthread_mutex_lock(&g_bt_mx);
        BtConn *c = bt_conn(addrs[i], 1);
        if (c) {
            // Connect again what was connected where it came from (one device at a time).
            if (c->nwant && now >= c->want_until) {
                for (int k = 0; k < c->nwant; k++) if (c->want[k][0]) logm("Bluetooth device %s didn't connect again through the adapter that came here (gave up after 90 s)", c->want[k]);
                c->nwant = 0;
            }
            if (c->nwant && !c->busy && now >= c->next_try) {
                int pick = -1, left = 0;
                for (int k = 0; k < c->nwant; k++) {
                    if (!c->want[k][0]) continue;
                    int already = 0;
                    for (int m = 0; m < nd; m++) if (conn[m] && !strcmp(devs[m], c->want[k])) already = 1;
                    if (already) { c->want[k][0] = 0; continue; }
                    left++;
                    if (pick < 0) pick = k;
                }
                if (!left) c->nwant = 0;
                else {
                    BtConnJob *j = malloc(sizeof *j);
                    if (j) {
                        snprintf(j->addr, 18, "%s", c->addr); snprintf(j->dev, 18, "%s", c->want[pick]); j->hci = hci;
                        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                        c->busy = 1; c->next_try = now + 2000;
                        if (pthread_create(&t, &at, bt_connect_main, j)) { c->busy = 0; free(j); }
                        pthread_attr_destroy(&at);
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_bt_mx);
    }
}

// ---------------------------------------------------------------- sealing
// The key for one request: from the pair key, the challenge and what is asked.
static void bt_key(uint8_t out[32], const uint8_t pairkey[32], const uint8_t ch[16], int cmd, const char *busid) {
    crypto_blake2b_ctx c;
    uint8_t b = (uint8_t)cmd;
    crypto_blake2b_keyed_init(&c, 32, pairkey, 32);
    pr_h_str(&c, "USBeam bluetooth v1");
    crypto_blake2b_update(&c, ch, 16);
    crypto_blake2b_update(&c, &b, 1);
    pr_h_str(&c, busid);
    crypto_blake2b_final(&c, out);
}
// nonce (24) + mac (16) + sealed data
static int bt_seal(sb_t *out, const uint8_t key[32], const sb_t *plain) {
    uint8_t nonce[24], mac[16];
    plat_random(nonce, sizeof nonce);
    uint8_t *ct = malloc(plain->len ? plain->len : 1);
    if (!ct) return -1;
    crypto_aead_lock(ct, mac, key, nonce, NULL, 0, (const uint8_t *)plain->p, plain->len);
    sb_add(out, (const char *)nonce, 24); sb_add(out, (const char *)mac, 16); sb_add(out, (const char *)ct, plain->len);
    free(ct);
    return 0;
}
static uint8_t *bt_open(const uint8_t key[32], const uint8_t *m, size_t n, size_t *plen) {
    if (n < 40) return NULL;
    uint8_t *pt = malloc(n - 40 + 1);
    if (!pt) return NULL;
    if (crypto_aead_unlock(pt, m + 24, key, m, NULL, 0, m + 40, n - 40)) { free(pt); return NULL; }
    *plen = n - 40;
    return pt;
}
static int bt_send_blob(plat_sock s, const sb_t *b) {
    uint8_t l[4]; bt_put32(l, (uint32_t)b->len);
    return plat_send(s, l, 4) || (b->len && plat_send(s, b->p, (int)b->len));
}
static uint8_t *bt_recv_blob(plat_sock s, size_t *n) {
    uint8_t l[4];
    if (plat_recv(s, l, 4)) return NULL;
    uint32_t len = bt_get32(l);
    if (len < 40 || len > BT_MAXBLOB + 40) return NULL;
    uint8_t *b = malloc(len);
    if (!b || plat_recv(s, b, (int)len)) { free(b); return NULL; }
    *n = len;
    return b;
}

// ---------------------------------------------------------------- the PC the adapter is plugged into
static void bt_rebind(const char *busid) {         // BlueZ reads pairings when the adapter appears
    char p[PATH_MAX];
    snprintf(p, sizeof p, USBDEV "/%s/driver/unbind", busid);
    write_sysfs(p, busid);
    write_sysfs("/sys/bus/usb/drivers_probe", busid);
}

// GET: may that PC have this adapter's pairings? It must be about to use it (offered to it, or kept
// for it while restarting) or using it now. On success *h and addr are set.
static int bt_get_ok(const char *peer, const char *busid, HostDev **hp, char addr[18]) {
    if (!bt_have_bluez() || !dev_present(busid) || !shareable(busid) || !is_bt_adapter(busid)) return 0;
    HostDev *h = hdev(busid, 1);
    if (!h) return 0;
    int mine = (h->exported || host_held(h)) && !strcmp(h->peer, peer);
    if (!mine && !offerable(busid)) return 0;
    if (bt_adapter_addr(busid, addr) == 0) snprintf(h->btaddr, sizeof h->btaddr, "%s", addr);
    else if (h->btaddr[0]) snprintf(addr, 18, "%s", h->btaddr);    // in use elsewhere: as last seen here
    else return 0;
    *hp = h;
    return 1;
}

// A request from another PC (paired PCs only): GET = send the adapter's pairings; PUT = take them back.
// Request: its id (40), command (1), busid length (1), busid. Then a challenge it must answer, as for
// pair.h's requests. Status byte: 0 = ok, 1 = not paired, 2 = failed the check, 3 = not available.
static BtSess *bt_sess(const char *pc, const char *busid, int create);
static void bt_server(plat_sock s, const char *peer, const uint8_t *op) {
    uint32_t len = cl_g32(op + 4);
    uint8_t m[128];
    plat_sock_timeout(s, 5000);
    if (len < 42 || len > sizeof m || plat_recv(s, m, (int)len)) return;
    char id[40], busid[32];
    pr_unfix(id, m, 40);
    int cmd = m[40], bl = m[41];
    if (bl < 1 || bl > 31 || len < 42u + (uint32_t)bl) return;
    memcpy(busid, m + 42, (size_t)bl); busid[bl] = 0;
    if (!bt_valid_busid(busid)) return;
    uint8_t key[32]; int have = 0;
    pr_lock();
    PairRec *p = pr_find(id);
    if (p) { memcpy(key, p->key, 32); have = 1; }
    pr_unlock();
    uint8_t st;
    if (!have) { st = 1; pr_reply(s, OP_REP_BT, &st, 1); return; }
    uint8_t ch[17], mac[32], want[32];
    ch[0] = 0; plat_random(ch + 1, 16);
    pr_reply(s, OP_REP_BT, ch, sizeof ch);
    int got = !plat_recv(s, mac, 32);
    pr_mac(want, key, ch + 1, id, g_id, 100 + cmd, busid);
    uint8_t k[32]; bt_key(k, key, ch + 1, cmd, busid);
    crypto_wipe(key, sizeof key);
    if (!got) { crypto_wipe(k, sizeof k); return; }
    if (crypto_verify32(mac, want)) {
        st = 2; pr_reply(s, OP_REP_BT, &st, 1);
        logm("[%s] %s asked about its Bluetooth pairings, but failed the pairing check", busid, peer);
        crypto_wipe(k, sizeof k);
        return;
    }
    char addr[18] = "";
    if (cmd == BTC_GET) {
        HostDev *h = NULL;
        host_lock();
        int ok = bt_get_ok(peer, busid, &h, addr);
        if (ok) { snprintf(h->bt_peer, sizeof h->bt_peer, "%s", id); g_bt_hostdirty = 1; }   // by its USBeam id (its address may change)
        host_unlock();
        if (!ok) { st = 3; pr_reply(s, OP_REP_BT, &st, 1); crypto_wipe(k, sizeof k); return; }
        sb_t plain = {0}, sealed = {0};
        bt_snapshot_now(addr);                   // what is connected through it now, so it follows it
        bt_pack(addr, &plain);
        st = 0;
        if (!bt_seal(&sealed, k, &plain)) { pr_reply(s, OP_REP_BT, &st, 1); bt_send_blob(s, &sealed); }
        int nc = 0;
        pthread_mutex_lock(&g_bt_mx);
        BtConn *c = bt_conn(addr, 0);
        if (c) nc = c->nconn;
        pthread_mutex_unlock(&g_bt_mx);
        logm("[%s] its Bluetooth pairings went to %s with it (%d device(s) connected through it go along)", busid, peer, nc);
        crypto_wipe(plain.p, plain.len); sb_free(&plain); sb_free(&sealed);
        // Before it goes (that PC asks for it next). If it doesn't go after all, they are connected
        // here again in a moment; if it goes, this PC can't reach them any more by then.
        if (nc && bt_leave(addr)) {
            pthread_mutex_lock(&g_bt_mx);
            c = bt_conn(addr, 0);
            if (c) {
                c->nwant = 0;
                memcpy(c->want, c->conn, sizeof c->want); c->nwant = c->nconn;
                c->want_until = now_ms() + 90000; c->next_try = now_ms() + 10000;
            }
            pthread_mutex_unlock(&g_bt_mx);
        }
    } else if (cmd == BTC_PUT) {
        // Only from the PC that last had the adapter's pairings, and only for that adapter.
        host_lock();
        HostDev *h = hdev(busid, 0);
        int ok = h && h->btaddr[0] && h->bt_peer[0] && !strcmp(h->bt_peer, id);
        if (ok) snprintf(addr, sizeof addr, "%s", h->btaddr);
        host_unlock();
        if (!ok) {
            // Nothing is expected from it (already taken back, or never sent): tell it to stop trying.
            st = 4; pr_reply(s, OP_REP_BT, &st, 1); crypto_wipe(k, sizeof k);
            return;
        }
        st = 0; pr_reply(s, OP_REP_BT, &st, 1);
        size_t n = 0, pl = 0;
        uint8_t *b = bt_recv_blob(s, &n), *pt = b ? bt_open(k, b, n, &pl) : NULL;
        free(b);
        int changed = 0, r = pt ? bt_apply(addr, pt, pl, &changed) : -1;
        if (pt) { crypto_wipe(pt, pl); free(pt); }
        st = r ? 5 : 0;
        pr_reply(s, OP_REP_BT, &st, 1);
        if (!r) {
            host_lock();
            h = hdev(busid, 0);
            if (h && !strcmp(h->bt_peer, id)) {
                h->bt_peer[0] = 0; g_bt_hostdirty = 1;
                // Already back on this PC (it came back before its pairings): let BlueZ read them again.
                if (changed && !h->bt_wait_until && !h->exported && !host_held(h) && dev_present(busid) && !bound_to_stub(busid)) bt_rebind(busid);
            }
            host_unlock();
            logm("[%s] its Bluetooth pairings came back from %s%s", busid, peer, changed ? " (with changes)" : "");
        } else logm("[%s] %s sent Bluetooth pairings that couldn't be used", busid, peer);
    } else if (cmd == BTC_LEAVE) {
        // Its own PC is taking the adapter back: disconnect what is connected through it first.
        pthread_mutex_lock(&g_bt_mx);
        BtSess *b = bt_sess(id, busid, 0);
        if (b && b->state == BTS_HERE) snprintf(addr, sizeof addr, "%s", b->addr);
        pthread_mutex_unlock(&g_bt_mx);
        if (addr[0]) bt_leave(addr);
        st = addr[0] ? 0 : 4;
        pr_reply(s, OP_REP_BT, &st, 1);
    } else { st = 3; pr_reply(s, OP_REP_BT, &st, 1); }
    crypto_wipe(k, sizeof k);
}

// ---------------------------------------------------------------- the PC using the adapter
// Asks the PC the adapter is plugged into (cmd GET or PUT). For GET, *blob gets the opened set.
// Returns the status (0 = ok), or -1 for no answer.
static int bt_request(const char *pc, const char *ip, int cmd, const char *busid, const sb_t *put, uint8_t **blob, size_t *blen) {
    uint8_t key[32];
    pr_lock();
    PairRec *p = pr_find(pc);
    if (p) memcpy(key, p->key, 32);
    pr_unlock();
    if (!p) return 1;
    uint8_t req[42 + 32]; size_t bl = strlen(busid); if (bl > 31) bl = 31;
    pr_fixed(req, g_id, 40);
    req[40] = (uint8_t)cmd; req[41] = (uint8_t)bl; memcpy(req + 42, busid, bl);
    char b2[32]; memcpy(b2, busid, bl); b2[bl] = 0;
    int st = -1;
    plat_sock s = plat_connect(ip, 2500);
    if (s != PLAT_BAD_SOCK) {
        plat_sock_timeout(s, 6000);
        uint8_t rep[32]; uint32_t got = 0, h8;
        if (!pr_exchange(s, OP_REQ_BT, req, (uint32_t)(42 + bl), OP_REP_BT, rep, sizeof rep, &got)) {
            if (rep[0] != 0) st = rep[0];
            else if (got >= 17) {
                uint8_t mac[32], k[32], h[8];
                pr_mac(mac, key, rep + 1, g_id, pc, 100 + cmd, b2);
                bt_key(k, key, rep + 1, cmd, b2);
                if (!plat_send(s, mac, 32) && !plat_recv(s, h, 8) && cl_g16(h + 2) == OP_REP_BT && (h8 = cl_g32(h + 4)) == 1 && !plat_recv(s, rep, 1)) {
                    st = rep[0];
                    if (st == 0 && cmd == BTC_GET) {
                        size_t n = 0;
                        uint8_t *b = bt_recv_blob(s, &n);
                        *blob = b ? bt_open(k, b, n, blen) : NULL;
                        free(b);
                        if (!*blob) st = -1;
                    } else if (st == 0 && cmd == BTC_PUT) {
                        sb_t sealed = {0};
                        st = -1;
                        if (!bt_seal(&sealed, k, put) && !bt_send_blob(s, &sealed) &&
                            !plat_recv(s, h, 8) && cl_g16(h + 2) == OP_REP_BT && cl_g32(h + 4) == 1 && !plat_recv(s, rep, 1)) st = rep[0];
                        sb_free(&sealed);
                    }
                }
                crypto_wipe(k, sizeof k);
            }
        }
        plat_close(s);
    }
    crypto_wipe(key, sizeof key);
    return st;
}

static BtSess *bt_sess(const char *pc, const char *busid, int create) {   // call with g_bt_mx held
    for (int i = 0; i < BT_MAXSESS; i++) if (g_bts[i].state && !strcmp(g_bts[i].pc, pc) && !strcmp(g_bts[i].busid, busid)) return &g_bts[i];
    if (!create) return NULL;
    for (int i = 0; i < BT_MAXSESS; i++) if (!g_bts[i].state) { memset(&g_bts[i], 0, sizeof g_bts[i]); return &g_bts[i]; }
    return NULL;
}

// Sends the pairings back. 0 = done (or nothing to send any more).
static int bt_send_back(const char *pc, const char *ip, const char *busid, const char *addr) {
    sb_t set = {0};
    bt_pack(addr, &set);
    int st = bt_request(pc, ip, BTC_PUT, busid, &set, NULL, NULL);
    crypto_wipe(set.p, set.len); sb_free(&set);
    if (st == 0 || st == 4 || st == 1) {        // taken (4: wasn't expected any more; 1: not paired any more)
        bt_forget(addr);
        logm("[%s] Bluetooth pairings %s", busid, st == 0 ? "sent back to its PC" : "no longer needed here: removed");
        return 0;
    }
    return -1;
}

// Before connecting a Bluetooth adapter from another Linux PC (client.h). again: reconnecting it
// right after it restarted (its pairings are in place already).
static void plat_bt_before(const char *pc, const char *ip, const char *busid, int again) {
    if (!bt_have_bluez()) return;
    char addr[18] = ""; int state = BTS_NONE;
    pthread_mutex_lock(&g_bt_mx);
    BtSess *b = bt_sess(pc, busid, 0);
    if (b) { state = b->state; snprintf(addr, sizeof addr, "%s", b->addr); }
    pthread_mutex_unlock(&g_bt_mx);
    if (again && state == BTS_HERE) return;
    // Pairings from last time still to go back: they are the newest, so send them first. If that
    // isn't possible now, use them as they are (and send them back later).
    if (state == BTS_SEND && bt_send_back(pc, ip, busid, addr) != 0) {
        pthread_mutex_lock(&g_bt_mx);
        b = bt_sess(pc, busid, 0);
        if (b) b->state = BTS_HERE;
        pthread_mutex_unlock(&g_bt_mx);
        logm("[%s] its Bluetooth pairings from last time couldn't be sent back yet: using them here", busid);
        return;
    }
    uint8_t *blob = NULL; size_t n = 0;
    int st = bt_request(pc, ip, BTC_GET, busid, NULL, &blob, &n);
    if (st != 0 || !blob) {
        free(blob);
        pthread_mutex_lock(&g_bt_mx);
        b = bt_sess(pc, busid, 0);
        if (b) b->state = BTS_NONE;
        pthread_mutex_unlock(&g_bt_mx);
        if (st == 1) logm("[%s] Bluetooth pairings stay on its PC: pair the two PCs to bring them along", busid);
        else if (st != 3) logm("[%s] its Bluetooth pairings couldn't be brought along this time", busid);
        return;
    }
    char a[18] = "";
    if (n >= 26) { memcpy(a, blob + 9, 17); a[17] = 0; }
    int changed = 0;
    if (!bt_valid_addr(a) || bt_apply(a, blob, n, &changed) != 0) {
        logm("[%s] its Bluetooth pairings couldn't be put in place", busid);
    } else {
        pthread_mutex_lock(&g_bt_mx);
        b = bt_sess(pc, busid, 1);
        if (b) { b->state = BTS_HERE; snprintf(b->pc, sizeof b->pc, "%s", pc); snprintf(b->busid, sizeof b->busid, "%s", busid); snprintf(b->addr, sizeof b->addr, "%s", a); }
        pthread_mutex_unlock(&g_bt_mx);
        int nw = 0;
        pthread_mutex_lock(&g_bt_mx);
        BtConn *c = bt_conn(a, 0);
        if (c) nw = c->nwant;
        pthread_mutex_unlock(&g_bt_mx);
        logm("[%s] its Bluetooth pairings are in place here (%d device(s) to connect again)", busid, nw);
        g_cl_dirty = 1;
    }
    crypto_wipe(blob, n); free(blob);
}
// About to let go of the adapter (released here, or this PC goes to sleep or stops): disconnect
// what is connected through it first (see bt_leave).
static void plat_bt_leaving(const char *pc, const char *busid) {
    char addr[18] = "";
    pthread_mutex_lock(&g_bt_mx);
    BtSess *b = bt_sess(pc, busid, 0);
    if (b && b->state == BTS_HERE) snprintf(addr, sizeof addr, "%s", b->addr);
    pthread_mutex_unlock(&g_bt_mx);
    if (addr[0]) bt_leave(addr);
}
// Its own PC takes it back: it asks this PC to disconnect them first (BTC_LEAVE). A moment at most.
static void bt_ask_leave(const char *pc, const char *ip, const char *busid) {
    int st = bt_request(pc, ip, BTC_LEAVE, busid, NULL, NULL, NULL);
    if (st != 0) logm("[%s] the PC using it didn't disconnect its Bluetooth devices first (%d): they connect again here after their own timeout", busid, st);
}
// The adapter left this PC (released, taken back, connection lost...): its pairings go back (bt_tick).
static void plat_bt_after(const char *pc, const char *busid) {
    pthread_mutex_lock(&g_bt_mx);
    BtSess *b = bt_sess(pc, busid, 0);
    if (b && b->state == BTS_HERE) { b->state = BTS_SEND; b->next_try = 0; g_cl_dirty = 1; }
    pthread_mutex_unlock(&g_bt_mx);
}
// Sends back what is due, as soon as the PC it belongs to can be reached (client_tick, every second).
static void plat_bt_tick(void) {
    bt_conn_tick();
    unsigned long long now = now_ms();
    for (int i = 0; i < BT_MAXSESS; i++) {
        char pc[40], busid[32], addr[18], ip[64] = "";
        pthread_mutex_lock(&g_bt_mx);
        int due = g_bts[i].state == BTS_SEND && now >= g_bts[i].next_try;
        if (due) { snprintf(pc, sizeof pc, "%s", g_bts[i].pc); snprintf(busid, sizeof busid, "%s", g_bts[i].busid); snprintf(addr, sizeof addr, "%s", g_bts[i].addr); }
        pthread_mutex_unlock(&g_bt_mx);
        if (!due) continue;
        // Still (or again) using it here: nothing to send.
        int here = 0;
        cl_lock();
        Use *u = use_find(pc, busid);
        if (u && u->used && u->port >= 0) here = 1;
        Remote *r = rm_find(pc);
        if (r && r->via >= 0) snprintf(ip, sizeof ip, "%s", r->addr[r->via].ip);
        cl_unlock();
        int ok = here ? -1 : ip[0] ? bt_send_back(pc, ip, busid, addr) : -1;
        pthread_mutex_lock(&g_bt_mx);
        if (g_bts[i].state == BTS_SEND && !strcmp(g_bts[i].pc, pc) && !strcmp(g_bts[i].busid, busid)) {
            if (here) g_bts[i].state = BTS_HERE;
            else if (ok == 0) g_bts[i].state = BTS_NONE;
            else g_bts[i].next_try = now + 5000;
            g_cl_dirty = 1;
        }
        pthread_mutex_unlock(&g_bt_mx);
    }
}

// Kept in the settings file, so pairings still go back after a restart of this PC.
static void bt_save_lines(FILE *f) {
    // This PC's adapters whose pairings are out with another PC (so they are still taken back
    // after USBeam or this PC restarts). Called with host_lock held (save_settings).
    for (int i = 0; i < MAXH; i++)
        if (g_h[i].busid[0] && g_h[i].bt_peer[0] && bt_valid_addr(g_h[i].btaddr)) fprintf(f, "btout %s %s %s\n", g_h[i].busid, g_h[i].bt_peer, g_h[i].btaddr);
    pthread_mutex_lock(&g_bt_mx);
    for (int i = 0; i < BT_MAXSESS; i++) if (g_bts[i].state)
        fprintf(f, "btsess %d %s %s %s\n", g_bts[i].state == BTS_HERE ? BTS_SEND : g_bts[i].state, g_bts[i].pc, g_bts[i].busid, g_bts[i].addr);
    pthread_mutex_unlock(&g_bt_mx);
}
static int bt_load_line(const char *line) {
    int st; char pc[40], busid[32], addr[18];
    if (sscanf(line, "btout %31s %39s %17s", busid, pc, addr) == 3) {
        if (bt_valid_busid(busid) && bt_valid_addr(addr)) {
            HostDev *h = hdev(busid, 1);
            if (h) { snprintf(h->bt_peer, sizeof h->bt_peer, "%s", pc); snprintf(h->btaddr, sizeof h->btaddr, "%s", addr); }
        }
        return 1;
    }
    if (sscanf(line, "btsess %d %39s %31s %17s", &st, pc, busid, addr) != 4) return 0;
    if (st != BTS_SEND || !bt_valid_addr(addr)) return 1;
    pthread_mutex_lock(&g_bt_mx);
    BtSess *b = bt_sess(pc, busid, 1);
    if (b) { b->state = BTS_SEND; snprintf(b->pc, sizeof b->pc, "%s", pc); snprintf(b->busid, sizeof b->busid, "%s", busid); snprintf(b->addr, sizeof b->addr, "%s", addr); }
    pthread_mutex_unlock(&g_bt_mx);
    return 1;
}
