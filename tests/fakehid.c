// SPDX-License-Identifier: GPL-3.0-or-later
// Pretend "other PC" for the Windows smooth-input test: a USB/IP server on 127.0.0.1:3240 with one
// simulated USB device (bus id 1-1), a HID device that has a report ready every 1 ms while reads are
// waiting, like a 1000 Hz mouse (vendor-defined, so a test program may read it; Windows keeps mice
// to itself). Each report carries a counter. Replies are delayed like a busy home Wi-Fi link:
// ~2.7 ms each way, uneven, rare 20 ms pauses; order is kept (as TCP does).
//   fakehid [count] [nodelay]   serves connections one after another; each gets `count` reports
//                               (default 4000); nodelay: no simulated network delay
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static uint64_t now_ns(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
}
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int rcv(SOCKET s, void *b, int n) { char *p = b; while (n) { int r = recv(s, p, n, 0); if (r <= 0) return -1; p += r; n -= r; } return 0; }
static int snd(SOCKET s, const void *b, int n) { const char *p = b; while (n) { int r = send(s, p, n, 0); if (r <= 0) return -1; p += r; n -= r; } return 0; }

static const uint8_t DEV[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 1 };
static const uint8_t REPDESC[] = { 0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00,
                                   0x75, 0x08, 0x95, 0x08, 0x09, 0x01, 0x81, 0x02, 0xC0 };
static uint8_t CFG[9 + 9 + 9 + 7];
static void make_cfg(void) {
    uint8_t c[] = { 9, 2, sizeof CFG, 0, 1, 1, 0, 0x80, 50,
                    9, 4, 0, 0, 1, 3, 0, 0, 0,
                    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof REPDESC, 0,
                    7, 5, 0x81, 3, 8, 0, 1 };
    memcpy(CFG, c, sizeof CFG);
}

static unsigned seed = 4242; static int g_delay = 1;
static double rnd(void) { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFFFF) / 16777216.0; }
static uint64_t delay(void) { if (!g_delay) return 0; double ms = 1.2 + -log(1.0 - rnd()) * 1.5; if (rnd() < 0.005) ms += 20; return (uint64_t)(ms * 1e6); }

typedef struct { uint64_t at; int n; uint8_t m[48 + 64]; } Out;
static Out q[65536]; static int qh, qn; static uint64_t last;
static void queue(const uint8_t *h, const uint8_t *data, int n) {
    Out *o = &q[(qh + qn++) % 65536];
    memcpy(o->m, h, 48); if (n) memcpy(o->m + 48, data, n); o->n = 48 + n;
    o->at = now_ns() + delay(); if (o->at < last) o->at = last; last = o->at;
}
static void reply(uint32_t seq, int32_t status, const uint8_t *data, int n) {
    uint8_t h[48] = {0}; p32(h, 3); p32(h + 4, seq); p32(h + 20, (uint32_t)status); p32(h + 24, (uint32_t)n);
    queue(h, data, n);
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 4000;
    if (argc > 2 && !strcmp(argv[2], "nodelay")) g_delay = 0;
    make_cfg();
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    timeBeginPeriod(1);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL one = TRUE; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(3240); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 4)) { printf("fakehid: port 3240 busy\n"); return 1; }
    printf("fakehid: ready\n"); fflush(stdout);
    for (;;) {                                         // a connection that imports 1-1 is served; others closed
        SOCKET s = accept(l, NULL, NULL);
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
        uint8_t op[8], bus[32];
        if (rcv(s, op, 8) || op[3] != 0x03 || rcv(s, bus, 32)) { closesocket(s); continue; }
        uint8_t r[8 + 312] = {0};
        p16(r, 0x0111); p16(r + 2, 0x0003);
        uint8_t *d = r + 8;
        strcpy((char *)d, "/sys/devices/fake/1-1"); strcpy((char *)d + 256, "1-1");
        p32(d + 288, 1); p32(d + 292, 2); p32(d + 296, 2);          // bus 1, device 2, full speed
        p16(d + 300, 0x1209); p16(d + 302, 0x0001); p16(d + 304, 0x0100);
        d[306] = 0; d[307] = 0; d[308] = 0; d[309] = 1; d[310] = 1; d[311] = 1;
        snd(s, r, sizeof r);
        printf("fakehid: device handed over\n"); fflush(stdout);
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        uint8_t h[48], buf[4096]; int hn = 0;
        struct { uint32_t seq; uint64_t arrive; } pend[512]; int np = 0;
        uint64_t dev_next = now_ns(); uint32_t counter = 0; int done = 0;
        while (!done) {
            // requests that came in
            for (;;) {
                if (hn < 48) { int k = recv(s, (char *)h + hn, 48 - hn, 0); if (k == 0) { done = 1; break; } if (k < 0) break; hn += k; if (hn < 48) continue; }
                uint32_t cmd = g32(h), seq = g32(h + 4), dir = g32(h + 12), ep = g32(h + 16), len = g32(h + 24);
                if (cmd == 1 && !dir && len) {                 // OUT data follows (none expected here, read it anyway)
                    nb = 0; ioctlsocket(s, FIONBIO, &nb); if (rcv(s, buf, (int)(len > 4096 ? 4096 : len))) { done = 1; break; } nb = 1; ioctlsocket(s, FIONBIO, &nb);
                }
                hn = 0;
                if (cmd == 2) {                                // cancel
                    uint32_t target = g32(h + 20); int found = 0;
                    for (int i = 0; i < np; i++) if (pend[i].seq == target) { memmove(&pend[i], &pend[i + 1], (size_t)(np - i - 1) * sizeof pend[0]); np--; found = 1; break; }
                    uint8_t u[48] = {0}; p32(u, 4); p32(u + 4, seq); p32(u + 20, found ? (uint32_t)-104 : 0); queue(u, NULL, 0);
                    continue;
                }
                if (cmd != 1) { done = 1; break; }
                if (ep == 1 && dir) { if (np < 512) { pend[np].seq = seq; pend[np].arrive = now_ns() + delay(); np++; } continue; }
                if (ep != 0) { reply(seq, -32, NULL, 0); continue; }
                uint8_t bm = h[40], rq = h[41]; uint16_t val = (uint16_t)(h[42] | h[43] << 8); uint16_t wl = (uint16_t)(h[46] | h[47] << 8);
                const uint8_t *data = NULL; int n = 0; int32_t st = 0;
                uint8_t zero2[2] = {0, 0};
                if (bm == 0x80 && rq == 6) {                   // GET_DESCRIPTOR
                    uint8_t type = (uint8_t)(val >> 8);
                    if (type == 1) { data = DEV; n = 18; } else if (type == 2) { data = CFG; n = sizeof CFG; } else st = -32;
                } else if (bm == 0x81 && rq == 6 && (val >> 8) == 0x22) { data = REPDESC; n = sizeof REPDESC; }
                else if (bm == 0x80 && rq == 0) { data = zero2; n = 2; }       // GET_STATUS
                else if ((bm & 0x60) == 0x20 && rq == 0x0A) st = 0;            // SET_IDLE
                else if (bm == 0x00 && (rq == 9 || rq == 5 || rq == 3 || rq == 1)) st = 0;
                else if (bm == 0x01 && rq == 11) st = 0;                       // SET_INTERFACE
                else if (bm == 0x02 && rq == 1) st = 0;                        // CLEAR_FEATURE(halt)
                else st = -32;
                if (n > wl) n = wl;
                reply(seq, st, data, st ? 0 : n);
            }
            uint64_t t = now_ns();
            while (np && pend[0].arrive <= t && dev_next <= t && counter < (uint32_t)count) {   // the device: one report per 1 ms
                uint8_t rep[8] = {0}; memcpy(rep, &counter, 4); counter++;
                reply(pend[0].seq, 0, rep, 8);
                memmove(&pend[0], &pend[1], (size_t)(np - 1) * sizeof pend[0]); np--;
                dev_next = (dev_next + 1000000ull > t ? dev_next : t) + 1000000ull;
            }
            while (qn && q[qh].at <= now_ns()) {
                nb = 0; ioctlsocket(s, FIONBIO, &nb);
                if (snd(s, q[qh].m, q[qh].n)) { done = 1; break; }
                nb = 1; ioctlsocket(s, FIONBIO, &nb);
                qh = (qh + 1) % 65536; qn--;
            }
            Sleep(0);
        }
        closesocket(s);
        printf("fakehid: connection ended after %u reports\n", counter); fflush(stdout);
        qh = qn = 0; last = 0;
    }
}
