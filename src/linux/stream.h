// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// stream.h - smooth mice, keyboards and controllers on the PC that uses a device (Linux).
//
// Why: Linux's HID driver asks for one input report at a time. Over the network every report then
// costs a full round trip, so a 1000 Hz mouse arrives ~150 times a second at uneven times, and
// the cursor jumps and bends (pointer acceleration works from arrival times).
//
// How: a relay sits between the kernel's virtual USB controller (vhci) and the network. For
// interrupt IN endpoints (mice, keyboards, controllers) it keeps several reads waiting on the
// PC the device is plugged into, so reports stream over without waiting to be asked. The
// kernel's own request is answered here at once from what has arrived, in order, nothing
// dropped. When a burst arrives late (the network paused), reports are handed over at about the
// device's own rate instead of all at once. Everything else (control, bulk: drives, isochronous)
// passes through untouched. Works with any USB/IP host: it only sends ordinary requests.
#ifndef USBEAM_STREAM_H
#define USBEAM_STREAM_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#define ST_AHEAD   32         // reads kept waiting on the other PC per endpoint (covers a 32 ms round trip at 1000 Hz)
#define ST_BUFMAX  128        // reports held here at most (then reading ahead pauses; nothing is dropped)
#define ST_SEQBIT  0x80000000u  // this relay's own request numbers (the kernel's stay below)
#define ST_MAXFWD  4096       // forwarded requests in flight (drives use many)

typedef struct { uint8_t hdr[48]; uint32_t len; uint8_t *data; } StRep;

typedef struct {
    int on;                   // this endpoint is streamed
    uint8_t sub[48];          // the kernel's request, as a template for reads ahead
    uint32_t wait_seq;        // the kernel's request waiting for a report (0 = none)
    int waiting;
    uint32_t len;             // buffer size the kernel asks for
    int ahead;                // reads waiting on the other PC
    int stopped;              // an error came back: no more reads ahead until the kernel asks again
    uint32_t epoch;           // bumped when the kernel cancels: late replies are dropped
    StRep q[ST_BUFMAX]; int qh, qn;
    uint64_t pace_ns, next_ns;
} StEp;

typedef struct { uint32_t seq; uint8_t ep, dir, used; uint32_t epoch; } StOut;   // our reads ahead / cancels

typedef struct {
    int net, kern;            // the connection to the other PC; our end of the kernel's socket
    uint32_t speed;           // USB/IP speed code
    char busid[32];
    pthread_mutex_t mx;       // state below
    pthread_mutex_t net_mx;   // writes to net
    pthread_mutex_t kern_mx;  // writes to kern
    pthread_cond_t cv;
    int dead;
    StEp ep[16];
    uint32_t next_seq;
    StOut out[256];           // reads ahead and cancels sent by us
    struct { uint32_t seq; uint8_t dir; uint8_t used; } fwd[ST_MAXFWD];   // kernel requests passed on
    struct { uint32_t seq, target; uint8_t used; } unl[64];               // kernel cancels passed on
    int refs;
    // numbers for the log when the connection ends
    uint64_t n_ahead, n_back, n_given, n_paced, n_stopped; int max_ahead, max_q;
} Stream;

static uint64_t st_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint32_t st_g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void st_p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int st_recv(int s, void *b, size_t n) {
    uint8_t *p = b;
    while (n) { ssize_t r = recv(s, p, n, 0); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
static int st_send(int s, const void *b, size_t n) {
    const uint8_t *p = b;
    while (n) { ssize_t r = send(s, p, n, MSG_NOSIGNAL); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
static void st_kill(Stream *st) {           // either side ended: end both (the kernel then frees the port)
    if (!st->dead) { st->dead = 1; shutdown(st->net, SHUT_RDWR); shutdown(st->kern, SHUT_RDWR); }
    pthread_cond_broadcast(&st->cv);
}
static void st_unref(Stream *st) {
    pthread_mutex_lock(&st->mx);
    int left = --st->refs;
    pthread_mutex_unlock(&st->mx);
    if (left) return;
    logm("[%s] smooth input: %llu reads ahead sent, %llu answered, %llu reports handed over (%llu spaced out), stopped %llu times, most waiting there %d, most queued here %d",
         st->busid, (unsigned long long)st->n_ahead, (unsigned long long)st->n_back, (unsigned long long)st->n_given,
         (unsigned long long)st->n_paced, (unsigned long long)st->n_stopped, st->max_ahead, st->max_q);
    for (int e = 0; e < 16; e++) for (int i = 0; i < st->ep[e].qn; i++) free(st->ep[e].q[(st->ep[e].qh + i) % ST_BUFMAX].data);
    close(st->net); close(st->kern);
    pthread_mutex_destroy(&st->mx); pthread_mutex_destroy(&st->net_mx); pthread_mutex_destroy(&st->kern_mx); pthread_cond_destroy(&st->cv);
    free(st);
}

// ---- requests we send ourselves (mx held)
static StOut *st_out_new(Stream *st, uint8_t ep, uint8_t dir, uint32_t epoch) {
    for (int i = 0; i < 256; i++) if (!st->out[i].used) {
        StOut *o = &st->out[i];
        o->used = 1; o->ep = ep; o->dir = dir; o->epoch = epoch;
        o->seq = ST_SEQBIT | (st->next_seq++ & 0x7FFFFFFFu);
        if (o->seq == ST_SEQBIT) o->seq = ST_SEQBIT | (st->next_seq++ & 0x7FFFFFFFu);
        return o;
    }
    return NULL;
}
static StOut *st_out_find(Stream *st, uint32_t seq) {
    for (int i = 0; i < 256; i++) if (st->out[i].used && st->out[i].seq == seq) return &st->out[i];
    return NULL;
}
static void st_net_write(Stream *st, const uint8_t *h, const uint8_t *data, uint32_t n) {
    pthread_mutex_lock(&st->net_mx);
    if (st_send(st->net, h, 48) || (n && st_send(st->net, data, n))) st_kill(st);
    pthread_mutex_unlock(&st->net_mx);
}

// Keep ST_AHEAD reads waiting on the other PC. Called with mx held; mx is let go while sending,
// so a slow network never holds up the other direction.
static void st_fill(Stream *st, int e) {
    StEp *p = &st->ep[e];
    uint8_t h[ST_AHEAD][48]; int n = 0;
    while (n < ST_AHEAD && p->on && !p->stopped && !st->dead && p->ahead < ST_AHEAD && p->qn + p->ahead < ST_BUFMAX) {
        StOut *o = st_out_new(st, (uint8_t)e, 1, p->epoch);
        if (!o) break;
        memcpy(h[n], p->sub, 48);
        st_p32(h[n] + 4, o->seq);
        p->ahead++; n++; st->n_ahead++;
        if (p->ahead > st->max_ahead) st->max_ahead = p->ahead;
    }
    if (!n) return;
    pthread_mutex_unlock(&st->mx);
    for (int i = 0; i < n; i++) st_net_write(st, h[i], NULL, 0);
    pthread_mutex_lock(&st->mx);
}

// Answer the kernel's waiting request from the reports that arrived, at about the device's rate
// when several are queued (mx held). Returns ns until the next one may go, 0 if nothing to wait for.
static uint64_t st_deliver(Stream *st, int e) {
    StEp *p = &st->ep[e];
    if (!p->waiting || !p->qn || st->dead) return 0;
    uint64_t now = st_now();
    if (now < p->next_ns) { st->n_paced++; return p->next_ns - now; }
    StRep *r = &p->q[p->qh];
    p->qh = (p->qh + 1) % ST_BUFMAX; p->qn--;
    uint8_t h[48]; memcpy(h, r->hdr, 48);
    st_p32(h + 4, p->wait_seq);
    uint32_t n = r->len;
    if (n > p->len) n = p->len;
    st_p32(h + 24, n);
    uint8_t *data = r->data;
    p->waiting = 0; st->n_given++;
    // Several waiting: hand them over at ~1.33x the device's rate (smooth, and the delay drains).
    p->next_ns = p->qn ? now + p->pace_ns : 0;
    pthread_mutex_unlock(&st->mx);
    pthread_mutex_lock(&st->kern_mx);
    int bad = st_send(st->kern, h, 48) || (n && st_send(st->kern, data, n));
    pthread_mutex_unlock(&st->kern_mx);
    free(data);
    pthread_mutex_lock(&st->mx);
    if (bad) st_kill(st);
    st_fill(st, e);
    return 0;
}

static void *st_pacer(void *arg) {         // hands over queued reports when their time comes
    Stream *st = arg;
    pthread_mutex_lock(&st->mx);
    while (!st->dead) {
        uint64_t wait = 0;
        for (int e = 1; e < 16; e++) { uint64_t w = st_deliver(st, e); if (w && (!wait || w < wait)) wait = w; }
        if (!wait) pthread_cond_wait(&st->cv, &st->mx);
        else {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t t = (uint64_t)ts.tv_nsec + wait;
            ts.tv_sec += (time_t)(t / 1000000000ull); ts.tv_nsec = (long)(t % 1000000000ull);
            pthread_cond_timedwait(&st->cv, &st->mx, &ts);
        }
    }
    pthread_mutex_unlock(&st->mx);
    st_unref(st);
    return NULL;
}

static int st_fwd_add(Stream *st, uint32_t seq, uint8_t dir) {
    for (int i = 0; i < ST_MAXFWD; i++) if (!st->fwd[i].used) { st->fwd[i].used = 1; st->fwd[i].seq = seq; st->fwd[i].dir = dir; return 0; }
    return -1;
}
static int st_fwd_take(Stream *st, uint32_t seq, uint8_t *dir) {
    for (int i = 0; i < ST_MAXFWD; i++) if (st->fwd[i].used && st->fwd[i].seq == seq) { *dir = st->fwd[i].dir; st->fwd[i].used = 0; return 0; }
    return -1;
}

static int st_copy(int from, int to, uint64_t n, uint8_t *buf, size_t cap) {
    while (n) { size_t k = n > cap ? cap : (size_t)n; if (st_recv(from, buf, k) || st_send(to, buf, k)) return -1; n -= k; }
    return 0;
}

// Kernel -> other PC.
static void *st_up(void *arg) {
    Stream *st = arg;
    uint8_t h[48], *buf = malloc(65536);
    while (buf && !st->dead && !st_recv(st->kern, h, 48)) {
        uint32_t cmd = st_g32(h), seq = st_g32(h + 4), dir = st_g32(h + 12), ep = st_g32(h + 16);
        if (cmd == 1) {                                          // CMD_SUBMIT
            uint32_t len = st_g32(h + 24), np = st_g32(h + 32), iv = st_g32(h + 36);
            int isoc = np != 0xFFFFFFFFu && np > 0;
            uint64_t extra = (dir == 0 ? len : 0) + (isoc ? (uint64_t)np * 16 : 0);
            if (extra > (64u << 20) || seq & ST_SEQBIT) break;
            if (dir == 1 && ep > 0 && ep < 16 && iv > 0 && !isoc && len > 0 && len <= 4096) {   // interrupt IN: streamed
                pthread_mutex_lock(&st->mx);
                StEp *p = &st->ep[ep];
                if (!p->on || p->len != len) {
                    p->on = 1; p->len = len;
                    uint64_t unit = st->speed >= 3 ? 125000ull : 1000000ull;     // high speed counts microframes
                    uint64_t pace = (uint64_t)iv * unit * 3 / 4;
                    if (pace < 125000ull) pace = 125000ull;
                    if (pace > 16000000ull) pace = 16000000ull;
                    p->pace_ns = pace;
                }
                memcpy(p->sub, h, 48);
                p->wait_seq = seq; p->waiting = 1; p->stopped = 0;
                st_fill(st, (int)ep);
                st_deliver(st, (int)ep);
                pthread_cond_broadcast(&st->cv);
                pthread_mutex_unlock(&st->mx);
                continue;
            }
            pthread_mutex_lock(&st->mx);
            int full = st_fwd_add(st, seq, (uint8_t)(dir != 0));
            pthread_mutex_unlock(&st->mx);
            if (full) break;
            pthread_mutex_lock(&st->net_mx);
            int bad = st_send(st->net, h, 48) || st_copy(st->kern, st->net, extra, buf, 65536);
            pthread_mutex_unlock(&st->net_mx);
            if (bad) break;
        } else if (cmd == 2) {                                   // CMD_UNLINK
            uint32_t target = st_g32(h + 20);
            uint8_t cancel[ST_AHEAD * 2][48]; int nc = 0;
            pthread_mutex_lock(&st->mx);
            int mine = 0;
            for (int e = 1; e < 16; e++) {
                StEp *p = &st->ep[e];
                if (!p->on || !p->waiting || p->wait_seq != target) continue;
                mine = 1;
                p->waiting = 0; p->stopped = 1; p->epoch++;      // cancelled: drop what is queued and read ahead
                for (int i = 0; i < p->qn; i++) free(p->q[(p->qh + i) % ST_BUFMAX].data);
                p->qn = 0; p->qh = 0; p->next_ns = 0;
                for (int i = 0; i < 256 && nc < ST_AHEAD * 2; i++) {
                    StOut *o = &st->out[i];
                    if (!o->used || o->ep != e || !o->dir) continue;
                    StOut *c = st_out_new(st, (uint8_t)e, 0, p->epoch);   // cancel our read ahead
                    if (!c) continue;
                    memset(cancel[nc], 0, 48);
                    st_p32(cancel[nc], 2); st_p32(cancel[nc] + 4, c->seq); memcpy(cancel[nc] + 8, h + 8, 12); st_p32(cancel[nc] + 20, o->seq);
                    nc++;
                }
                p->ahead = 0;
                break;
            }
            if (!mine) for (int i = 0; i < 64; i++) if (!st->unl[i].used) { st->unl[i].used = 1; st->unl[i].seq = seq; st->unl[i].target = target; break; }
            pthread_mutex_unlock(&st->mx);
            for (int i = 0; i < nc; i++) st_net_write(st, cancel[i], NULL, 0);
            if (mine) {
                uint8_t r[48] = {0};
                st_p32(r, 4); st_p32(r + 4, seq); st_p32(r + 20, (uint32_t)-104);   // RET_UNLINK, -ECONNRESET
                pthread_mutex_lock(&st->kern_mx);
                int bad = st_send(st->kern, r, 48);
                pthread_mutex_unlock(&st->kern_mx);
                if (bad) break;
            } else st_net_write(st, h, NULL, 0);
        } else break;
    }
    free(buf);
    pthread_mutex_lock(&st->mx); st_kill(st); pthread_mutex_unlock(&st->mx);
    st_unref(st);
    return NULL;
}

// Other PC -> kernel.
static void *st_down(void *arg) {
    Stream *st = arg;
    uint8_t h[48], *buf = malloc(65536);
    while (buf && !st->dead && !st_recv(st->net, h, 48)) {
        uint32_t cmd = st_g32(h), seq = st_g32(h + 4);
        if (cmd == 3) {                                          // RET_SUBMIT
            uint32_t actual = st_g32(h + 24), np = st_g32(h + 32);
            int isoc = np != 0xFFFFFFFFu && np > 0 && np <= 1024;
            if (actual > (64u << 20)) break;
            if (seq & ST_SEQBIT) {                               // one of our reads ahead
                uint8_t *data = actual ? malloc(actual) : NULL;
                if (actual && (!data || st_recv(st->net, data, actual))) { free(data); break; }
                pthread_mutex_lock(&st->mx);
                StOut *o = st_out_find(st, seq);
                int keep = 0;
                if (o) {
                    StEp *p = &st->ep[o->ep];
                    if (o->epoch == p->epoch && p->on) {
                        if (p->ahead > 0) p->ahead--;
                        st->n_back++;
                        if (p->qn < ST_BUFMAX) {
                            StRep *r = &p->q[(p->qh + p->qn) % ST_BUFMAX];
                            memcpy(r->hdr, h, 48); r->len = actual; r->data = data; p->qn++; keep = 1;
                            if ((int32_t)st_g32(h + 20) != 0) { p->stopped = 1; st->n_stopped++; }   // an error: the kernel decides what next
                            if (p->qn > st->max_q) st->max_q = p->qn;
                        }
                        st_fill(st, o->ep);
                        st_deliver(st, o->ep);
                        pthread_cond_broadcast(&st->cv);
                    }
                    o->used = 0;
                }
                pthread_mutex_unlock(&st->mx);
                if (!keep) free(data);
                continue;
            }
            uint8_t dir = 1;
            pthread_mutex_lock(&st->mx);
            int known = !st_fwd_take(st, seq, &dir);
            pthread_mutex_unlock(&st->mx);
            if (!known) break;
            uint64_t extra = (dir ? actual : 0) + (isoc ? (uint64_t)np * 16 : 0);
            pthread_mutex_lock(&st->kern_mx);
            int bad = st_send(st->kern, h, 48) || st_copy(st->net, st->kern, extra, buf, 65536);
            pthread_mutex_unlock(&st->kern_mx);
            if (bad) break;
        } else if (cmd == 4) {                                   // RET_UNLINK
            if (seq & ST_SEQBIT) {                               // answer to one of our cancels
                pthread_mutex_lock(&st->mx);
                StOut *o = st_out_find(st, seq); if (o) o->used = 0;
                for (int i = 0; i < 256; i++) if (st->out[i].used && st->out[i].dir && st->out[i].epoch != st->ep[st->out[i].ep].epoch) st->out[i].used = 0;
                pthread_mutex_unlock(&st->mx);
                continue;
            }
            pthread_mutex_lock(&st->mx);
            for (int i = 0; i < 64; i++) if (st->unl[i].used && st->unl[i].seq == seq) {   // the cancelled request is done
                uint8_t d; st_fwd_take(st, st->unl[i].target, &d); st->unl[i].used = 0; break;
            }
            pthread_mutex_unlock(&st->mx);
            pthread_mutex_lock(&st->kern_mx);
            int bad = st_send(st->kern, h, 48);
            pthread_mutex_unlock(&st->kern_mx);
            if (bad) break;
        } else break;
    }
    free(buf);
    pthread_mutex_lock(&st->mx); st_kill(st); pthread_mutex_unlock(&st->mx);
    st_unref(st);
    return NULL;
}

// Start relaying between `net` (connected to the other PC, import done) and `kern` (our end of
// the socket pair whose other end was given to vhci). Takes ownership of both. 0 = ok.
static int stream_start(int net, int kern, uint32_t speed, const char *busid) {
    Stream *st = calloc(1, sizeof *st);
    if (!st) return -1;
    st->net = net; st->kern = kern; st->speed = speed; st->refs = 3; st->next_seq = 1;
    snprintf(st->busid, sizeof st->busid, "%s", busid);
    pthread_mutex_init(&st->mx, NULL); pthread_mutex_init(&st->net_mx, NULL); pthread_mutex_init(&st->kern_mx, NULL); pthread_cond_init(&st->cv, NULL);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int ok = !pthread_create(&t, &at, st_up, st);
    if (!ok) { pthread_attr_destroy(&at); close(net); close(kern); free(st); return -1; }
    if (pthread_create(&t, &at, st_down, st)) { pthread_mutex_lock(&st->mx); st_kill(st); st->refs--; pthread_mutex_unlock(&st->mx); }
    if (pthread_create(&t, &at, st_pacer, st)) { pthread_mutex_lock(&st->mx); st_kill(st); st->refs--; pthread_mutex_unlock(&st->mx); }
    pthread_attr_destroy(&at);
    return 0;
}
#endif
