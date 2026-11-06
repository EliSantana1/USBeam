// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// relay_win.h - smooth input on Windows (included by app_win.c). usbip-win2's driver makes its own
// connection to the other PC, so USBeam puts itself in the way: for each device it opens a local
// port, tells the driver to connect there, makes the real connection to the other PC, passes the
// hand-over through, and then relays with stream.h (reads kept waiting on the other PC, reports
// handed over evenly). USBEAM_NOSTREAM=1 in the service's environment: connect directly, as before.
#include "../common/stream.h"

typedef struct { int used; uint16_t lport; int usbip_port; char host[64], busid[32]; ULONGLONG made; } WinRelay;
static WinRelay g_rel[64];
static CRITICAL_SECTION g_rel_cs;
static int g_rel_init;

static void relay_tune(SOCKET s) {                  // same settings as the sharing side (usbeam_host.c)
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    struct tcp_keepalive ka = { 1, 10000, 2000 };   // a dead network is noticed in ~20 s
    DWORD br; WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof ka, NULL, 0, &br, NULL, NULL);
    DWORD maxrt = 20, kcnt = 5;
    setsockopt(s, IPPROTO_TCP, TCP_MAXRT, (char *)&maxrt, sizeof maxrt);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, (char *)&kcnt, sizeof kcnt);
}

typedef struct { SOCKET l; char host[64], busid[32]; } RelayStart;
static DWORD WINAPI relay_accept(LPVOID arg) {
    RelayStart *rs = arg;
    fd_set f; FD_ZERO(&f); FD_SET(rs->l, &f);
    struct timeval tv = { 90, 0 };                  // the driver connects within moments of "attach"
    SOCKET drv = select(0, &f, NULL, NULL, &tv) == 1 ? accept(rs->l, NULL, NULL) : INVALID_SOCKET;
    closesocket(rs->l);
    SOCKET net = INVALID_SOCKET;
    uint8_t req[40], rep[8 + 312];
    if (drv == INVALID_SOCKET) { logm("[%s] smooth input: the USB driver didn't connect", rs->busid); goto done; }
    BOOL one = TRUE; setsockopt(drv, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    DWORD to = 30000; setsockopt(drv, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    if (!recv_all(drv, req, 40) || req[2] != 0x80 || req[3] != 0x03) { logm("[%s] smooth input: unexpected request from the USB driver", rs->busid); goto done; }
    net = plat_connect(rs->host, 15000);
    if (net == INVALID_SOCKET) { logm("[%s] smooth input: %s didn't answer", rs->busid, rs->host); goto done; }
    relay_tune(net);
    to = 30000; setsockopt(net, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);   // the other PC may need moments to switch drivers
    if (!send_all(net, req, 40) || !recv_all(net, rep, 8)) { logm("[%s] smooth input: no reply from %s", rs->busid, rs->host); goto done; }
    int n = 8;
    if (get32(rep + 4) == 0) { if (!recv_all(net, rep + 8, 312)) goto done; n += 312; }
    if (!send_all(drv, rep, n) || n == 8) goto done;        // refused: the driver gets the refusal as is
    to = 0;
    setsockopt(net, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    setsockopt(drv, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    uint32_t speed = get32(rep + 8 + 296);
    if (stream_start(net, drv, speed, rs->busid) == 0) { free(rs); return 0; }   // the relay owns both now
    net = drv = INVALID_SOCKET;
done:
    if (net != INVALID_SOCKET) closesocket(net);
    if (drv != INVALID_SOCKET) closesocket(drv);
    free(rs);
    return 0;
}

// A local port for the driver to connect to for (host, busid); 0 = smooth input not available.
static uint16_t relay_open(const char *host, const char *busid) {
    if (getenv("USBEAM_NOSTREAM")) return 0;
    if (!g_rel_init) { InitializeCriticalSection(&g_rel_cs); g_rel_init = 1; }
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0}; int al = sizeof a;
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL excl = TRUE; setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&excl, sizeof excl);
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1) || getsockname(l, (struct sockaddr *)&a, &al)) {
        if (l != INVALID_SOCKET) closesocket(l);
        logm("[%s] smooth input not available (no local port): connecting directly", busid);
        return 0;
    }
    uint16_t port = ntohs(a.sin_port);
    RelayStart *rs = calloc(1, sizeof *rs);
    rs->l = l;
    snprintf(rs->host, sizeof rs->host, "%s", host); snprintf(rs->busid, sizeof rs->busid, "%s", busid);
    HANDLE t = CreateThread(NULL, 0, relay_accept, rs, 0, NULL);
    if (!t) { closesocket(l); free(rs); return 0; }
    CloseHandle(t);
    EnterCriticalSection(&g_rel_cs);
    WinRelay *slot = NULL;
    for (int i = 0; i < 64; i++) if (!g_rel[i].used) { slot = &g_rel[i]; break; }
    if (!slot) {                                    // full: reuse the oldest
        slot = &g_rel[0];
        for (int i = 1; i < 64; i++) if (g_rel[i].made < slot->made) slot = &g_rel[i];
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1; slot->lport = port; slot->usbip_port = -1; slot->made = GetTickCount64();
    snprintf(slot->host, sizeof slot->host, "%s", host); snprintf(slot->busid, sizeof slot->busid, "%s", busid);
    LeaveCriticalSection(&g_rel_cs);
    return port;
}

// usbip-win2 lists a relayed device as 127.0.0.1:<local port>: the real PC's address, if known.
static int relay_host(uint16_t lport, char *host, size_t n) {
    if (!g_rel_init) return 0;
    int found = 0;
    EnterCriticalSection(&g_rel_cs);
    for (int i = 0; i < 64; i++) if (g_rel[i].used && g_rel[i].lport == lport) { snprintf(host, n, "%s", g_rel[i].host); found = 1; break; }
    LeaveCriticalSection(&g_rel_cs);
    return found;
}
static void relay_forget(uint16_t lport) {
    if (!g_rel_init) return;
    EnterCriticalSection(&g_rel_cs);
    for (int i = 0; i < 64; i++) if (g_rel[i].used && g_rel[i].lport == lport) g_rel[i].used = 0;
    LeaveCriticalSection(&g_rel_cs);
}
