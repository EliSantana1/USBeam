// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// usbeam.c - USBeam for Linux: client and host (test version 4).
//
//   usbeam list <host-ip>                 show devices the host is sharing
//   sudo usbeam use <host-ip> <busid>...  attach devices and KEEP them attached:
//                                            reconnects automatically after unplug,
//                                            host restart or network drop.
//                                            Ctrl+C detaches everything and exits.
//
// Uses the kernel's built-in USB/IP receiver (vhci-hcd). Also raises the
// transfer size of shared drives (max_sectors) for better speed.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define USBIP_PORT 3240
#define USBIP_VERSION 0x0111
#define OP_REQ_DEVLIST 0x8005
#define OP_REQ_IMPORT 0x8003
#define VHCI "/sys/devices/platform/vhci_hcd.0"
#define ST_NOTASSIGNED 5   // attached, device still being set up by Linux
#define ST_USED 6

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void logm(const char *fmt, ...) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    printf("%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout);
}

// ---------------------------------------------------------------- network
static int recv_all(int s, void *b, size_t n) {
    char *p = b;
    while (n) { ssize_t r = recv(s, p, n, 0); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}
static int send_all(int s, const void *b, size_t n) {
    const char *p = b;
    while (n) { ssize_t r = send(s, p, n, MSG_NOSIGNAL); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

#include "netsock.h"

static int connect_host(const char *ip) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { logm("'%s' is not an IP address", ip); return -1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 30, 0 };           // handshake timeout: the host may need a few seconds to switch drivers
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    tune_device_socket(s);
    return s;
}

static void op_header(uint8_t *h, uint16_t code) {
    memset(h, 0, 8);
    h[0] = USBIP_VERSION >> 8; h[1] = USBIP_VERSION & 0xFF; h[2] = code >> 8; h[3] = code & 0xFF;
}

static const char *speed_name(uint32_t s) {
    switch (s) { case 1: return "low"; case 2: return "full"; case 3: return "high"; case 5: return "super"; case 6: return "super+"; }
    return "?";
}

static int cmd_list(const char *ip) {
    int s = connect_host(ip);
    if (s < 0) { printf("Could not reach %s on port %d (%s).\n", ip, USBIP_PORT, strerror(errno)); return 1; }
    uint8_t h[12];
    op_header(h, OP_REQ_DEVLIST);
    if (send_all(s, h, 8) || recv_all(s, h, 12)) { printf("No reply from %s.\n", ip); close(s); return 1; }
    uint32_t n = g32(h + 8);
    printf("Devices shared by %s: %u\n", ip, n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t d[312];
        if (recv_all(s, d, 312)) break;
        printf("  bus id %-6s  %04x:%04x  %s speed  %u interface(s)\n", (char *)d + 256, g16(d + 300), g16(d + 302), speed_name(g32(d + 296)), d[311]);
        uint8_t ifc[4];
        for (int k = 0; k < d[311]; k++) recv_all(s, ifc, 4);
    }
    close(s);
    return 0;
}

// ---------------------------------------------------------------- vhci (kernel receiver)
// Find a free virtual port of the right kind (hs for USB 1/2, ss for USB 3).
static int free_port(int super) {
    char path[64];
    for (int c = 0; c < 16; c++) {
        if (c == 0) snprintf(path, sizeof path, VHCI "/status");
        else snprintf(path, sizeof path, VHCI "/status.%d", c);
        FILE *f = fopen(path, "r");
        if (!f) { if (c == 0) return -2; break; }
        char line[256], hub[8];
        int port, sta;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }   // header
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%7s %d %d", hub, &port, &sta) != 3) continue;
            if (sta == 4 && (super ? !strcmp(hub, "ss") : !strcmp(hub, "hs"))) { fclose(f); return port; }
        }
        fclose(f);
    }
    return -1;
}

static int port_status(int port) {
    char path[64], line[256], hub[8];
    for (int c = 0; c < 16; c++) {
        if (c == 0) snprintf(path, sizeof path, VHCI "/status");
        else snprintf(path, sizeof path, VHCI "/status.%d", c);
        FILE *f = fopen(path, "r");
        if (!f) break;
        int p, sta;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%7s %d %d", hub, &p, &sta) == 3 && p == port) { fclose(f); return sta; }
        fclose(f);
    }
    return -1;
}

static int write_sysfs(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int r = fputs(val, f) < 0 ? -1 : 0;
    if (fclose(f) != 0) r = -1;
    return r;
}

static void detach_port(int port) {
    char v[16]; snprintf(v, sizeof v, "%d", port);
    write_sysfs(VHCI "/detach", v);
}

// Raise max_sectors on any disk that sits behind the virtual controller.
static void tune_drives(void) {
    DIR *dir = opendir("/sys/block");
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "sd", 2)) continue;
        char link[PATH_MAX], real[PATH_MAX], ms[PATH_MAX], buf[32] = "";
        snprintf(link, sizeof link, "/sys/block/%s", e->d_name);
        if (!realpath(link, real) || !strstr(real, "vhci_hcd")) continue;
        snprintf(ms, sizeof ms, "/sys/block/%s/device/max_sectors", e->d_name);
        FILE *f = fopen(ms, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
        if (atoi(buf) < 2048 && write_sysfs(ms, "2048") == 0)
            logm("Speed tuning: /dev/%s now uses 1 MB transfers", e->d_name);
    }
    closedir(dir);
}

// ---------------------------------------------------------------- attach
typedef struct { char busid[32]; int port; int was_attached; time_t next_try; int quiet; } Want;

static char g_attach_err[160];     // why the last attach failed, in plain words (shown in the app)
static int attach_one(const char *ip, Want *w) {
    int s = connect_host(ip);
    if (s < 0) { snprintf(g_attach_err, sizeof g_attach_err, "The other PC didn't answer."); if (!w->quiet) logm("[%s] host %s not reachable, will keep trying", w->busid, ip); return -1; }
    uint8_t req[40];
    op_header(req, OP_REQ_IMPORT);
    memset(req + 8, 0, 32);
    strncpy((char *)req + 8, w->busid, 31);
    uint8_t rep[8 + 312];
    if (send_all(s, req, 40) || recv_all(s, rep, 8)) { close(s); snprintf(g_attach_err, sizeof g_attach_err, "The other PC didn't answer."); if (!w->quiet) logm("[%s] no reply from host, will keep trying", w->busid); return -1; }
    if (g32(rep + 4) != 0) {
        close(s);
        snprintf(g_attach_err, sizeof g_attach_err, "The other PC didn't hand it over. It may have just been taken, unplugged or kept there.");
        if (!w->quiet) logm("[%s] not available on host (not shared, unplugged, or in use), will keep trying", w->busid);
        return -1;
    }
    if (recv_all(s, rep + 8, 312)) { close(s); return -1; }
    uint8_t *d = rep + 8;
    uint32_t busnum = g32(d + 288), devnum = g32(d + 292), speed = g32(d + 296);
    int port = free_port(speed >= 5);
    if (port < 0) { close(s); snprintf(g_attach_err, sizeof g_attach_err, "All of this PC's virtual USB ports are in use. Release a device first."); logm("[%s] no free virtual USB port", w->busid); return -1; }
    struct timeval tv = { 0, 0 };            // kernel owns the socket from here: no timeouts
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char val[64];
    snprintf(val, sizeof val, "%d %d %u %u", port, s, (busnum << 16) | devnum, speed);
    if (write_sysfs(VHCI "/attach", val) < 0) {
        snprintf(g_attach_err, sizeof g_attach_err, "Linux refused to connect the device (%s).", strerror(errno));
        logm("[%s] kernel refused attach (%s)", w->busid, strerror(errno));
        close(s); return -1;
    }
    close(s);   // the kernel keeps its own reference to the connection
    w->port = port;
    w->was_attached = 1;
    w->quiet = 0;
    logm("[%s] ATTACHED %04x:%04x (%s speed) on virtual port %d", w->busid, g16(d + 300), g16(d + 302), speed_name(speed), port);
    return 0;
}

static int cmd_use(const char *ip, int n, char **busids) {
    if (geteuid() != 0) { printf("Run with sudo: attaching USB devices needs admin rights.\n"); return 1; }
    if (port_status(0) < 0) {
        if (system("modprobe vhci-hcd") != 0 || port_status(0) < 0) { printf("Could not load the vhci-hcd kernel driver.\n"); return 1; }
        logm("Loaded vhci-hcd kernel driver");
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    Want *w = calloc(n, sizeof *w);
    for (int i = 0; i < n; i++) { strncpy(w[i].busid, busids[i], 31); w[i].port = -1; }
    logm("Keeping %d device(s) from %s attached. Press Ctrl+C to detach and quit.", n, ip);

    while (!g_stop) {
        time_t now = time(NULL);
        for (int i = 0; i < n; i++) {
            if (w[i].port >= 0) {
                int st = port_status(w[i].port);
                if (st == ST_USED || st == ST_NOTASSIGNED) continue;
                logm("[%s] connection lost (device unplugged, unshared, or network drop). Reconnecting...", w[i].busid);
                w[i].port = -1; w[i].next_try = 0; w[i].quiet = 0;
            }
            if (now < w[i].next_try) continue;
            if (attach_one(ip, &w[i]) < 0) { w[i].next_try = now + 2; w[i].quiet = 1; }
        }
        tune_drives();
        for (int k = 0; k < 10 && !g_stop; k++) usleep(100000);   // ~1 s
    }
    logm("Detaching...");
    for (int i = 0; i < n; i++) if (w[i].port >= 0 && (port_status(w[i].port) == ST_USED || port_status(w[i].port) == ST_NOTASSIGNED)) { detach_port(w[i].port); logm("[%s] detached", w[i].busid); }
    free(w);
    return 0;
}

#include "host_linux.c"
#include "app_linux.c"

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--service")) return cmd_app(1);
    if (argc == 2 && !strcmp(argv[1], "--cleanup")) return cmd_cleanup();
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("%s\n", USBEAM_VERSION); return 0; }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "app"))) return cmd_app(0);
    if (argc >= 3 && !strcmp(argv[1], "list")) return cmd_list(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "use")) return cmd_use(argv[2], argc - 3, argv + 3);
    if (argc >= 2 && !strcmp(argv[1], "host")) return cmd_host();
    printf("USBeam for Linux - test version 5\n\n"
           "  usbeam list <host-ip>                  show devices the host is sharing\n"
           "  sudo usbeam use <host-ip> <busid> ...  attach and keep attached (auto-reconnect)\n"
           "  sudo usbeam host                       share this PC's USB devices\n"
           "  sudo usbeam                            start the USBeam app (opens its window)\n");
    return 1;
}
