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
} HostDev;

static HostDev g_h[MAXH];

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

// Shareable = a real device (not a root hub or interface), not a hub, not a network adapter,
// and not a device that is itself borrowed from another PC.
static int shareable(const char *busid) {
    if (!strncmp(busid, "usb", 3) || strchr(busid, ':') || !strchr(busid, '-')) return 0;
    if (!dev_present(busid)) return 0;
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
static int stub_bind(const char *busid) {
    char p[PATH_MAX], v[64];
    snprintf(v, sizeof v, "add %s", busid);
    if (write_sysfs(STUB "/match_busid", v) < 0) { logm("[%s] match_busid add failed: %s", busid, strerror(errno)); return -1; }
    snprintf(p, sizeof p, USBDEV "/%s/driver/unbind", busid);
    write_sysfs(p, busid);                         // detach from its normal driver (if any)
    if (write_sysfs(STUB "/bind", busid) < 0) {
        logm("[%s] bind to usbip-host failed: %s", busid, strerror(errno));
        snprintf(v, sizeof v, "del %s", busid); write_sysfs(STUB "/match_busid", v);
        write_sysfs("/sys/bus/usb/drivers_probe", busid);
        return -1;
    }
    return 0;
}

static void return_to_host(const char *busid) {
    char v[64];
    if (bound_to_stub(busid)) write_sysfs(STUB "/unbind", busid);
    snprintf(v, sizeof v, "del %s", busid);
    write_sysfs(STUB "/match_busid", v);           // so a replug binds to the normal driver
    if (dev_present(busid)) write_sysfs("/sys/bus/usb/drivers_probe", busid);
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

static int offerable(const char *busid) {
    HostDev *h = hdev(busid, 0);
    if (!shareable(busid)) return 0;
    if (is_hidden(busid) || drive_mounted(busid)) return 0;
    if (is_radio(busid) && !is_shared(busid)) return 0;
    if (h && (h->exported || h->blocked)) return 0;
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

static void handle_import(int s, const char *peer) {
    char busid[33] = {0};
    if (recv_all(s, busid, 32)) return;
    uint8_t r[8 + 312] = {0};
    put16b(r, USBIP_VERSION); put16b(r + 2, 0x0003);
    char name[256];
    if (!offerable(busid)) { put32b(r + 4, 1); send_all(s, r, 8); logm("%s asked for %s: not available", peer, busid); return; }
    dev_name(busid, name, sizeof name);
    if (!bound_to_stub(busid) && stub_bind(busid) < 0) { put32b(r + 4, 1); send_all(s, r, 8); return; }
    char p[PATH_MAX], v[32];
    snprintf(p, sizeof p, USBDEV "/%s/usbip_sockfd", busid);
    for (int i = 0; i < 20 && access(p, W_OK) != 0; i++) usleep(50000);
    snprintf(v, sizeof v, "%d", s);
    fill_record(busid, r + 8);
    if (write_sysfs(p, v) < 0) {
        logm("[%s] kernel refused export: %s", busid, strerror(errno));
        return_to_host(busid);
        put32b(r + 4, 1); send_all(s, r, 8);
        return;
    }
    send_all(s, r, sizeof r);
    HostDev *h = hdev(busid, 1);
    h->exported = 1; h->returning = 0;
    snprintf(h->peer, sizeof h->peer, "%s", peer);
    logm("[%s] %s is now in use by %s", busid, name, peer);
}

// ---------------------------------------------------------------- periodic checks
static void host_tick(void) {
    time_t now = time(NULL);
    char name[256];
    for (int i = 0; i < MAXH; i++) {
        HostDev *h = &g_h[i];
        if (!h->busid[0]) continue;
        if (h->exported) {
            if (!dev_present(h->busid)) {
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
        if (h->returning && dev_present(h->busid)) {
            h->returning = 0;
            if (bound_to_stub(h->busid)) return_to_host(h->busid);
            h->hold_until = now + 10; h->last_shown = 0;
            logm("[%s] %s plugged back in and works on this PC. Clients may take it again in 10 s. Type C %s to cancel.",
                 h->busid, dev_name(h->busid, name, sizeof name), h->busid);
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
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                int idle = 10, intvl = 2, cnt = 5;
                setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
                setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
                setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
                setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
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
    for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0] && g_h[i].exported) return_to_host(g_h[i].busid);
    close(ls);
    return 0;
}
