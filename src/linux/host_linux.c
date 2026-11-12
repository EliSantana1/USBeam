// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// host_linux.c - Linux HOST side of USBeam (included by usbeam.c).
//
//   sudo usbeam host
//
// Offers this PC's USB devices to clients over USB/IP (TCP 3240), using the
// kernel's built-in usbip-host driver. Policy (same as the Windows host):
//   - Devices keep working here until a client connects; only then is the
//     device handed to usbip-host.
//   - When the client lets go, the device returns to this PC immediately.
//   - If a device is unplugged while in use, it comes back to THIS PC when
//     plugged in again, then stays blocked for a 10 s countdown before a
//     client may take it again. "C <busid>" cancels (keeps it here);
//     "S <busid>" allows it again.
//   - Never offered: hubs and USB network adapters.
//   - On start and on quit, every device is returned to this PC.

#include <poll.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <pthread.h>
#include "../common/devpolicy.h"
#include "sysdrive.h"

#define USBDEV "/sys/bus/usb/devices"
#define STUB "/sys/bus/usb/drivers/usbip-host"
#define MAXH 64

typedef struct {
    char busid[32];
    int exported;        // handed to a client
    char peer[64];
    int returning;       // unplugged during a session
    int blocked;         // refuse clients for now
    time_t hold_until;   // countdown end, 0 = none
    int last_shown;
    char taken_from[40]; // USBeam id of the PC this PC took it back from ("Take back"), until it's used again
    time_t exported_at;  // when it was handed to the client
    unsigned exp_devnum, exp_vid, exp_pid;   // its USB address and ids then (a new address: it left the bus meanwhile)
    time_t restart_until;  // restarted itself while the client set it up: waiting for it to come back (host_tick)
    time_t reserve_until;  // came back after restarting: kept for that client (h->peer) until then
    int claim;             // open file + 1 holding its port while it restarts (claim_port), 0 = none
    int watch_gen;         // which hand-over the restart watcher belongs to (restart_watch)
} HostDev;

// Some devices restart themselves while the PC they were given to sets them up (a Bluetooth chip
// whose firmware gets reloaded, for example): they drop off USB and come back a moment later.
// That is not the user unplugging it, so such a device stays with that PC.
#define RESTART_SETUP 10   // s after being handed over that a restart counts as part of setting it up
#define RESTART_BACK 5     // s it has to come back in
#define RESTART_KEEP 15    // s it is then kept for that PC to connect it again
static int host_held(const HostDev *h) { return h && (h->restart_until || h->reserve_until); }

static HostDev g_h[MAXH];
static int pair_import_ok(const char *ip, const char *busid);   // pair.h
static int g_hold_secs = 10;     // countdown before a replugged device may be taken again (gear menu)

static HostDev *hdev(const char *busid, int create) {
    for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0] && !strcmp(g_h[i].busid, busid)) return &g_h[i];
    if (!create) return NULL;
    for (int i = 0; i < MAXH; i++) if (!g_h[i].busid[0]) { memset(&g_h[i], 0, sizeof g_h[i]); snprintf(g_h[i].busid, sizeof g_h[i].busid, "%s", busid); return &g_h[i]; }
    return NULL;
}

static int read_attr(const char *busid, const char *attr, char *out, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, USBDEV "/%s/%s", busid, attr);
    FILE *f = fopen(p, "r");
    if (!f) { out[0] = 0; return -1; }
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}
static unsigned attr_hex(const char *busid, const char *a) { char b[64]; read_attr(busid, a, b, sizeof b); return (unsigned)strtoul(b, NULL, 16); }
static unsigned attr_dec(const char *busid, const char *a) { char b[64]; read_attr(busid, a, b, sizeof b); return (unsigned)strtoul(b, NULL, 10); }

static int dev_present(const char *busid) {
    char p[PATH_MAX]; struct stat st;
    snprintf(p, sizeof p, USBDEV "/%s/idVendor", busid);
    return stat(p, &st) == 0;
}

static int has_net_child(const char *busid) {
    char pat[PATH_MAX]; glob_t g;
    snprintf(pat, sizeof pat, USBDEV "/%s/%s:*/net", busid, busid);
    int r = glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0;
    globfree(&g);
    return r;
}

static int bound_to_stub(const char *busid) {
    char p[PATH_MAX], real[PATH_MAX];
    snprintf(p, sizeof p, USBDEV "/%s/driver", busid);
    if (!realpath(p, real)) return 0;
    return strstr(real, "/usbip-host") != NULL;
}

// True for devices that came over the network (used from another PC through vhci-hcd).
static int on_vhci(const char *busid) {
    char p[PATH_MAX], real[PATH_MAX];
    snprintf(p, sizeof p, USBDEV "/%s", busid);
    return realpath(p, real) && strstr(real, "vhci_hcd") != NULL;
}

// ---------------------------------------------------------------- system drives: never shown, never touched
// See sysdrv_check in sysdrive.h. "busid devnum" identifies the same physical connection.
static int sysdrive_check(const char *busid, int fresh) {
    if (!busid || !busid[0] || !strncmp(busid, "usb", 3) || strchr(busid, ':')) return 0;
    char key[48], dn[16] = "";
    read_attr(busid, "devnum", dn, sizeof dn);
    snprintf(key, sizeof key, "%s %s", busid, dn);
    return sysdrv_check(key, busid, fresh, sysdrv_usb_state);
}
static int is_system_drive(const char *busid) { return sysdrive_check(busid, 0); }        // for listing
static int is_system_drive_now(const char *busid) { return sysdrive_check(busid, 1); }    // before acting

// Shareable = a real device (not a root hub or interface), not a hub, not a network adapter,
// not a drive the running system lives on, and not a device that is itself borrowed from another PC.
static int shareable(const char *busid) {
    if (!strncmp(busid, "usb", 3) || strchr(busid, ':') || !strchr(busid, '-')) return 0;
    if (!dev_present(busid)) return 0;
    if (is_system_drive(busid)) return 0;
    if (on_vhci(busid)) return 0;
    if (attr_hex(busid, "bDeviceClass") == 0x09) return 0;
    if (has_net_child(busid)) return 0;
    return 1;
}

static uint32_t speed_code(const char *busid) {
    char b[32]; read_attr(busid, "speed", b, sizeof b);
    if (!strcmp(b, "1.5")) return 1;
    int v = atoi(b);
    if (v == 12) return 2;
    if (v == 480) return 3;
    if (v == 5000) return 5;
    if (v >= 10000) return 6;
    return 2;
}

static void put32b(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put16b(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

static void fill_record(const char *busid, uint8_t *r) {
    char p[PATH_MAX], real[PATH_MAX] = "";
    memset(r, 0, 312);
    snprintf(p, sizeof p, USBDEV "/%s", busid);
    if (!realpath(p, real)) snprintf(real, sizeof real, "%s", p);
    snprintf((char *)r, 256, "%s", real);
    snprintf((char *)r + 256, 32, "%s", busid);
    put32b(r + 288, attr_dec(busid, "busnum"));
    put32b(r + 292, attr_dec(busid, "devnum"));
    put32b(r + 296, speed_code(busid));
    put16b(r + 300, (uint16_t)attr_hex(busid, "idVendor"));
    put16b(r + 302, (uint16_t)attr_hex(busid, "idProduct"));
    put16b(r + 304, (uint16_t)attr_hex(busid, "bcdDevice"));
    r[306] = (uint8_t)attr_hex(busid, "bDeviceClass");
    r[307] = (uint8_t)attr_hex(busid, "bDeviceSubClass");
    r[308] = (uint8_t)attr_hex(busid, "bDeviceProtocol");
    r[309] = (uint8_t)attr_dec(busid, "bConfigurationValue");
    r[310] = (uint8_t)attr_dec(busid, "bNumConfigurations");
    r[311] = (uint8_t)attr_dec(busid, "bNumInterfaces");
}

static void iface_classes(const char *busid, uint8_t *out, int n) {
    unsigned cfg = attr_dec(busid, "bConfigurationValue");
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX]; FILE *f; char v[16];
        const char *names[3] = {"bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol"};
        for (int k = 0; k < 3; k++) {
            snprintf(p, sizeof p, USBDEV "/%s:%u.%d/%s", busid, cfg, i, names[k]);
            v[0] = 0;
            if ((f = fopen(p, "r"))) { if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f); }
            out[i * 4 + k] = (uint8_t)strtoul(v, NULL, 16);
        }
        out[i * 4 + 3] = 0;
    }
}

static const char *dev_name(const char *busid, char *buf, size_t n) {
    char m[128], p[128];
    read_attr(busid, "manufacturer", m, sizeof m);
    read_attr(busid, "product", p, sizeof p);
    snprintf(buf, n, "%s%s%s", m, (m[0] && p[0]) ? " " : "", p[0] ? p : "(unnamed)");
    return buf;
}

// ---------------------------------------------------------------- bind / return
// A USB reset right before a device goes to another PC. Without it, a device that was busy sending
// data at that moment (a mouse being moved, for example while clicking "Give To" with it) can keep
// one of its channels stuck: on the other PC its movement never arrives while its buttons work.
// The reset starts every channel clean, on the device and on this PC. Its normal driver is already
// detached here, so nothing on this PC is using it.
static void usb_reset_now(const char *busid) {
    char node[64];
    snprintf(node, sizeof node, "/dev/bus/usb/%03u/%03u", attr_dec(busid, "busnum"), attr_dec(busid, "devnum"));
    int fd = open(node, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, USBDEVFS_RESET, 0) != 0) logm("[%s] USB reset before handing it over failed: %s", busid, strerror(errno));
    if (fd >= 0) close(fd);
}
static int stub_bind(const char *busid) {
    char p[PATH_MAX], v[64];
    if (is_system_drive_now(busid)) { logm("[%s] refused: holds the running system", busid); return -1; }
    snprintf(v, sizeof v, "add %s", busid);
    if (write_sysfs(STUB "/match_busid", v) < 0) { logm("[%s] match_busid add failed: %s", busid, strerror(errno)); return -1; }
    snprintf(p, sizeof p, USBDEV "/%s/driver/unbind", busid);
    write_sysfs(p, busid);                         // detach from its normal driver (if any)
    usb_reset_now(busid);                          // hand it over clean (see usb_reset_now)
    if (write_sysfs(STUB "/bind", busid) < 0) {
        logm("[%s] bind to usbip-host failed: %s", busid, strerror(errno));
        snprintf(v, sizeof v, "del %s", busid); write_sysfs(STUB "/match_busid", v);
        write_sysfs("/sys/bus/usb/drivers_probe", busid);
        return -1;
    }
    return 0;
}

static void return_to_host(const char *busid) {
    char v[64], p[PATH_MAX], drv[PATH_MAX];
    if (is_system_drive_now(busid)) return;       // never re-probed: it is in use by this PC right now
    if (bound_to_stub(busid)) write_sysfs(STUB "/unbind", busid);
    snprintf(v, sizeof v, "del %s", busid);
    write_sysfs(STUB "/match_busid", v);           // so a replug binds to the normal driver
    // Left unconfigured while its port was claimed (claim_port): set it up from scratch.
    read_attr(busid, "bConfigurationValue", v, sizeof v);
    snprintf(p, sizeof p, USBDEV "/%s/driver", busid);
    ssize_t dl = readlink(p, drv, sizeof drv - 1);
    if (dl > 0 && !v[0]) { drv[dl] = 0; if (!strcmp(strrchr(drv, '/') ? strrchr(drv, '/') + 1 : drv, "usb")) { snprintf(p, sizeof p, USBDEV "/%s/driver/unbind", busid); write_sysfs(p, busid); } }
    if (dev_present(busid)) write_sysfs("/sys/bus/usb/drivers_probe", busid);
}

// ---------------------------------------------------------------- devices restarting themselves
// While a device restarts (host_tick), its port is claimed the way usbfs lets a program claim a hub
// port: a device appearing there is left unconfigured, so no driver on this PC starts using it (a
// Bluetooth chip would get its firmware loaded here, and the other PC would restart it once more).
// The claim belongs to an open file of this process, so it also ends if USBeam stops.
static int claim_port(const char *busid) {
    char hub[40], node[64];
    const char *dot = strrchr(busid, '.'), *dash = strchr(busid, '-');
    int port;
    if (!dash) return -1;
    if (dot) { snprintf(hub, sizeof hub, "%.*s", (int)(dot - busid), busid); port = atoi(dot + 1); }
    else { snprintf(hub, sizeof hub, "usb%.*s", (int)(dash - busid), busid); port = atoi(dash + 1); }   // on a root port
    if (port <= 0) return -1;
    snprintf(node, sizeof node, "/dev/bus/usb/%03u/%03u", attr_dec(hub, "busnum"), attr_dec(hub, "devnum"));
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned int pn = (unsigned int)port;
    if (ioctl(fd, USBDEVFS_CLAIM_PORT, &pn) != 0) { close(fd); return -1; }
    return fd;
}
static void release_claim(HostDev *h) {             // closing the file releases the port
    int c = __atomic_exchange_n(&h->claim, 0, __ATOMIC_SEQ_CST);
    if (c > 0) close(c - 1);
}

// The moment a device just handed over drops off the bus, claim its port, before it can come
// back (that takes a USB reset and debounce, well over 100 ms). Watches for RESTART_SETUP s.
typedef struct { HostDev *h; int gen; unsigned devnum; char busid[32]; } Watch;
static void *restart_watch(void *arg) {
    Watch w = *(Watch *)arg;
    free(arg);
    for (int i = 0; i < RESTART_SETUP * 50; i++) {
        usleep(20000);
        if (__atomic_load_n(&w.h->watch_gen, __ATOMIC_SEQ_CST) != w.gen) break;   // handed over again since
        if (dev_present(w.busid) && attr_dec(w.busid, "devnum") == w.devnum) continue;
        if (!__atomic_load_n(&w.h->exported, __ATOMIC_SEQ_CST) && !__atomic_load_n(&w.h->restart_until, __ATOMIC_SEQ_CST)) break;   // released normally
        int fd = claim_port(w.busid);
        if (fd < 0) continue;                     // (usbip-host may not have let go of the port yet)
        int old = __atomic_exchange_n(&w.h->claim, fd + 1, __ATOMIC_SEQ_CST);
        if (old > 0) close(old - 1);
        break;
    }
    return NULL;
}
static void restart_watch_start(HostDev *h) {
    Watch *w = malloc(sizeof *w);
    if (!w) return;
    w->h = h; w->gen = __atomic_add_fetch(&h->watch_gen, 1, __ATOMIC_SEQ_CST); w->devnum = h->exp_devnum;
    snprintf(w->busid, sizeof w->busid, "%s", h->busid);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, restart_watch, w)) free(w);
    pthread_attr_destroy(&at);
}

// Stop keeping a restarted device for the PC it restarted on: it works on this PC again. One that
// hasn't come back yet counts as unplugged (it returns to this PC when it's back).
static void host_end_hold(HostDev *h) {
    release_claim(h);
    if (h->restart_until && !dev_present(h->busid)) {
        char v[64]; snprintf(v, sizeof v, "del %s", h->busid);
        write_sysfs(STUB "/match_busid", v);
        h->returning = 1; h->blocked = 1;
    } else return_to_host(h->busid);
    h->restart_until = 0; h->reserve_until = 0;
}

// ---------------------------------------------------------------- scan for hardware changes
// One click finds USB devices Linux doesn't see: every USB port with no working device on it (nor
// on its USB2/USB3 twin) is switched off and on, as if replugged. A drive that "Safely remove"
// powered off comes back this way. Ports with a device on them are never touched, so nothing in
// use is disturbed, and system drives (always on a port with a device) can't be affected.
// Returns how many devices were found that weren't there before.
static int usb_device_list(char ids[][32], int max) {
    DIR *d = opendir(USBDEV);
    if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max)
        if (e->d_name[0] != '.' && strncmp(e->d_name, "usb", 3) && !strchr(e->d_name, ':')) snprintf(ids[n++], 32, "%s", e->d_name);
    closedir(d);
    return n;
}
static int scan_hardware(void) {
    static char before[256][32], after[256][32];
    int nb = usb_device_list(before, 256), reset = 0;
    glob_t g;
    if (glob(USBDEV "/*/*:1.0/*-port*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *port = g.gl_pathv[i];
            char p[PATH_MAX], peer[PATH_MAX], real[PATH_MAX], hub[64] = "", v[16] = "";
            struct stat st;
            if (!realpath(port, real) || strstr(real, "vhci_hcd")) continue;         // virtual ports for remote devices
            // The hub this port belongs to ("usb2", "1-1"...): a hub with a system drive behind
            // it is left alone entirely (root hubs only switch the one port, so they're fine).
            const char *hb = port + strlen(USBDEV "/");                           // "<hub>/<hub>:1.0/<port>"
            snprintf(hub, sizeof hub, "%.*s", (int)strcspn(hb, "/"), hb);
            if (strncmp(hub, "usb", 3) && is_system_drive_now(hub)) continue;
            snprintf(p, sizeof p, "%s/disable", port);
            FILE *df = fopen(p, "r");
            if (!df) continue;
            if (!fgets(v, sizeof v, df)) v[0] = 0;
            fclose(df);
            if (atoi(v) != 0 || access(p, W_OK) != 0) continue;                   // switched off on purpose: leave it
            snprintf(p, sizeof p, "%s/peer", port);
            int busy = 0;
            if (realpath(p, peer)) { snprintf(p, sizeof p, "%s/device", peer); if (lstat(p, &st) == 0) busy = 1; }
            snprintf(p, sizeof p, "%s/device", port);
            if (busy || lstat(p, &st) == 0) continue;                             // a device is on it (or its twin)
            snprintf(p, sizeof p, "%s/state", port);                              // being plugged in right now?
            df = fopen(p, "r"); v[0] = 0;
            if (df) { if (!fgets(v, sizeof v, df)) v[0] = 0; fclose(df); }
            if (strstr(v, "attached") && !strstr(v, "not")) continue;
            snprintf(p, sizeof p, "%s/device", port);                             // last look, right before switching
            if (lstat(p, &st) == 0) continue;
            snprintf(p, sizeof p, "%s/disable", port);
            if (write_sysfs(p, "1") == 0) { usleep(100000); write_sysfs(p, "0"); reset++; }
        }
        globfree(&g);
    }
    logm("Scan for hardware changes: %d empty USB ports checked", reset);
    int found = 0;
    for (int wait = 0; wait < (reset ? 8 : 0); wait++) {                    // devices take a moment to appear
        usleep(500000);
        int na = usb_device_list(after, 256);
        found = 0;
        for (int i = 0; i < na; i++) {
            int old = 0;
            for (int k = 0; k < nb && !old; k++) if (!strcmp(after[i], before[k])) old = 1;
            if (!old) found++;
        }
    }
    if (found) logm("Scan for hardware changes: found %d new USB device(s)", found);
    return found;
}

// ---------------------------------------------------------------- requests
static int list_shareable(char ids[][32], int max) {
    DIR *d = opendir(USBDEV);
    if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || !shareable(e->d_name)) continue;
        snprintf(ids[n++], 32, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

// ---------------------------------------------------------------- hidden devices & drives in use
#define MAXHIDE 64
static char g_hidden[MAXHIDE][32];
static int is_hidden(const char *busid) {
    for (int i = 0; i < MAXHIDE; i++) if (g_hidden[i][0] && !strcmp(g_hidden[i], busid)) return 1;
    return 0;
}
static void set_hidden(const char *busid, int on) {
    for (int i = 0; i < MAXHIDE; i++) if (g_hidden[i][0] && !strcmp(g_hidden[i], busid)) { if (!on) g_hidden[i][0] = 0; return; }
    if (on) for (int i = 0; i < MAXHIDE; i++) if (!g_hidden[i][0]) { snprintf(g_hidden[i], 32, "%s", busid); return; }
}

static char g_shared[MAXHIDE][32];      // opt-in devices the user chose to share
static int is_shared(const char *busid) {
    for (int i = 0; i < MAXHIDE; i++) if (g_shared[i][0] && !strcmp(g_shared[i], busid)) return 1;
    return 0;
}
static void set_shared(const char *busid, int on) {
    for (int i = 0; i < MAXHIDE; i++) if (g_shared[i][0] && !strcmp(g_shared[i], busid)) { if (!on) g_shared[i][0] = 0; return; }
    if (on) for (int i = 0; i < MAXHIDE; i++) if (!g_shared[i][0]) { snprintf(g_shared[i], 32, "%s", busid); return; }
}

// Bluetooth / Wi-Fi radios (class e0) are usually built into the PC: only shared if chosen.
static int is_radio(const char *busid) {
    if (attr_hex(busid, "bDeviceClass") == 0xe0) return 1;
    unsigned cfg = attr_dec(busid, "bConfigurationValue"), n = attr_dec(busid, "bNumInterfaces");
    for (unsigned i = 0; i < n && i < 32; i++) {
        char p[PATH_MAX], v[16] = ""; FILE *f;
        snprintf(p, sizeof p, USBDEV "/%s:%u.%u/bInterfaceClass", busid, cfg, i);
        if ((f = fopen(p, "r"))) { if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f); }
        if (strtoul(v, NULL, 16) == 0xe0) return 1;
    }
    return 0;
}

// True if a disk behind this USB device has a mounted partition (it is in use by this PC).
static int drive_mounted(const char *busid) {
    DIR *d = opendir("/sys/block");
    if (!d) return 0;
    struct dirent *e;
    char needle[48]; snprintf(needle, sizeof needle, "/%s/", busid);
    int mounted = 0;
    while (!mounted && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof link, "/sys/block/%s", e->d_name);
        if (!realpath(link, real) || !strstr(real, needle)) continue;
        FILE *m = fopen("/proc/mounts", "r");
        if (!m) continue;
        char line[1024], dev[64];
        snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
        while (fgets(line, sizeof line, m)) if (!strncmp(line, dev, strlen(dev))) { mounted = 1; break; }
        fclose(m);
    }
    closedir(d);
    return mounted;
}

// Interfaces as (class, subclass, protocol) triplets.
static int dev_ifaces(const char *busid, unsigned char ifc[][3], int max) {
    int n = (int)attr_dec(busid, "bNumInterfaces");
    if (n > max) n = max;
    if (n > 32) n = 32;
    uint8_t raw[4 * 32];
    iface_classes(busid, raw, n);
    for (int i = 0; i < n; i++) { ifc[i][0] = raw[i * 4]; ifc[i][1] = raw[i * 4 + 1]; ifc[i][2] = raw[i * 4 + 2]; }
    return n;
}
static int is_input_dev(const char *busid) {
    unsigned char ifc[32][3]; int n = dev_ifaces(busid, ifc, 32);
    return devp_is_input((const unsigned char (*)[3])ifc, n);
}
// Part of the PC itself rather than plugged into it: hidden unless the user chooses Share.
// Returns why, or NULL for an ordinary plug-in device.
static const char *system_reason(const char *busid) {
    if (is_radio(busid)) return "Bluetooth/wireless adapter";
    char name[256], m[128], p[128];
    read_attr(busid, "manufacturer", m, sizeof m);
    read_attr(busid, "product", p, sizeof p);
    snprintf(name, sizeof name, "%s %s", m, p);
    unsigned char ifc[32][3]; int n = dev_ifaces(busid, ifc, 32);
    if (devp_is_lighting(name, attr_hex(busid, "idVendor"), attr_hex(busid, "idProduct"), (const unsigned char (*)[3])ifc, n, 1))
        return "lighting controller";
    char rem[16]; read_attr(busid, "removable", rem, sizeof rem);
    if (!strcmp(rem, "fixed")) return "built into this PC";
    return NULL;
}

static int offerable(const char *busid) {
    HostDev *h = hdev(busid, 0);
    if (is_system_drive(busid)) return 0;
    if (!shareable(busid)) return 0;
    if (is_hidden(busid) || drive_mounted(busid)) return 0;
    if (system_reason(busid) && !is_shared(busid)) return 0;
    if (h && (h->exported || h->blocked || host_held(h))) return 0;
    return 1;
}

static void handle_devlist(int s, const char *peer) {
    char ids[MAXH][32];
    int n = list_shareable(ids, MAXH), m = 0;
    for (int i = 0; i < n; i++) if (offerable(ids[i])) m++;
    uint8_t h[12] = {0};
    put16b(h, USBIP_VERSION); put16b(h + 2, 0x0005); put32b(h + 8, m);
    send_all(s, h, 12);
    for (int i = 0; i < n; i++) {
        if (!offerable(ids[i])) continue;
        uint8_t r[312]; fill_record(ids[i], r);
        send_all(s, r, 312);
        int ni = r[311];
        uint8_t ic[4 * 32] = {0};
        if (ni > 32) ni = 32;
        iface_classes(ids[i], ic, ni);
        send_all(s, ic, 4 * ni);
    }
    logm("%s listed devices (%d available)", peer, m);
}

// ---------------------------------------------------------------- relay between the client and the kernel
// The kernel's usbip-host takes requests straight from a socket. Windows clients (usbip-win2)
// mark many reads "a short reply is an error" (URB_SHORT_NOT_OK). Windows itself ignores that
// mark, but Linux obeys it, so devices like some game controllers fail (-121, EREMOTEIO).
// USBeam sits in between: requests pass through unchanged except that this mark is removed;
// replies pass through untouched.
typedef struct { int client, kern; int refs; pthread_mutex_t mx; char busid[32]; } Relay;

static void relay_drop(Relay *r) {
    pthread_mutex_lock(&r->mx);
    int left = --r->refs;
    pthread_mutex_unlock(&r->mx);
    shutdown(r->client, SHUT_RDWR);            // wake the other direction
    shutdown(r->kern, SHUT_RDWR);
    if (left) return;
    close(r->client); close(r->kern);
    pthread_mutex_destroy(&r->mx);
    free(r);
}

static int relay_copy(int from, int to, uint64_t n, uint8_t *buf, size_t bufsz) {
    while (n) {
        size_t k = n < bufsz ? (size_t)n : bufsz;
        if (recv_all(from, buf, k) || send_all(to, buf, k)) return -1;
        n -= k;
    }
    return 0;
}

// Client -> kernel: forward whole requests, clearing URB_SHORT_NOT_OK.
static void *relay_up(void *arg) {
    Relay *r = arg;
    uint8_t h[48], *buf = malloc(65536);
    int fixed = 0;
    while (buf && !recv_all(r->client, h, 48)) {
        uint32_t cmd = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
        uint64_t extra = 0;
        if (cmd == 1) {                                         // CMD_SUBMIT
            uint32_t dir = (uint32_t)h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15];
            uint32_t len = (uint32_t)h[24] << 24 | h[25] << 16 | h[26] << 8 | h[27];
            uint32_t np = (uint32_t)h[32] << 24 | h[33] << 16 | h[34] << 8 | h[35];
            if (h[23] & 0x01) { h[23] &= (uint8_t)~0x01; if (!fixed++) logm("[%s] relaying with short replies allowed", r->busid); }
            if (dir == 0) extra += len;                         // OUT: data follows
            if (np != 0xFFFFFFFFu && np > 0) {
                if (np > 1024) break;                           // not a sane request
                extra += (uint64_t)np * 16;                     // isochronous packet descriptors
            }
            if (extra > (64u << 20)) break;
        } else if (cmd != 2) {                                  // only CMD_SUBMIT and CMD_UNLINK exist
            logm("[%s] unexpected data from the other PC: closing the connection", r->busid);
            break;
        }
        if (send_all(r->kern, h, 48) || relay_copy(r->client, r->kern, extra, buf, 65536)) break;
    }
    free(buf);
    relay_drop(r);
    return NULL;
}

// Kernel -> client: replies, passed through untouched.
static void *relay_down(void *arg) {
    Relay *r = arg;
    uint8_t *buf = malloc(65536);
    ssize_t n;
    while (buf && (n = recv(r->kern, buf, 65536, 0)) > 0) if (send_all(r->client, buf, (size_t)n)) break;
    free(buf);
    relay_drop(r);
    return NULL;
}

// A connected pair of sockets on this PC: *kern goes to the kernel, *mine to the relay.
static int loopback_pair(int *kern, int *mine) {
    struct sockaddr_in a = {0}; socklen_t al = sizeof a;
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0) return -1;
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1) || getsockname(l, (struct sockaddr *)&a, &al)) { close(l); return -1; }
    int k = socket(AF_INET, SOCK_STREAM, 0);
    if (k < 0 || connect(k, (struct sockaddr *)&a, sizeof a)) { if (k >= 0) close(k); close(l); return -1; }
    int m = accept(l, NULL, NULL);
    close(l);
    if (m < 0) { close(k); return -1; }
    int one = 1;
    setsockopt(k, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(m, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    *kern = k; *mine = m;
    return 0;
}

static int relay_start(int client, int mine, const char *busid) {
    Relay *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->client = client; r->kern = mine; r->refs = 2;
    snprintf(r->busid, sizeof r->busid, "%s", busid);
    pthread_mutex_init(&r->mx, NULL);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t1, t2;
    if (pthread_create(&t1, &at, relay_up, r)) { pthread_attr_destroy(&at); close(mine); pthread_mutex_destroy(&r->mx); free(r); return -1; }   // caller closes client
    if (pthread_create(&t2, &at, relay_down, r)) relay_drop(r);    // up thread holds the last reference
    pthread_attr_destroy(&at);
    return 0;
}

static void handle_import(int s, const char *peer) {
    char busid[33] = {0};
    if (recv_all(s, busid, 32)) return;
    uint8_t r[8 + 312] = {0};
    put16b(r, USBIP_VERSION); put16b(r + 2, 0x0003);
    char name[256];
    HostDev *held = hdev(busid, 0);
    if (!host_held(held)) held = NULL;
    // Kept after restarting itself (host_tick): only for the PC it restarted on, once it's back.
    int again = held && held->reserve_until && !strcmp(held->peer, peer) && shareable(busid) && bound_to_stub(busid) &&
                !is_hidden(busid) && (!system_reason(busid) || is_shared(busid));
    if (held && !again) { put32b(r + 4, 1); send_all(s, r, 8); logm("[%s] %s asked for it: kept for %s while it restarts", busid, peer, held->peer); return; }
    if (!again && !offerable(busid)) { put32b(r + 4, 1); send_all(s, r, 8); logm("%s asked for %s: not available", peer, busid); return; }
    if (!pair_import_ok(peer, busid)) {
        put32b(r + 4, 1); send_all(s, r, 8);
        logm("[%s] %s asked for it, but this PC lets only paired PCs use its devices", busid, peer);
        return;
    }
    dev_name(busid, name, sizeof name);
    if (!bound_to_stub(busid) && stub_bind(busid) < 0) { put32b(r + 4, 1); send_all(s, r, 8); return; }
    char p[PATH_MAX], v[32];
    snprintf(p, sizeof p, USBDEV "/%s/usbip_sockfd", busid);
    for (int i = 0; i < 20 && access(p, W_OK) != 0; i++) usleep(50000);
    int kern, mine;
    if (loopback_pair(&kern, &mine) < 0) {
        logm("[%s] could not set up the connection: %s", busid, strerror(errno));
        return_to_host(busid);
        put32b(r + 4, 1); send_all(s, r, 8);
        return;
    }
    snprintf(v, sizeof v, "%d", kern);
    fill_record(busid, r + 8);
    if (write_sysfs(p, v) < 0) {
        logm("[%s] kernel refused export: %s", busid, strerror(errno));
        close(kern); close(mine);
        return_to_host(busid);
        put32b(r + 4, 1); send_all(s, r, 8);
        return;
    }
    close(kern);                                 // the kernel holds its own reference
    send_all(s, r, sizeof r);
    struct timeval z = {0, 0};                   // from now on: no time limits on this connection
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof z);
    int c = dup(s);                              // the caller closes s; the relay keeps its own copy
    if (c < 0 || relay_start(c, mine, busid) < 0) {
        logm("[%s] could not start the relay", busid);
        if (c >= 0) close(c);
        return_to_host(busid);
        return;
    }
    HostDev *h = hdev(busid, 1);
    h->exported = 1; h->returning = 0; h->taken_from[0] = 0;
    h->restart_until = 0; h->reserve_until = 0;
    h->exported_at = time(NULL);
    h->exp_devnum = attr_dec(busid, "devnum");
    h->exp_vid = attr_hex(busid, "idVendor"); h->exp_pid = attr_hex(busid, "idProduct");
    snprintf(h->peer, sizeof h->peer, "%s", peer);
    release_claim(h);
    restart_watch_start(h);
    logm("[%s] %s is now in use by %s%s", busid, name, peer, again ? " again" : "");
}

// ---------------------------------------------------------------- periodic checks
static void host_tick(void) {
    time_t now = time(NULL);
    char name[256];
    for (int i = 0; i < MAXH; i++) {
        HostDev *h = &g_h[i];
        if (!h->busid[0]) continue;
        if (h->claim && !h->exported && !host_held(h)) {    // (a port claim left over: never keep one)
            release_claim(h);
            if (dev_present(h->busid) && !bound_to_stub(h->busid)) return_to_host(h->busid);
        }
        if (h->exported) {
            // Gone, or back already as a new connection (a new USB address): it left the bus.
            if (!dev_present(h->busid) || attr_dec(h->busid, "devnum") != h->exp_devnum) {
                if (now - h->exported_at <= RESTART_SETUP) {
                    // Most likely it restarted itself while that PC set it up. usbip-host keeps its
                    // claim on this port (match_busid), so when it's back it goes straight to
                    // usbip-host, not to this PC's own driver.
                    logm("[%s] restarted itself while %s was setting it up: keeping it for that PC", h->busid, h->peer);
                    h->exported = 0; h->restart_until = now + RESTART_BACK;
                    continue;
                }
                logm("[%s] device unplugged while %s was using it. It will come back to this PC when plugged in.", h->busid, h->peer);
                char v[64]; snprintf(v, sizeof v, "del %s", h->busid);
                write_sysfs(STUB "/match_busid", v);
                h->exported = 0; h->returning = 1; h->blocked = 1;
                continue;
            }
            char st[16] = "";
            read_attr(h->busid, "usbip_status", st, sizeof st);
            if (atoi(st) != 2) {     // 1 = available again: the client let go (or the network dropped)
                return_to_host(h->busid);
                h->exported = 0;
                logm("[%s] %s released by %s. It works on this PC again.", h->busid, dev_name(h->busid, name, sizeof name), h->peer);
            }
            continue;
        }
        if (h->restart_until) {
            if (dev_present(h->busid)) {
                // Back. Only the same kind of device is kept for that PC; anything else stays here.
                release_claim(h);                // usbip-host claims the port itself when it takes it
                if (attr_hex(h->busid, "idVendor") == h->exp_vid && attr_hex(h->busid, "idProduct") == h->exp_pid &&
                    shareable(h->busid) && (bound_to_stub(h->busid) || stub_bind(h->busid) == 0)) {
                    h->restart_until = 0; h->reserve_until = now + RESTART_KEEP;
                    logm("[%s] back after restarting: waiting for %s to connect it again", h->busid, h->peer);
                } else {
                    h->restart_until = 0;
                    return_to_host(h->busid);
                    logm("[%s] a different device is on this port now: it stays on this PC", h->busid);
                }
            } else if (now >= h->restart_until) {
                host_end_hold(h);             // didn't come back: it was unplugged after all
                logm("[%s] device unplugged while %s was using it. It will come back to this PC when plugged in.", h->busid, h->peer);
            }
            continue;
        }
        if (h->reserve_until) {
            if (!dev_present(h->busid)) {
                h->restart_until = 1;         // unplugged while waiting: same as above
                host_end_hold(h);
                logm("[%s] device unplugged before %s connected it again. It will come back to this PC when plugged in.", h->busid, h->peer);
            } else if (now >= h->reserve_until) {
                host_end_hold(h);
                logm("[%s] %s didn't connect it again: it works on this PC again.", h->busid, h->peer);
            }
            continue;
        }
        if (h->returning && dev_present(h->busid)) {
            h->returning = 0;
            if (bound_to_stub(h->busid)) return_to_host(h->busid);
            h->hold_until = now + g_hold_secs; h->last_shown = 0;
            logm("[%s] %s plugged back in and works on this PC. Other PCs may take it again in %d s.",
                 h->busid, dev_name(h->busid, name, sizeof name), g_hold_secs);
        }
        if (h->hold_until) {
            int left = (int)(h->hold_until - now);
            if (left <= 0) { h->hold_until = 0; h->blocked = 0; logm("[%s] countdown finished: available to clients again.", h->busid); }
            else if (left != h->last_shown && (left <= 5 || left % 5 == 0)) { h->last_shown = left; logm("[%s] available to clients in %d s (C %s to cancel)", h->busid, left, h->busid); }
        }
    }
}

static void host_print_list(void) {
    char ids[MAXH][32], name[256];
    int n = list_shareable(ids, MAXH);
    printf("\nUSB devices offered by this PC:\n");
    for (int i = 0; i < n; i++) {
        HostDev *h = hdev(ids[i], 0);
        const char *st = !h ? "available" : h->exported ? "IN USE" : h->hold_until ? "countdown" : h->blocked ? "kept on this PC" : "available";
        printf("  %-8s %04x:%04x  %-40.40s %s %s\n", ids[i], attr_hex(ids[i], "idVendor"), attr_hex(ids[i], "idProduct"),
               dev_name(ids[i], name, sizeof name), st, (h && h->exported) ? h->peer : "");
    }
    printf("Commands: L list   C <busid> cancel countdown   S <busid> allow again   Q quit\n> ");
    fflush(stdout);
}

static int cmd_host(void) {
    if (geteuid() != 0) { printf("Run with sudo: sharing USB devices needs admin rights.\n"); return 1; }
    struct stat st;
    if (stat(STUB, &st) != 0) {
        if (system("modprobe usbip-host") != 0 || stat(STUB, &st) != 0) { printf("Could not load the usbip-host kernel driver.\n"); return 1; }
        logm("Loaded usbip-host kernel driver");
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    // Safety net: return anything left bound to usbip-host.
    DIR *dd = opendir(USBDEV); struct dirent *e;
    while (dd && (e = readdir(dd))) if (e->d_name[0] != '.' && !strchr(e->d_name, ':') && bound_to_stub(e->d_name)) { logm("[%s] was left shared: returning it to this PC", e->d_name); return_to_host(e->d_name); }
    if (dd) closedir(dd);

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) { printf("Cannot use port %d: %s\n", USBIP_PORT, strerror(errno)); return 1; }
    printf("USBeam host for Linux - test version 1. Offering USB devices on port %d.\n", USBIP_PORT);
    host_print_list();

    while (!g_stop) {
        struct pollfd pf[2] = { { ls, POLLIN, 0 }, { 0, POLLIN, 0 } };
        int r = poll(pf, 2, 1000);
        if (r > 0 && (pf[0].revents & POLLIN)) {
            struct sockaddr_in ca; socklen_t cl = sizeof ca;
            int s = accept(ls, (struct sockaddr *)&ca, &cl);
            if (s >= 0) {
                char peer[64]; inet_ntop(AF_INET, &ca.sin_addr, peer, sizeof peer);
                tune_device_socket(s);
                struct timeval tv = { 5, 0 };
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                uint8_t op[8];
                if (!recv_all(s, op, 8)) {
                    uint16_t code = (uint16_t)(op[2] << 8 | op[3]);
                    if (code == OP_REQ_DEVLIST) handle_devlist(s, peer);
                    else if (code == OP_REQ_IMPORT) { struct timeval z = {0, 0}; handle_import(s, peer); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z); }
                }
                close(s);   // after an export the kernel holds its own reference
            }
        }
        if (r > 0 && (pf[1].revents & POLLIN)) {
            char line[128] = "", c = 0, arg[40] = "";
            if (!fgets(line, sizeof line, stdin)) { g_stop = 1; break; }
            sscanf(line, " %c %39s", &c, arg);
            c = (char)toupper((unsigned char)c);
            HostDev *h = arg[0] ? hdev(arg, 0) : NULL;
            if (c == 'Q') break;
            else if (c == 'C') { if (h && h->hold_until) { h->hold_until = 0; h->blocked = 1; logm("[%s] cancelled: stays on this PC. Type S %s to allow clients again.", arg, arg); } else printf("No countdown for '%s'.\n", arg); }
            else if (c == 'S') { if (h && h->blocked) { h->blocked = 0; h->hold_until = 0; logm("[%s] available to clients again.", arg); } else printf("'%s' is not being kept back.\n", arg); }
            host_print_list();
        }
        host_tick();
    }
    logm("Returning all devices to this PC...");
    for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0]) {
        if (g_h[i].exported) return_to_host(g_h[i].busid);
        else if (host_held(&g_h[i])) host_end_hold(&g_h[i]);
    }
    close(ls);
    return 0;
}
