// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// app_linux.c - the USBeam app for Linux (included by usbeam.c):
// host + client + automatic discovery + the app window (local web UI).
//
//   sudo usbeam          start USBeam and open its window

#include <pthread.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include "../common/version.h"
#include "../common/http.h"
#include "../common/client_types.h"
#include "ui_html.h"

#define DISCO_PORT 3241

static char g_name[64];
static char g_id[33];
static char g_boot_id[64];
static char g_settings[] = "/var/lib/usbeam/settings";
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
// Host-side state (shared devices, hidden/shared lists) is used by the network thread and the
// app window thread. Recursive: window actions save settings while holding it.
// Lock order: g_hmx before g_mx.
static pthread_mutex_t g_hmx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static void host_lock(void) { pthread_mutex_lock(&g_hmx); }
static void host_unlock(void) { pthread_mutex_unlock(&g_hmx); }

// ---------------------------------------------------------------- platform hooks for the shared client
typedef int plat_sock;
#define PLAT_BAD_SOCK (-1)
static plat_sock plat_connect(const char *ip, int ms) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    return s;
}
static int plat_send(plat_sock s, const void *b, int n) { return send_all(s, b, (size_t)n); }
static int plat_recv(plat_sock s, void *b, int n) { return recv_all(s, b, (size_t)n); }
static void plat_close(plat_sock s) { close(s); }
static void cl_lock(void) { pthread_mutex_lock(&g_mx); }
static void cl_unlock(void) { pthread_mutex_unlock(&g_mx); }
static unsigned long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t);      // keeps counting through sleep
    return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000;
}
// This PC's networks, and what kind each one is (by interface name).
static int plat_local_nets(LocalNet *out, int max) {
    struct ifaddrs *ifs, *i;
    int n = 0;
    if (getifaddrs(&ifs)) return 0;
    for (i = ifs; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        int kind = NET_HOME;
        if (!strncmp(i->ifa_name, "zt", 2)) kind = NET_ZT;
        else if (!strncmp(i->ifa_name, "tailscale", 9) || (a & 0xFFC00000u) == 0x64400000u) kind = NET_TS;
        else if (!strncmp(i->ifa_name, "docker", 6) || !strncmp(i->ifa_name, "br-", 3) || !strncmp(i->ifa_name, "virbr", 5) || !strncmp(i->ifa_name, "veth", 4)) continue;
        out[n].net = a & m; out[n].mask = m; out[n].kind = kind; n++;
    }
    freeifaddrs(ifs);
    return n;
}
static void plat_client_tick_begin(void) {}
// Each device this PC uses is noted in /run/usbeam/port-N ("ip busid"), so USBeam still knows
// it after its service restarts. /run is emptied at every power-on, like the virtual ports.
static void port_note(int port, const char *ip, const char *busid) {
    char p[64]; snprintf(p, sizeof p, "/run/usbeam/port-%d", port);
    if (!ip) { unlink(p); return; }
    mkdir("/run/usbeam", 0755);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s %s\n", ip, busid); fclose(f); }
}
static int plat_attach(const char *ip, const char *busid) {
    Want w; memset(&w, 0, sizeof w);
    snprintf(w.busid, sizeof w.busid, "%s", busid);
    w.port = -1;
    if (attach_one(ip, &w) != 0) return -1;
    port_note(w.port, ip, busid);
    return w.port;
}
static int plat_port_alive(int port, const char *ip, const char *busid) {
    (void)ip; (void)busid;
    int st = port_status(port);
    return st == ST_USED || st == ST_NOTASSIGNED;
}
static void plat_detach(int port) { if (plat_port_alive(port, NULL, NULL)) detach_port(port); port_note(port, NULL, NULL); }
static void plat_stop_retries(void) {}           // Linux's vhci-hcd never reconnects by itself
static int plat_attached(int *port, char (*ip)[64], char (*busid)[32], int max) {
    int n = 0;
    DIR *d = opendir("/run/usbeam");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        int pn;
        if (sscanf(e->d_name, "port-%d", &pn) != 1) continue;
        int st = port_status(pn);
        if (st != ST_USED && st != ST_NOTASSIGNED) { port_note(pn, NULL, NULL); continue; }   // no longer in use
        char p[300]; snprintf(p, sizeof p, "/run/usbeam/%s", e->d_name);
        FILE *f = fopen(p, "r");
        if (f && fscanf(f, "%63s %31s", ip[n], busid[n]) == 2) port[n++] = pn;
        if (f) fclose(f);
    }
    if (d) closedir(d);
    return n;
}
// At start: virtual ports in use that USBeam has no note of were connected by something else
// earlier (or a crash): disconnect them so nothing is left half-connected.
static void vhci_cleanup(void) {
    char path[64], line[256], hub[8], np[64];
    for (int c = 0; c < 16; c++) {
        if (c == 0) snprintf(path, sizeof path, VHCI "/status");
        else snprintf(path, sizeof path, VHCI "/status.%d", c);
        FILE *f = fopen(path, "r");
        if (!f) break;
        int p, sta;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%7s %d %d", hub, &p, &sta) != 3 || (sta != ST_USED && sta != ST_NOTASSIGNED)) continue;
            snprintf(np, sizeof np, "/run/usbeam/port-%d", p);
            if (access(np, F_OK) == 0) continue;
            logm("Virtual port %d was in use without USBeam knowing: disconnecting it", p);
            detach_port(p);
        }
        fclose(f);
    }
}
// USB/IP kernel drivers: built into most distributions, a separate package on some (e.g. Ubuntu).
static int g_have_stub = 1, g_have_vhci = 1;
static char g_driver_note[256];
static const char *plat_client_note(void) { return g_have_vhci ? NULL : g_driver_note; }
static void plat_tune(void) { tune_drives(); }

#include "../common/client.h"

// ---------------------------------------------------------------- which networks USBeam may be used on
// Linux has no shared idea of "home" vs "public" networks, so USBeam asks once per network
// (in its window) and remembers the answer. ZeroTier and Tailscale are always allowed.
static pthread_mutex_t g_nets_mx = PTHREAD_MUTEX_INITIALIZER;
static void nets_lock(void) { pthread_mutex_lock(&g_nets_mx); }
static void nets_unlock(void) { pthread_mutex_unlock(&g_nets_mx); }
#include "../common/netpolicy.h"

#define MAXTRUST 32
static pthread_mutex_t g_trust_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_trust_id[MAXTRUST][96], g_trust_name[MAXTRUST][96];
static int is_trusted(const char *id) {
    int r = 0;
    pthread_mutex_lock(&g_trust_mx);
    for (int i = 0; i < MAXTRUST; i++) if (g_trust_id[i][0] && !strcmp(g_trust_id[i], id)) r = 1;
    pthread_mutex_unlock(&g_trust_mx);
    return r;
}
static void set_trusted(const char *id, const char *name, int on) {
    pthread_mutex_lock(&g_trust_mx);
    int done = 0;
    for (int i = 0; i < MAXTRUST; i++) if (g_trust_id[i][0] && !strcmp(g_trust_id[i], id)) { if (!on) g_trust_id[i][0] = 0; done = 1; }
    if (on && !done) for (int i = 0; i < MAXTRUST; i++) if (!g_trust_id[i][0]) {
        snprintf(g_trust_id[i], sizeof g_trust_id[i], "%s", id);
        snprintf(g_trust_name[i], sizeof g_trust_name[i], "%s", name && *name ? name : id);
        for (char *c = g_trust_name[i]; *c; c++) if (*c == '\t' || *c == '\n') *c = ' ';
        break;
    }
    pthread_mutex_unlock(&g_trust_mx);
}

// The default gateway of an interface (network byte order), 0 = none.
static uint32_t if_gateway(const char *ifn) {
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], name[32]; unsigned dest, gw, flags;
    uint32_t r = 0;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "%31s %x %x %x", name, &dest, &gw, &flags) == 4 && !strcmp(name, ifn) && dest == 0 && gw) { r = gw; break; }
    if (f) fclose(f);
    return r;
}
// The gateway's hardware address (identifies "this" home network), "" if not known yet.
static void gateway_mac(uint32_t gw, const char *ifn, char *out, size_t n) {
    char ip[32], line[256], a[64], hw[64], dev[32]; unsigned t, fl;
    struct in_addr ia; ia.s_addr = gw;
    inet_ntop(AF_INET, &ia, ip, sizeof ip);
    out[0] = 0;
    FILE *f = fopen("/proc/net/arp", "r");
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "%63s %x %x %63s %*s %31s", a, &t, &fl, hw, dev) == 5 && !strcmp(a, ip) && !strcmp(dev, ifn) && (fl & 2) && strcmp(hw, "00:00:00:00:00:00")) { snprintf(out, n, "%s", hw); break; }
    if (f) fclose(f);
    if (!out[0]) {                       // ask for it: any packet to the gateway makes Linux look it up
        int u = socket(AF_INET, SOCK_DGRAM, 0);
        if (u >= 0) {
            struct sockaddr_in d = {0}; d.sin_family = AF_INET; d.sin_port = htons(9); d.sin_addr.s_addr = gw;
            sendto(u, "", 0, MSG_DONTWAIT, (struct sockaddr *)&d, sizeof d);
            close(u);
        }
    }
}
// A friendly name for the network on an interface: NetworkManager's connection name (often the Wi-Fi name).
static void net_display_name(const char *ifn, uint32_t net, char *out, size_t n) {
    static struct { char ifn[32]; uint32_t net; char name[96]; } cache[8];
    static int next;
    for (int i = 0; i < 8; i++) if (cache[i].ifn[0] && !strcmp(cache[i].ifn, ifn) && cache[i].net == net) { snprintf(out, n, "%s", cache[i].name); return; }
    char cmd[128], name[96] = "";
    int safe = 1;
    for (const char *c = ifn; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.') safe = 0;
    if (safe) {
        snprintf(cmd, sizeof cmd, "timeout 3 nmcli -t -g GENERAL.CONNECTION device show '%s' 2>/dev/null", ifn);
        FILE *p = popen(cmd, "r");
        if (p) { if (!fgets(name, sizeof name, p)) name[0] = 0; pclose(p); }
        name[strcspn(name, "\r\n")] = 0;
    }
    if (!name[0] || !strcmp(name, "--")) snprintf(name, sizeof name, "%s", ifn);
    int k = next++ % 8;
    snprintf(cache[k].ifn, sizeof cache[k].ifn, "%s", ifn); cache[k].net = net;
    snprintf(cache[k].name, sizeof cache[k].name, "%s", name);
    snprintf(out, n, "%s", name);
}

static int plat_networks(NetInfo *out, int max) {
    static struct { char ifn[32]; uint32_t net; int tries; } wait[8];
    struct ifaddrs *ifs, *i;
    int n = 0;
    if (getifaddrs(&ifs)) return 0;
    for (i = ifs; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        if (!strncmp(i->ifa_name, "docker", 6) || !strncmp(i->ifa_name, "br-", 3) || !strncmp(i->ifa_name, "virbr", 5) || !strncmp(i->ifa_name, "veth", 4)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        if ((a & 0xFFFF0000u) == 0xA9FE0000u) continue;                 // 169.254.x.x: no network
        NetInfo *x = &out[n++];
        memset(x, 0, sizeof *x);
        x->net = a & m; x->mask = m; x->kind = NET_HOME; x->ip = a;
        if (!strncmp(i->ifa_name, "zt", 2)) x->kind = NET_ZT;
        else if (!strncmp(i->ifa_name, "tailscale", 9) || (a & 0xFFC00000u) == 0x64400000u) x->kind = NET_TS;
        int bits = 0; for (uint32_t k = m; k; k <<= 1) bits++;
        char sub[40]; snprintf(sub, sizeof sub, "%u.%u.%u.%u/%d", x->net >> 24, (x->net >> 16) & 255, (x->net >> 8) & 255, x->net & 255, bits);
        if (x->kind != NET_HOME) {
            snprintf(x->id, sizeof x->id, "%s:%s", x->kind == NET_ZT ? "zerotier" : "tailscale", sub);
            snprintf(x->name, sizeof x->name, "%s", x->kind == NET_ZT ? "ZeroTier" : "Tailscale");
            x->allowed = 1;
            continue;
        }
        // Wi-Fi / Ethernet: known by its address range + the hardware address of its router.
        uint32_t gw = if_gateway(i->ifa_name);
        char mac[64] = "";
        if (gw) gateway_mac(gw, i->ifa_name, mac, sizeof mac);
        if (gw && !mac[0]) {                    // still looking it up: wait a few seconds before asking
            int w = -1, freeslot = -1;
            for (int k = 0; k < 8; k++) { if (!strcmp(wait[k].ifn, i->ifa_name) && wait[k].net == x->net) w = k; if (!wait[k].ifn[0] && freeslot < 0) freeslot = k; }
            if (w < 0) { w = freeslot >= 0 ? freeslot : 0; snprintf(wait[w].ifn, sizeof wait[w].ifn, "%s", i->ifa_name); wait[w].net = x->net; wait[w].tries = 0; }
            if (++wait[w].tries <= 3) { x->pending = 1; snprintf(x->id, sizeof x->id, "%s@?", sub); snprintf(x->name, sizeof x->name, "%s", i->ifa_name); continue; }
            struct in_addr ga; ga.s_addr = gw; inet_ntop(AF_INET, &ga, mac, sizeof mac);   // no answer: use the router's address
        }
        snprintf(x->id, sizeof x->id, "%s@%s", sub, gw ? mac : i->ifa_name);
        net_display_name(i->ifa_name, x->net, x->name, sizeof x->name);
        x->allowed = is_trusted(x->id);
    }
    freeifaddrs(ifs);
    return n;
}

// ---------------------------------------------------------------- settings
static void save_settings(void) {
    host_lock();
    mkdir("/var/lib/usbeam", 0755);
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.new", g_settings);
    FILE *f = fopen(tmp, "w");
    if (!f) { host_unlock(); return; }
    fprintf(f, "id %s\n", g_id);
    for (int i = 0; i < MAXHIDE; i++) if (g_hidden[i][0]) fprintf(f, "hide %s\n", g_hidden[i]);
    for (int i = 0; i < MAXHIDE; i++) if (g_shared[i][0]) fprintf(f, "share %s\n", g_shared[i]);
    pthread_mutex_lock(&g_trust_mx);
    for (int i = 0; i < MAXTRUST; i++) if (g_trust_id[i][0]) fprintf(f, "trustnet %s\t%s\n", g_trust_id[i], g_trust_name[i]);
    pthread_mutex_unlock(&g_trust_mx);
    cl_lock();
    client_save_lines(f);
    cl_unlock();
    fclose(f);
    rename(tmp, g_settings);
    host_unlock();
}
static void client_save(void) { save_settings(); }
static void load_settings(void) {
    FILE *f = fopen(g_settings, "r");
    if (!f) return;
    char line[256], a[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "id %32s", a) == 1) snprintf(g_id, sizeof g_id, "%s", a);
        else if (sscanf(line, "hide %63s", a) == 1) set_hidden(a, 1);
        else if (sscanf(line, "share %63s", a) == 1) set_shared(a, 1);
        else if (!strncmp(line, "trustnet ", 9)) {
            char tid[96] = "", tname[96] = "";
            if (sscanf(line + 9, "%95[^\t\n]\t%95[^\n]", tid, tname) >= 1) set_trusted(tid, tname, 1);
        }
        else client_load_line(line);
    }
    fclose(f);
}

// ---------------------------------------------------------------- host side: info for other PCs
static void host_state(const char *busid, const char **state, const char **peer, int *countdown) {
    HostDev *h = hdev(busid, 0);
    *peer = ""; *countdown = 0;
    if (h && h->exported) { *state = "in_use"; *peer = h->peer; }
    else if (h && h->hold_until) { *state = "countdown"; *countdown = (int)(h->hold_until - time(NULL)); if (*countdown < 0) *countdown = 0; }
    else if (h && h->blocked) *state = "kept";
    else *state = "available";
}

// Another PC introduces itself ("USBEAM2\t<id>\t<name>"), as if its announcement had arrived.
static void handle_hello(int s, const char *peer, const uint8_t *op) {
    uint32_t len = (uint32_t)op[4] << 24 | (uint32_t)op[5] << 16 | (uint32_t)op[6] << 8 | op[7];
    char buf[260];
    struct timeval tv = { 2, 0 };                 // short: a stalled sender must not hold up other PCs
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (len == 0 || len > 256 || recv_all(s, buf, len)) return;
    buf[len] = 0;
    disco_received(buf, peer);
}

static void handle_info(int s) {
    char ids[MAXH][32], name[256];
    int n = list_shareable(ids, MAXH);
    sb_t j = {0};
    sb_add(&j, "{\"name\":", 8); sb_jstr(&j, g_name);
    sb_add(&j, ",\"id\":", 6); sb_jstr(&j, g_id);
    sb_add(&j, ",\"hello\":true", 13);           // this version understands OP_REQ_HELLO
    sb_add(&j, ",\"devices\":[", 12);
    int first = 1;
    for (int i = 0; i < n; i++) {
        if (is_hidden(ids[i]) || drive_mounted(ids[i]) || (is_radio(ids[i]) && !is_shared(ids[i]))) continue;
        const char *st, *peer; int cd;
        host_state(ids[i], &st, &peer, &cd);
        if (!first) sb_add(&j, ",", 1);
        first = 0;
        sb_add(&j, "{\"busid\":", 9); sb_jstr(&j, ids[i]);
        sb_add(&j, ",\"name\":", 8); sb_jstr(&j, dev_name(ids[i], name, sizeof name));
        sb_printf(&j, ",\"vid\":\"%04x\",\"pid\":\"%04x\",\"state\":\"%s\",\"countdown\":%d,\"peer\":", attr_hex(ids[i], "idVendor"), attr_hex(ids[i], "idProduct"), st, cd);
        sb_jstr(&j, peer);
        sb_add(&j, "}", 1);
    }
    sb_add(&j, "]}", 2);
    uint8_t h[12] = {0};
    put16b(h, USBIP_VERSION); put16b(h + 2, OP_REP_INFO); put32b(h + 8, (uint32_t)j.len);
    send_all(s, h, 12);
    send_all(s, j.p, j.len);
    sb_free(&j);
}

static void *client_main(void *arg) {
    (void)arg;
    while (!g_stop) {
        client_tick();
        for (int k = 0; k < 10 && !g_stop; k++) usleep(100000);
    }
    return NULL;
}

// ---------------------------------------------------------------- discovery
static int is_local_ip(const char *ip) {
    struct ifaddrs *ifs, *i;
    int r = 0;
    if (getifaddrs(&ifs)) return 0;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        char b[64]; inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, b, sizeof b);
        if (!strcmp(b, ip)) { r = 1; break; }
    }
    freeifaddrs(ifs);
    return r;
}

static void disco_announce(int us) {
    char msg[128]; disco_message(msg, sizeof msg);
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs)) return;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING)) continue;
        if (!net_ip_allowed(ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr))) continue;   // not a network USBeam is used on
        struct sockaddr_in b = *(struct sockaddr_in *)i->ifa_broadaddr;
        b.sin_port = htons(DISCO_PORT);
        // Never wait: if a network can't take the message right now, skip it (next try in 3 s).
        sendto(us, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&b, sizeof b);
    }
    freeifaddrs(ifs);
}

static void disco_receive(int us) {
    char buf[160]; struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t n = recvfrom(us, buf, sizeof buf - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n <= 8) return;
    buf[n] = 0;
    char ip[64]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    if (is_local_ip(ip) || !net_str_allowed(ip)) return;
    if (disco_received(buf, ip) && disco_reply_due(ip)) {     // answer directly: one announcement getting through is enough
        char msg[128]; int len = disco_message(msg, sizeof msg);
        struct sockaddr_in to = from; to.sin_port = htons(DISCO_PORT);
        if (len > 0) sendto(us, msg, (size_t)len, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof to);
    }
}

// Firewall (ufw, when it is switched on): open USBeam's ports only for the networks USBeam
// may be used on, and remove rules USBeam added earlier for other networks.
// The networks USBeam may be used on, as firewall address ranges ("192.168.1.0/24", "100.64.0.0/10").
static int allowed_ranges(char want[][40], int max) {
    int nw = 0;
    nets_lock();
    for (int i = 0; i < g_nnets && nw < max; i++) {
        NetInfo *x = &g_nets[i];
        if (!x->allowed) continue;
        int bits = 0; for (uint32_t m = x->mask; m; m <<= 1) bits++;
        char r[40];
        if (x->kind == NET_TS) snprintf(r, sizeof r, "100.64.0.0/10");
        else snprintf(r, sizeof r, "%u.%u.%u.%u/%d", x->net >> 24, (x->net >> 16) & 255, (x->net >> 8) & 255, x->net & 255, bits);
        int dup = 0; for (int k = 0; k < nw; k++) if (!strcmp(want[k], r)) dup = 1;
        if (!dup) snprintf(want[nw++], 40, "%s", r);
    }
    nets_unlock();
    return nw;
}

// ufw (Ubuntu, Mint, Arch...): rules carry the comment "USBeam".
static void ufw_sync(char want[][40], int nw) {
    if (system("command -v ufw >/dev/null 2>&1 && timeout 20 ufw status 2>/dev/null | grep -q 'Status: active'") != 0) return;
    char cmd[256];
    FILE *p = popen("timeout 20 ufw status 2>/dev/null", "r");
    char line[512], have[64][40]; int nh = 0;
    while (p && fgets(line, sizeof line, p) && nh < 64) {
        if (!strstr(line, "# USBeam") || strncmp(line, "3240/tcp", 8)) continue;
        char t1[64], t2[64], t3[64], t4[64];
        int k = sscanf(line, "%63s %63s %63s %63s", t1, t2, t3, t4);
        const char *from = (k >= 4 && !strcmp(t3, "IN")) ? t4 : t3;
        if (k >= 3 && strchr(from, '.') && strlen(from) < 40) snprintf(have[nh++], 40, "%s", from);
    }
    if (p) pclose(p);
    for (int h = 0; h < nh; h++) {             // remove rules for networks that are no longer allowed
        int keep = 0;
        for (int k = 0; k < nw; k++) if (!strcmp(have[h], want[k])) keep = 1;
        if (keep) continue;
        snprintf(cmd, sizeof cmd, "timeout 20 ufw delete allow from %s to any port 3240 proto tcp >/dev/null 2>&1", have[h]);
        if (system(cmd) != 0) logm("Could not remove firewall rule for %s", have[h]);
        snprintf(cmd, sizeof cmd, "timeout 20 ufw delete allow from %s to any port 3241 proto udp >/dev/null 2>&1", have[h]);
        if (system(cmd) != 0) logm("Could not remove firewall rule for %s", have[h]);
    }
    for (int k = 0; k < nw; k++) {
        snprintf(cmd, sizeof cmd, "timeout 20 ufw allow from %s to any port 3240 proto tcp comment USBeam >/dev/null 2>&1", want[k]);
        if (system(cmd) != 0) logm("Could not add firewall rule for %s", want[k]);
        snprintf(cmd, sizeof cmd, "timeout 20 ufw allow from %s to any port 3241 proto udp comment USBeam >/dev/null 2>&1", want[k]);
        if (system(cmd) != 0) logm("Could not add firewall rule for %s", want[k]);
    }
    logm("Firewall (ufw): USBeam allowed on %d network(s)", nw);
}

// firewalld (Fedora, openSUSE, RHEL...): the ranges USBeam opened are noted in /var/lib/usbeam/firewalld.
#define FWD_LIST "/var/lib/usbeam/firewalld"
static void firewalld_rule(const char *range, int add) {
    static const char *ports[2][2] = { { "3240", "tcp" }, { "3241", "udp" } };
    for (int k = 0; k < 2; k++) for (int perm = 0; perm < 2; perm++) {
        char cmd[384];
        snprintf(cmd, sizeof cmd, "timeout 20 firewall-cmd -q %s--%s-rich-rule='rule family=\"ipv4\" source address=\"%s\" port port=\"%s\" protocol=\"%s\" accept' >/dev/null 2>&1",
                 perm ? "--permanent " : "", add ? "add" : "remove", range, ports[k][0], ports[k][1]);
        if (system(cmd) != 0 && add) logm("Could not %s firewall rule for %s", add ? "add" : "remove", range);
    }
}
static void firewalld_sync(char want[][40], int nw) {
    if (system("command -v firewall-cmd >/dev/null 2>&1 && timeout 20 firewall-cmd --state >/dev/null 2>&1") != 0) return;
    char have[64][40]; int nh = 0;
    FILE *f = fopen(FWD_LIST, "r");
    while (f && nh < 64 && fscanf(f, "%39s", have[nh]) == 1) nh++;
    if (f) fclose(f);
    for (int h = 0; h < nh; h++) {
        int keep = 0;
        for (int k = 0; k < nw; k++) if (!strcmp(have[h], want[k])) keep = 1;
        if (!keep) firewalld_rule(have[h], 0);
    }
    for (int k = 0; k < nw; k++) {
        int had = 0;
        for (int h = 0; h < nh; h++) if (!strcmp(have[h], want[k])) had = 1;
        if (!had) firewalld_rule(want[k], 1);
    }
    mkdir("/var/lib/usbeam", 0755);
    if (nw) { f = fopen(FWD_LIST, "w"); for (int k = 0; f && k < nw; k++) fprintf(f, "%s\n", want[k]); if (f) fclose(f); }
    else unlink(FWD_LIST);
    logm("Firewall (firewalld): USBeam allowed on %d network(s)", nw);
}

// Open USBeam's ports only for the networks it may be used on; close them for the others.
static void firewall_sync(void) {
    char want[MAXNETS][40];
    int nw = allowed_ranges(want, MAXNETS);
    ufw_sync(want, nw);
    firewalld_sync(want, nw);
}

// "usbeam --cleanup": undo what the service set up on this PC (used when uninstalling).
static int cmd_cleanup(void) {
    if (geteuid() != 0) { printf("Needs admin rights.\n"); return 1; }
    char none[1][40];
    ufw_sync(none, 0);
    firewalld_sync(none, 0);
    system("rm -rf /var/lib/usbeam /run/usbeam");
    printf("USBeam's firewall rules and data were removed.\n");
    return 0;
}

// ---------------------------------------------------------------- app window API
static void add_local_json(sb_t *j) {
    char ids[MAXH][32], name[256];
    int n = list_shareable(ids, MAXH);
    sb_add(j, "\"local\":[", 9);
    for (int i = 0; i < n; i++) {
        const char *st, *peer, *note = ""; int cd;
        host_state(ids[i], &st, &peer, &cd);
        int fixed = 0;
        if (strcmp(st, "in_use") && drive_mounted(ids[i])) { st = "hidden"; note = "drive in use by this PC, not shared"; fixed = 1; }
        else if (strcmp(st, "in_use") && is_hidden(ids[i])) st = "hidden";
        else if (strcmp(st, "in_use") && is_radio(ids[i]) && !is_shared(ids[i])) { st = "hidden"; note = "Bluetooth/wireless adapter: shared only if you choose Share"; }
        if (i) sb_add(j, ",", 1);
        sb_add(j, "{\"id\":", 6); sb_jstr(j, ids[i]);
        sb_add(j, ",\"name\":", 8); sb_jstr(j, dev_name(ids[i], name, sizeof name));
        sb_printf(j, ",\"vid\":\"%04x\",\"pid\":\"%04x\",\"state\":\"%s\",\"countdown\":%d,\"fixed\":%s,\"peer\":", attr_hex(ids[i], "idVendor"), attr_hex(ids[i], "idProduct"), st, cd, fixed ? "true" : "false");
        char pl[160] = ""; if (peer[0]) peer_label(peer, pl, sizeof pl, 0);
        sb_jstr(j, pl);
        sb_add(j, ",\"note\":", 8); sb_jstr(j, note);
        sb_add(j, "}", 1);
    }
    sb_add(j, "]", 1);
}

static volatile sig_atomic_t g_restart;     // "Restart USBeam" from the gear menu
static int ui_api(const char *method, const char *path, const char *body, sb_t *out);

// "Save diagnostics": everything useful for finding a problem, as one text (the app saves it as a file).
static void diagnostics(sb_t *out) {
    sb_t t = {0};
    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    struct utsname un; uname(&un);
    char osname[128] = "Linux";
    FILE *f = fopen("/etc/os-release", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "PRETTY_NAME=", 12)) { snprintf(osname, sizeof osname, "%s", line + 12 + (line[12] == '"')); osname[strcspn(osname, "\"\r\n")] = 0; }
    if (f) fclose(f);
    sb_printf(&t, "USBeam %s diagnostics, %04d-%02d-%02d %02d:%02d\n%s, kernel %s (%s)\nThis PC: %s (id %s)\n"
                  "Drivers: usbip-host %s, vhci-hcd %s\nInstalled as: %s\n\nState:\n",
              USBEAM_VERSION, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, osname, un.release, un.machine,
              g_name, g_id, g_have_stub ? "ok" : "missing", g_have_vhci ? "ok" : "missing",
              access("/opt/usbeam/installed", F_OK) == 0 ? "AppImage" : "package");
    ui_api("GET", "/api/state", "", &t);
    sb_printf(&t, "%s", "\n\nLog (most recent last):\n");
    FILE *p = popen("timeout 10 journalctl -u usbeam --no-pager -n 400 -o short-iso 2>/dev/null", "r");
    char buf[4096]; size_t k;
    while (p && (k = fread(buf, 1, sizeof buf, p)) > 0) sb_add(&t, buf, k);
    if (p) pclose(p);
    sb_add(out, "{\"ok\":true,\"text\":", 18);
    sb_jstr(out, t.p ? t.p : "");
    sb_add(out, "}", 1);
    sb_free(&t);
}

static int ui_api(const char *method, const char *path, const char *body, sb_t *out) {
    if (!strcmp(method, "GET") && !strcmp(path, "/api/state")) {
        sb_printf(out, "%s", "{\"os\":\"linux\",\"version\":\"" USBEAM_VERSION "\",\"name\":"); sb_jstr(out, g_name);
        sb_printf(out, "%s", ","); add_local_json(out);
        sb_printf(out, "%s", ","); client_json(out);
        sb_printf(out, "%s", ","); nets_json(out);
        sb_add(out, ",\"trusted\":[", 12);
        pthread_mutex_lock(&g_trust_mx);
        int tf = 1;
        for (int i = 0; i < MAXTRUST; i++) if (g_trust_id[i][0]) {
            if (!tf) sb_add(out, ",", 1);
            tf = 0;
            sb_add(out, "{\"id\":", 6); sb_jstr(out, g_trust_id[i]);
            sb_add(out, ",\"name\":", 8); sb_jstr(out, g_trust_name[i]);
            sb_add(out, "}", 1);
        }
        pthread_mutex_unlock(&g_trust_mx);
        sb_printf(out, "%s", "],"); prefs_json(out);
        struct utsname un; uname(&un);
        sb_add(out, ",\"arch\":", 8); sb_jstr(out, un.machine);
        sb_printf(out, ",\"install\":\"%s\"", access("/opt/usbeam/installed", F_OK) == 0 ? "appimage" : "package");
        sb_add(out, ",\"host_note\":", 13); sb_jstr(out, g_have_stub ? "" : g_driver_note);
        sb_printf(out, "%s", "}");
        return 200;
    }
    if (strcmp(method, "POST") || strcmp(path, "/api/action")) return 404;
    char op[32], id[64];
    json_str(body, "op", op, sizeof op);
    json_str(body, "id", id, sizeof id);
    HostDev *h = id[0] ? hdev(id, 0) : NULL;
    int code = 200;

    char nid[96] = "";
    json_str(body, "net", nid, sizeof nid);
    if (!strcmp(op, "forget_auto")) { client_forget_auto(); }
    else if (!strcmp(op, "net_allow") || !strcmp(op, "net_forget")) {
        int on = !strcmp(op, "net_allow");
        char nname[96] = "";
        nets_lock();
        for (int i = 0; i < g_nnets; i++) if (!strcmp(g_nets[i].id, nid)) snprintf(nname, sizeof nname, "%s", g_nets[i].name);
        nets_unlock();
        if (on && (!nid[0] || strchr(nid, '?'))) { sb_printf(out, "%s", "{\"error\":\"This network is still being identified. Try again in a few seconds.\"}"); return 400; }
        set_trusted(nid, nname, on);
        save_settings();
        nets_refresh();                         // takes effect now; the firewall follows within 3 s
        logm("Network %s: USBeam %s", nname[0] ? nname : nid, on ? "allowed" : "no longer allowed");
    }
    else if (!strcmp(op, "net_later")) { if (nid[0]) nets_later(nid); }
    else if (!strcmp(op, "quit")) { logm("Quit requested from the USBeam app"); g_stop = 1; }
    else if (!strcmp(op, "restart")) { logm("Restart requested from the USBeam app"); g_restart = 1; g_stop = 1; }
    else if (!strcmp(op, "diagnostics")) { diagnostics(out); return 200; }
    else if (!strcmp(op, "reset")) {
        for (int i = 0; i < MAXHIDE; i++) { g_hidden[i][0] = 0; g_shared[i][0] = 0; }
        for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0] && !g_h[i].exported) { g_h[i].blocked = 0; g_h[i].hold_until = 0; }
        pthread_mutex_lock(&g_trust_mx); memset(g_trust_id, 0, sizeof g_trust_id); pthread_mutex_unlock(&g_trust_mx);
        nets_lock(); memset(g_nets_later, 0, sizeof g_nets_later); nets_unlock();
        int c2 = 200; client_action("reset_client", body, out, &c2);
        nets_refresh();
        logm("USBeam was reset to its starting settings");
    }
    else if (!strcmp(op, "local_cancel")) { if (h && h->hold_until) { h->hold_until = 0; h->blocked = 1; logm("[%s] reconnect cancelled", id); } }
    else if (!strcmp(op, "local_allow")) { if (h) { h->blocked = 0; h->hold_until = 0; } }
    else if (!strcmp(op, "local_hide")) { set_hidden(id, 1); set_shared(id, 0); save_settings(); }
    else if (!strcmp(op, "local_share")) {
        if (drive_mounted(id)) { sb_printf(out, "%s", "{\"error\":\"This drive is in use by this PC. Unmount (eject) it first.\"}"); return 400; }
        set_hidden(id, 0); if (is_radio(id)) set_shared(id, 1);
        if (h) { h->blocked = 0; h->hold_until = 0; }
        save_settings();
    }
    else if (!strcmp(op, "local_disconnect")) {
        if (h && h->exported) { return_to_host(id); h->exported = 0; h->blocked = 1; logm("[%s] taken back from %s", id, h->peer); }
    }
    else if (client_action(op, body, out, &code)) { if (code != 200) return code; }
    else { sb_printf(out, "%s", "{\"error\":\"unknown action\"}"); return 400; }
    sb_printf(out, "%s", "{\"ok\":true}");
    return 200;
}

// Open the app window as the desktop user (not root).
static void open_window(void) {
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/?t=%s", UI_PORT, g_ui_token);
    printf("\nUSBeam window: %s\n\n", url);
    fflush(stdout);
    const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID"), *user = getenv("SUDO_USER");
    if (!su || !sg || !user) return;
    unsigned uidn = (unsigned)atoi(su), gidn = (unsigned)atoi(sg);
    if (!uidn) return;
    if (fork() == 0) {
        if (fork() != 0) _exit(0);     // double fork: don't leave zombies
        char rt[64], bus[128], uid[16];
        snprintf(rt, sizeof rt, "XDG_RUNTIME_DIR=/run/user/%u", uidn);
        snprintf(bus, sizeof bus, "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus", uidn);
        snprintf(uid, sizeof uid, "%u", uidn);
        // sudo drops the desktop session's variables: find them in the user's runtime dir.
        char rdir[64]; snprintf(rdir, sizeof rdir, "/run/user/%u", uidn);
        char wname[64] = "", xauth[200] = "";
        DIR *rd = opendir(rdir); struct dirent *re;
        while (rd && (re = readdir(rd))) {
            if (!strncmp(re->d_name, "wayland-", 8) && !strstr(re->d_name, ".lock") && !wname[0]) snprintf(wname, sizeof wname, "%s", re->d_name);
            if (!strncmp(re->d_name, "xauth_", 6) && !xauth[0]) snprintf(xauth, sizeof xauth, "XAUTHORITY=%s/%s", rdir, re->d_name);
        }
        if (rd) closedir(rd);
        const char *wd = getenv("WAYLAND_DISPLAY"), *dp = getenv("DISPLAY"), *xa = getenv("XAUTHORITY");
        if (!xauth[0] && xa && *xa) snprintf(xauth, sizeof xauth, "XAUTHORITY=%s", xa);
        if (!xauth[0]) snprintf(xauth, sizeof xauth, "XAUTHORITY=/home/%s/.Xauthority", user);
        char wl[64], dsp[64], home[300];
        snprintf(wl, sizeof wl, "WAYLAND_DISPLAY=%s", wname[0] ? wname : (wd && *wd ? wd : "wayland-0"));
        snprintf(dsp, sizeof dsp, "DISPLAY=%s", dp && *dp ? dp : ":0");
        snprintf(home, sizeof home, "HOME=/home/%s", user);
        if (setgid(gidn) || setuid(uidn)) _exit(1);
        char app[200]; snprintf(app, sizeof app, "--app=%s", url);
        const char *chromes[] = { "/usr/bin/chromium", "/usr/bin/google-chrome-stable", "/usr/bin/brave", "/usr/bin/microsoft-edge-stable", NULL };
        char *envp[] = { rt, bus, wl, dsp, xauth, home, "PATH=/usr/local/bin:/usr/bin:/bin", NULL };
        for (int i = 0; chromes[i]; i++) if (access(chromes[i], X_OK) == 0) execle(chromes[i], chromes[i], app, (char *)NULL, envp);
        execle("/usr/bin/xdg-open", "xdg-open", url, (char *)NULL, envp);
        _exit(1);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

// The secret key for the app window, readable by root and the administrators' group.
static void write_token_file(void) {
    mkdir("/run/usbeam", 0755);
    int fd = open("/run/usbeam/ui-token.new", O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) { logm("Could not write the app key: %s", strerror(errno)); return; }
    if (write(fd, g_ui_token, strlen(g_ui_token)) < 0) logm("Could not write the app key");
    // Administrators: the "wheel" group (Arch, Fedora, openSUSE...), "sudo" (Debian, Ubuntu, Mint...) or "admin".
    gid_t gid = 0;
    const char *groups[] = { "wheel", "sudo", "admin" };
    for (int gi = 0; gi < 3 && !gid; gi++) {
        FILE *gf = fopen("/etc/group", "r");
        char gl[1024], pre[16]; snprintf(pre, sizeof pre, "%s:", groups[gi]);
        while (gf && fgets(gl, sizeof gl, gf)) { unsigned g; if (!strncmp(gl, pre, strlen(pre)) && sscanf(gl + strlen(pre), "%*[^:]:%u:", &g) == 1) { gid = (gid_t)g; break; } }
        if (gf) fclose(gf);
    }
    if (fchown(fd, 0, gid)) logm("Could not set the app key's group");
    close(fd);
    rename("/run/usbeam/ui-token.new", "/run/usbeam/ui-token");
}

static int g_uitest;

// The app window's requests run with the host lock held (they read and change shared devices).
static int ui_api_locked(const char *method, const char *path, const char *body, sb_t *out) {
    host_lock();
    int r = ui_api(method, path, body, out);
    host_unlock();
    return r;
}

// Requests from other PCs (port 3240) and the once-a-second host checks.
static void *host_main(void *arg) {
    int ls = (int)(intptr_t)arg, one = 1;
    time_t last_tick = 0;
    while (!g_stop) {
        struct pollfd pf = { ls, POLLIN, 0 };
        if (poll(&pf, 1, 250) > 0 && (pf.revents & POLLIN)) {
            struct sockaddr_in ca; socklen_t cl = sizeof ca;
            int s = accept(ls, (struct sockaddr *)&ca, &cl);
            if (s >= 0 && !net_ip_allowed(ntohl(ca.sin_addr.s_addr))) { close(s); s = -1; }   // not a network USBeam is used on
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
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                uint8_t op[8];
                if (!recv_all(s, op, 8)) {
                    uint16_t code = (uint16_t)(op[2] << 8 | op[3]);
                    host_lock();
                    if (code == OP_REQ_DEVLIST) handle_devlist(s, peer);
                    else if (code == OP_REQ_INFO) handle_info(s);
                    else if (code == OP_REQ_HELLO) handle_hello(s, peer, op);
                    else if (code == OP_REQ_IMPORT) {
                        handle_import(s, peer);
                        struct timeval z = {0, 0};      // the kernel uses this connection now: no time limits
                        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z);
                        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof z);
                    }
                    host_unlock();
                }
                close(s);   // after an export the kernel holds its own reference
            }
        }
        time_t now = time(NULL);
        if (now != last_tick) { last_tick = now; host_lock(); host_tick(); host_unlock(); }
    }
    return NULL;
}

// Discovery: announce this PC every 3 s, hear other PCs, and keep the firewall rules
// up to date when a network appears later (for example ZeroTier after boot).
static void *disco_main(void *arg) {
    int us = (int)(intptr_t)arg;
    time_t next_announce = 0;
    unsigned long sig = 0;
    while (!g_stop) {
        struct pollfd pf = { us, POLLIN, 0 };
        if (poll(&pf, 1, 500) > 0 && (pf.revents & POLLIN)) disco_receive(us);
        time_t now = time(NULL);
        if (now >= next_announce) {
            unsigned long s2 = nets_refresh();          // networks come and go (Wi-Fi, ZeroTier after boot...)
            if (s2 != sig) { sig = s2; if (!g_uitest) firewall_sync(); }
            disco_announce(us);
            next_announce = now + 3;
        }
    }
    return NULL;
}

static int cmd_app(int service) {
    if (geteuid() != 0) { printf("USBeam's service needs admin rights (it is normally started by systemd).\n"); return 1; }
    struct stat st;
    int uitest = getenv("USBEAM_UITEST") != NULL;     // developer test: run without USB drivers
    if (!uitest) {
        // Missing drivers don't stop USBeam: it runs and the window explains what to install.
        if (stat(STUB, &st) != 0 && (system("modprobe usbip-host 2>/dev/null") != 0 || stat(STUB, &st) != 0)) g_have_stub = 0;
        if (port_status(0) < 0 && (system("modprobe vhci-hcd 2>/dev/null") != 0 || port_status(0) < 0)) g_have_vhci = 0;
        if (!g_have_stub || !g_have_vhci) {
            struct utsname un; uname(&un);
            snprintf(g_driver_note, sizeof g_driver_note,
                     "Linux's USB/IP drivers are missing, so this PC can't %s. Install your distribution's extra kernel modules "
                     "(on Ubuntu: linux-modules-extra-%s), then restart the PC.",
                     !g_have_stub && !g_have_vhci ? "share or use devices" : !g_have_stub ? "share its devices" : "use devices from other PCs", un.release);
            logm("%s", g_driver_note);
        }
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    gethostname(g_name, sizeof g_name);
    prefs_init();
    FILE *bf = fopen("/proc/sys/kernel/random/boot_id", "r");     // changes on every power-on, not on sleep
    if (bf) { if (!fgets(g_boot_id, sizeof g_boot_id, bf)) g_boot_id[0] = 0; fclose(bf); g_boot_id[strcspn(g_boot_id, "\n")] = 0; }
    load_settings();
    if (!g_id[0]) { make_random_id(g_id); save_settings(); }

    // Safety net: return anything left bound to usbip-host.
    DIR *dd = opendir(USBDEV); struct dirent *e;
    while (dd && (e = readdir(dd))) if (e->d_name[0] != '.' && !strchr(e->d_name, ':') && bound_to_stub(e->d_name)) { logm("[%s] was left shared: returning it to this PC", e->d_name); return_to_host(e->d_name); }
    if (dd) closedir(dd);

    int one = 1;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) { printf("Cannot use port %d: %s. Is USBeam already running?\n", USBIP_PORT, strerror(errno)); return 1; }

    int us = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(us, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(us, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in ua = {0};
    ua.sin_family = AF_INET; ua.sin_port = htons(DISCO_PORT); ua.sin_addr.s_addr = INADDR_ANY;
    if (bind(us, (struct sockaddr *)&ua, sizeof ua) < 0) logm("Discovery port %d busy: other PCs must be added by address", DISCO_PORT);

    ui_make_token();
    int uis = ui_listen();
    if (uis < 0) { printf("Cannot use local port %d for the app window. Is USBeam already running?\n", UI_PORT); return 1; }

    nets_refresh();          // which networks USBeam may be used on
    if (!uitest) vhci_cleanup();
    write_token_file();
    g_uitest = uitest;
    pthread_t ct, ht, dt;
    pthread_create(&ct, NULL, client_main, NULL);
    pthread_create(&ht, NULL, host_main, (void *)(intptr_t)ls);
    pthread_create(&dt, NULL, disco_main, (void *)(intptr_t)us);
    logm("USBeam %s for Linux started on %s", USBEAM_VERSION, g_name);
    if (!service) open_window();

    // This thread serves the app window only, so nothing else can make the window hang.
    while (!g_stop) {
        struct pollfd pf = { uis, POLLIN, 0 };
        if (poll(&pf, 1, 250) > 0 && (pf.revents & POLLIN)) {
            int c = accept(uis, NULL, NULL);
            if (c >= 0) {
                struct timeval tv = { 2, 0 };
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                ui_serve(c, ui_api_locked);
                close(c);
            }
        }
    }
    logm("Stopping: returning all devices and releasing remote ones...");
    pthread_join(ht, NULL);
    pthread_join(dt, NULL);
    host_lock();
    for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0] && g_h[i].exported) return_to_host(g_h[i].busid);
    host_unlock();
    pthread_join(ct, NULL);
    client_release_all();
    unlink("/run/usbeam/ui-token");
    if (g_restart) { logm("Restarting..."); return 3; }    // systemd starts the service again (Restart=on-failure)
    return 0;
}
