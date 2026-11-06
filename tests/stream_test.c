// SPDX-License-Identifier: GPL-3.0-or-later
// Smooth-input test (no admin rights or hardware needed). A simulated 1000 Hz mouse sits on the
// other side of a simulated home network (delay like a busy Wi-Fi link: ~2.7 ms each way, uneven,
// with rare 20 ms pauses). A simulated Linux HID driver asks for one report at a time, like the
// real one. Measured on the receiving side: reports per second, how even the timing is, and that
// every report arrives once, in order. Runs once directly (how USBeam worked before) and once
// through the relay in src/linux/stream.h. Passes only if the relay is smooth and the direct way
// is clearly worse (so the test really sees the problem).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdarg.h>
static void logm(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
#include "../src/linux/stream.h"

#define REPORTS 3000
static uint64_t made_at[1 << 16];   // when the simulated mouse made report number n
static int fails;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "OK" : "FAIL", what); if (!ok) fails++; }

// ---- simulated other PC: a mouse with a report ready every 1 ms while reads are waiting
typedef struct { int s; uint64_t t0; unsigned seed; } Host;
static double rnd(unsigned *s) { *s = *s * 1103515245u + 12345u; return ((*s >> 8) & 0xFFFFFF) / 16777216.0; }
static uint64_t net_delay(unsigned *s) {     // one way, ns
    double ms = 1.2 + -log(1.0 - rnd(s)) * 1.5;
    if (rnd(s) < 0.005) ms += 20;
    return (uint64_t)(ms * 1e6);
}
static void sleep_until(uint64_t t) { uint64_t n = st_now(); if (t > n) { struct timespec ts = { (time_t)((t - n) / 1000000000ull), (long)((t - n) % 1000000000ull) }; nanosleep(&ts, NULL); } }

typedef struct { uint64_t at; uint8_t m[48 + 6]; int n; } Out;
static void *host_main(void *arg) {
    Host *h = arg;
    static Out q[65536]; int qh = 0, qn = 0;
    uint64_t dev_next = st_now(), last_send = 0;
    uint32_t counter = 0;
    uint8_t b[48];
    struct { uint32_t seq; uint64_t arrive; } pend[64]; int np = 0;
    for (;;) {
        // read every request available now
        for (;;) {
            struct timeval tv = { 0, 200 };
            fd_set fs; FD_ZERO(&fs); FD_SET(h->s, &fs);
            if (select(h->s + 1, &fs, NULL, NULL, &tv) <= 0) break;
            if (st_recv(h->s, b, 48)) return NULL;
            uint32_t cmd = st_g32(b), seq = st_g32(b + 4);
            if (cmd == 1 && np < 64) { pend[np].seq = seq; pend[np].arrive = st_now() + net_delay(&h->seed); np++; }
            else if (cmd == 2) {                     // cancel: answer it, drop the target if waiting
                uint32_t target = st_g32(b + 20);
                for (int i = 0; i < np; i++) if (pend[i].seq == target) { memmove(&pend[i], &pend[i + 1], (np - i - 1) * sizeof pend[0]); np--; break; }
                Out *o = &q[(qh + qn++) % 65536];
                memset(o->m, 0, 48); st_p32(o->m, 4); st_p32(o->m + 4, seq); st_p32(o->m + 20, (uint32_t)-104); o->n = 48;
                o->at = st_now() + net_delay(&h->seed); if (o->at < last_send) o->at = last_send; last_send = o->at;
            }
        }
        uint64_t now = st_now();
        // the oldest waiting read gets the next report once it has reached this PC and the mouse has one
        while (np && pend[0].arrive <= now && dev_next <= now) {
            Out *o = &q[(qh + qn++) % 65536];
            memset(o->m, 0, 48);
            st_p32(o->m, 3); st_p32(o->m + 4, pend[0].seq); st_p32(o->m + 24, 6);
            made_at[counter & 0xFFFF] = now;
            st_p32(o->m + 48, counter++); o->m[52] = 2; o->m[53] = 1;     // report: counter, dx 2, dy 1
            o->n = 54;
            o->at = now + net_delay(&h->seed); if (o->at < last_send) o->at = last_send;   // TCP keeps order
            last_send = o->at;
            memmove(&pend[0], &pend[1], (np - 1) * sizeof pend[0]); np--;
            dev_next = (dev_next + 1000000ull > now ? dev_next : now) + 1000000ull;   // next report 1 ms later
            if (np && pend[0].arrive <= now) {           // like some devices: an empty reply in between
                Out *z = &q[(qh + qn++) % 65536];
                memset(z->m, 0, 48); st_p32(z->m, 3); st_p32(z->m + 4, pend[0].seq); z->n = 48;
                z->at = last_send; memmove(&pend[0], &pend[1], (np - 1) * sizeof pend[0]); np--;
            }
        }
        while (qn && q[qh].at <= st_now()) { if (st_send(h->s, q[qh].m, (size_t)q[qh].n)) return NULL; qh = (qh + 1) % 65536; qn--; }
    }
}

// ---- simulated Linux HID driver: one read at a time, a new one right after each report
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double lat_p99;
static void run(const char *name, int use_relay, double *rate_out, double *p90_out, double *p99_out) {
    int net[2], kern[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, net);
    Host h = { net[0], 0, 12345 };
    pthread_t ht; pthread_create(&ht, NULL, host_main, &h);
    int drv;
    if (use_relay) { socketpair(AF_UNIX, SOCK_STREAM, 0, kern); stream_start(net[1], kern[1], 2, "1-1"); drv = kern[0]; }
    else drv = net[1];
    static uint64_t t[REPORTS]; static double lat[REPORTS];
    uint32_t expect = 0; int in_order = 1, bad_data = 0;
    uint8_t sub[48] = {0}, r[48], d[6];
    st_p32(sub, 1); st_p32(sub + 12, 1); st_p32(sub + 16, 1); st_p32(sub + 24, 6); st_p32(sub + 36, 1);   // interrupt IN, ep 1, 1 ms
    // first a control request (passes through the relay untouched) and a cancelled read
    for (int i = 0, seq = 1; i < REPORTS; seq++) {
        st_p32(sub + 4, (uint32_t)seq);
        if (st_send(drv, sub, 48) || st_recv(drv, r, 48) || st_g32(r) != 3 || st_g32(r + 4) != (uint32_t)seq) { printf("%s: wrong reply\n", name); fails++; break; }
        if (st_g32(r + 24) == 0) continue;                 // an empty reply: ask again
        if (st_g32(r + 24) != 6 || st_recv(drv, d, 6)) { bad_data++; break; }
        t[i] = st_now();
        uint32_t c = st_g32(d);
        lat[i] = (t[i] - made_at[(c ? c - 1 : 0) & 0xFFFF]) / 1e6;   // since the movement in it began
        if (c != expect) in_order = 0;
        expect = c + 1;
        if (d[4] != 2 || d[5] != 1) bad_data++;
        i++;
    }
    double secs = (t[REPORTS - 1] - t[0]) / 1e9, gaps[REPORTS];
    for (int i = 1; i < REPORTS; i++) gaps[i - 1] = (t[i] - t[i - 1]) / 1e6;
    int n = REPORTS - 1;
    for (int i = 1; i < n; i++) { double v = gaps[i]; int j = i - 1; while (j >= 0 && gaps[j] > v) { gaps[j + 1] = gaps[j]; j--; } gaps[j + 1] = v; }
    *rate_out = n / secs; *p90_out = gaps[(int)(0.9 * (n - 1))]; *p99_out = gaps[(int)(0.99 * (n - 1))];
    qsort(lat, REPORTS, sizeof lat[0], cmpd);
    lat_p99 = lat[(int)(0.99 * (REPORTS - 1))];
    printf("%s: %.0f reports/s, gap ms median %.2f p90 %.2f p99 %.2f max %.2f; delay ms median %.1f p99 %.1f; in order: %s; data ok: %s\n",
           name, *rate_out, gaps[n / 2], gaps[(int)(0.9 * (n - 1))], *p99_out, gaps[n - 1], lat[REPORTS / 2], lat_p99, in_order ? "yes" : "NO", bad_data ? "NO" : "yes");
    if (use_relay) check(in_order && !bad_data, "relay: every report arrives once, in order, unchanged");
    close(drv);
}

// ---- everything else passes through unchanged; a cancelled read is answered; closing ends both
static void *echo_host(void *arg) {           // answers control and bulk requests at once, never answers ep 1
    int s = *(int *)arg; uint8_t b[48], *data = malloc(4096); int cancels = 0;
    while (!st_recv(s, b, 48)) {
        uint32_t cmd = st_g32(b), seq = st_g32(b + 4), dir = st_g32(b + 12), ep = st_g32(b + 16), len = st_g32(b + 24);
        uint8_t r[48] = {0};
        if (cmd == 2) { cancels++; st_p32(r, 4); st_p32(r + 4, seq); st_p32(r + 20, (uint32_t)-104); st_send(s, r, 48); continue; }
        if (ep == 1) continue;                                // interrupt reads just wait
        if (!dir && len && st_recv(s, data, len)) break;      // OUT data
        st_p32(r, 3); st_p32(r + 4, seq); st_p32(r + 24, dir ? len : len);
        for (uint32_t i = 0; i < len && dir; i++) data[i] = (uint8_t)(i * 7 + ep);
        st_send(s, r, 48);
        if (dir) st_send(s, data, len);
    }
    free(data);
    *(int *)arg = -1000 - cancels;                            // tell the test how many cancels arrived
    return NULL;
}
static void protocol_checks(void) {
    int net[2], kern[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, net); socketpair(AF_UNIX, SOCK_STREAM, 0, kern);
    int hs = net[0]; pthread_t t; pthread_create(&t, NULL, echo_host, &hs);
    stream_start(net[1], kern[1], 3, "1-2");
    int k = kern[0]; uint8_t h[48], r[48], d[4096];
    // control IN on ep 0
    memset(h, 0, 48); st_p32(h, 1); st_p32(h + 4, 1); st_p32(h + 12, 1); st_p32(h + 24, 18); h[40] = 0x80; h[41] = 6; h[43] = 1;
    st_send(k, h, 48);
    int ok = !st_recv(k, r, 48) && st_g32(r + 4) == 1 && st_g32(r + 24) == 18 && !st_recv(k, d, 18) && d[3] == 21;
    check(ok, "control request passes through unchanged");
    // bulk OUT, 1000 bytes on ep 2, then bulk IN, 512 bytes on ep 3 (a drive)
    memset(h, 0, 48); st_p32(h, 1); st_p32(h + 4, 2); st_p32(h + 16, 2); st_p32(h + 24, 1000);
    memset(d, 0x5A, 1000); st_send(k, h, 48); st_send(k, d, 1000);
    ok = !st_recv(k, r, 48) && st_g32(r + 4) == 2 && st_g32(r + 24) == 1000;
    memset(h, 0, 48); st_p32(h, 1); st_p32(h + 4, 3); st_p32(h + 12, 1); st_p32(h + 16, 3); st_p32(h + 24, 512);
    st_send(k, h, 48);
    ok = ok && !st_recv(k, r, 48) && st_g32(r + 4) == 3 && st_g32(r + 24) == 512 && !st_recv(k, d, 512) && d[100] == (uint8_t)(100 * 7 + 3);
    check(ok, "bulk transfers (drives) pass through unchanged");
    // an interrupt read that the driver cancels
    memset(h, 0, 48); st_p32(h, 1); st_p32(h + 4, 4); st_p32(h + 12, 1); st_p32(h + 16, 1); st_p32(h + 24, 8); st_p32(h + 36, 8);
    st_send(k, h, 48);
    usleep(100000);
    memset(h, 0, 48); st_p32(h, 2); st_p32(h + 4, 5); st_p32(h + 20, 4);
    st_send(k, h, 48);
    ok = !st_recv(k, r, 48) && st_g32(r) == 4 && st_g32(r + 4) == 5 && (int32_t)st_g32(r + 20) == -104;
    check(ok, "a cancelled read is answered as cancelled");
    usleep(100000);
    close(k);                                                 // the driver lets go of the port
    pthread_join(t, NULL);
    check(hs <= -1000, "closing on this PC closes the connection to the other PC");
    check(hs <= -1000 - ST_AHEAD, "the reads kept waiting on the other PC are cancelled too");
}

int main(void) {
    protocol_checks();
    double r0, a0, p0, r1, a1, p1;
    run("direct (as before)", 0, &r0, &a0, &p0);
    double l0 = lat_p99;
    run("through the relay ", 1, &r1, &a1, &p1);
    check(lat_p99 <= l0, "relay: movement is not delayed more than before (slowest 1%; a mouse gathers movement until it is asked)");
    check(r1 >= 900, "relay: at least 900 reports per second from a 1000 Hz mouse");
    check(a1 <= 2.0, "relay: 90% of reports at most 2 ms apart (a 1000 Hz mouse plugged in directly: 1 ms)");
    check(p1 * 2 <= p0, "relay: the slowest 1% of gaps at least twice as short as before");
    check(r0 < r1 * 0.6 && a0 > a1 * 2, "the direct way is clearly worse (the test sees the problem)");
    printf(fails ? "Smooth-input test FAILED\n" : "PASS: smooth-input test\n");
    return fails != 0;
}
