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
#include "../common/http.h"
#include "../common/client_types.h"
#include "ui_html.h"

#define DISCO_PORT 3241

static char g_name[64];
static char g_id[33];
static char g_boot_id[64];
static char g_settings[] = "/var/lib/usbeam/settings";
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

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
static int plat_attach(const char *ip, const char *busid) {
    Want w; memset(&w, 0, sizeof w);
    snprintf(w.busid, sizeof w.busid, "%s", busid);
    w.port = -1;
    return attach_one(ip, &w) == 0 ? w.port : -1;
}
static int plat_port_alive(int port, const char *ip, const char *busid) {
    (void)ip; (void)busid;
    int st = port_status(port);
    return st == ST_USED || st == ST_NOTASSIGNED;
}
static void plat_detach(int port) { if (plat_port_alive(port, NULL, NULL)) detach_port(port); }
static const char *plat_client_note(void) { return NULL; }
static void plat_tune(void) { tune_drives(); }

#include "../common/client.h"

// ---------------------------------------------------------------- settings
static void save_settings(void) {
    mkdir("/var/lib/usbeam", 0755);
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.new", g_settings);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "id %s\n", g_id);
    for (int i = 0; i < MAXHIDE; i++) if (g_hidden[i][0]) fprintf(f, "hide %s\n", g_hidden[i]);
    for (int i = 0; i < MAXHIDE; i++) if (g_shared[i][0]) fprintf(f, "share %s\n", g_shared[i]);
    cl_lock();
    client_save_lines(f);
    cl_unlock();
    fclose(f);
    rename(tmp, g_settings);
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

static void handle_info(int s) {
    char ids[MAXH][32], name[256];
    int n = list_shareable(ids, MAXH);
    sb_t j = {0};
    sb_add(&j, "{\"name\":", 8); sb_jstr(&j, g_name);
    sb_add(&j, ",\"id\":", 6); sb_jstr(&j, g_id);
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
        if (i->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_in b = *(struct sockaddr_in *)i->ifa_broadaddr;
        b.sin_port = htons(DISCO_PORT);
        sendto(us, msg, strlen(msg), 0, (struct sockaddr *)&b, sizeof b);
    }
    freeifaddrs(ifs);
}

static void disco_receive(int us) {
    char buf[160]; struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t n = recvfrom(us, buf, sizeof buf - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n <= 8) return;
    buf[n] = 0;
    char ip[64]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    if (is_local_ip(ip)) return;
    disco_received(buf, ip);
}

// Allow USBeam through ufw for the local networks this PC is on (private addresses only).
static void firewall_setup(void) {
    if (system("command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'") != 0) return;
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs)) return;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        int priv = (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8;
        if (!priv) continue;
        int bits = 0; for (uint32_t x = m; x; x <<= 1) bits++;
        struct in_addr net; net.s_addr = htonl(a & m);
        char ns[32]; inet_ntop(AF_INET, &net, ns, sizeof ns);
        char cmd[256];
        snprintf(cmd, sizeof cmd, "ufw allow from %s/%d to any port 3240 proto tcp comment USBeam >/dev/null 2>&1", ns, bits);
        if (system(cmd) != 0) logm("Could not add firewall rule for %s/%d", ns, bits);
        snprintf(cmd, sizeof cmd, "ufw allow from %s/%d to any port 3241 proto udp comment USBeam >/dev/null 2>&1", ns, bits);
        if (system(cmd) != 0) logm("Could not add firewall rule for %s/%d", ns, bits);
    }
    freeifaddrs(ifs);
    logm("Firewall (ufw): USBeam allowed from this PC's local networks");
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
        sb_jstr(j, peer);
        sb_add(j, ",\"note\":", 8); sb_jstr(j, note);
        sb_add(j, "}", 1);
    }
    sb_add(j, "]", 1);
}

static int ui_api(const char *method, const char *path, const char *body, sb_t *out) {
    if (!strcmp(method, "GET") && !strcmp(path, "/api/state")) {
        sb_printf(out, "%s", "{\"os\":\"linux\",\"name\":"); sb_jstr(out, g_name);
        sb_printf(out, "%s", ","); add_local_json(out);
        sb_printf(out, "%s", ","); client_json(out);
        sb_printf(out, "%s", "}");
        return 200;
    }
    if (strcmp(method, "POST") || strcmp(path, "/api/action")) return 404;
    char op[32], id[64];
    json_str(body, "op", op, sizeof op);
    json_str(body, "id", id, sizeof id);
    HostDev *h = id[0] ? hdev(id, 0) : NULL;
    int code = 200;

    if (!strcmp(op, "forget_auto")) { client_forget_auto(); }
    else if (!strcmp(op, "quit")) { logm("Quit requested from the USBeam app"); g_stop = 1; }
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

// The secret key for the app window, readable by root and the "wheel" group
// (administrators, which on Arch includes the desktop user).
static void write_token_file(void) {
    mkdir("/run/usbeam", 0755);
    int fd = open("/run/usbeam/ui-token.new", O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) { logm("Could not write the app key: %s", strerror(errno)); return; }
    if (write(fd, g_ui_token, strlen(g_ui_token)) < 0) logm("Could not write the app key");
    gid_t gid = 0;                         // look up "wheel" in /etc/group (no NSS needed)
    FILE *gf = fopen("/etc/group", "r");
    char gl[1024];
    while (gf && fgets(gl, sizeof gl, gf)) { unsigned g; if (sscanf(gl, "wheel:%*[^:]:%u:", &g) == 1) { gid = (gid_t)g; break; } }
    if (gf) fclose(gf);
    if (fchown(fd, 0, gid)) logm("Could not set the app key's group");
    close(fd);
    rename("/run/usbeam/ui-token.new", "/run/usbeam/ui-token");
}

static int cmd_app(int service) {
    if (geteuid() != 0) { printf("USBeam's service needs admin rights (it is normally started by systemd).\n"); return 1; }
    struct stat st;
    int uitest = getenv("USBEAM_UITEST") != NULL;     // developer test: run without USB drivers
    if (!uitest && stat(STUB, &st) != 0 && (system("modprobe usbip-host") != 0 || stat(STUB, &st) != 0)) { printf("Could not load the usbip-host kernel driver.\n"); return 1; }
    if (!uitest && port_status(0) < 0 && (system("modprobe vhci-hcd") != 0 || port_status(0) < 0)) { printf("Could not load the vhci-hcd kernel driver.\n"); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    gethostname(g_name, sizeof g_name);
    FILE *bf = fopen("/proc/sys/kernel/random/boot_id", "r");     // changes on every power-on, not on sleep
    if (bf) { if (!fgets(g_boot_id, sizeof g_boot_id, bf)) g_boot_id[0] = 0; fclose(bf); g_boot_id[strcspn(g_boot_id, "\n")] = 0; }
    load_settings();
    if (!g_id[0]) { make_random_id(g_id); save_settings(); }
    if (!uitest) firewall_setup();

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

    write_token_file();
    pthread_t ct;
    pthread_create(&ct, NULL, client_main, NULL);
    logm("USBeam for Linux started on %s", g_name);
    if (!service) open_window();

    time_t next_announce = 0;
    while (!g_stop) {
        struct pollfd pf[3] = { { ls, POLLIN, 0 }, { uis, POLLIN, 0 }, { us, POLLIN, 0 } };
        int r = poll(pf, 3, 250);
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
                    else if (code == OP_REQ_INFO) handle_info(s);
                    else if (code == OP_REQ_IMPORT) { struct timeval z = {0, 0}; handle_import(s, peer); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z); }
                }
                close(s);
            }
        }
        if (r > 0 && (pf[1].revents & POLLIN)) {
            int c = accept(uis, NULL, NULL);
            if (c >= 0) {
                struct timeval tv = { 2, 0 };
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                ui_serve(c, ui_api);
                close(c);
            }
        }
        if (r > 0 && (pf[2].revents & POLLIN)) disco_receive(us);
        time_t now = time(NULL);
        if (now >= next_announce) { disco_announce(us); next_announce = now + 3; }
        static time_t last_tick;
        if (now != last_tick) { last_tick = now; host_tick(); }
    }
    logm("Stopping: returning all devices and releasing remote ones...");
    for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0] && g_h[i].exported) return_to_host(g_h[i].busid);
    pthread_join(ct, NULL);
    client_release_all();
    unlink("/run/usbeam/ui-token");
    return 0;
}
