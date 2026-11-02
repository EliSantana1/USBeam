// SPDX-License-Identifier: GPL-3.0-or-later
// Sleep test: a stand-in for systemd-logind on a private D-Bus checks that USBeam's sleep handling
// takes a sleep lock, lets go of devices when sleep is announced, releases the lock (so the PC
// can sleep), picks up again after waking and takes a new lock.
// Run by tests/sleep.sh (which starts the private bus).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
static volatile sig_atomic_t g_stop;
static volatile int slept, woke;
static void logm(const char *f, ...) { va_list a; va_start(a, f); printf("  usbeam: "); vprintf(f, a); va_end(a); printf("\n"); fflush(stdout); }
static void power_sleep(void) { slept++; }
static void power_wake(void) { woke++; }
#include "../src/linux/sleepwatch.h"

static int bus, fails;
static uint32_t ser = 100;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "OK" : "FAIL", what); if (!ok) fails++; }

static void send_msg(DbMsg *h, DbMsg *body, int fd_to_pass) {
    uint32_t flen = (uint32_t)(h->n - 16); memcpy(h->b + 12, &flen, 4);
    uint32_t blen = (uint32_t)body->n; memcpy(h->b + 4, &blen, 4);
    db_pad(h, 8); memcpy(h->b + h->n, body->b, body->n); h->n += body->n;
    struct iovec iov = { h->b, h->n };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {0}; mh.msg_iov = &iov; mh.msg_iovlen = 1;
    if (fd_to_pass >= 0) {
        mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &fd_to_pass, sizeof(int));
    }
    if (sendmsg(bus, &mh, MSG_NOSIGNAL) < 0) perror("sendmsg");
}
static void head(DbMsg *h, int type) { memset(h, 0, sizeof *h); h->b[0] = 'l'; h->b[1] = (uint8_t)type; h->b[3] = 1; h->n = 8; db_u32(h, ser++); db_u32(h, 0); }
static void emit_sleep(int start) {
    DbMsg h, b = {{0}, 0}; head(&h, 4);
    db_field(&h, 1, 'o', "/org/freedesktop/login1"); db_field(&h, 2, 's', "org.freedesktop.login1.Manager");
    db_field(&h, 3, 's', "PrepareForSleep"); db_field(&h, 8, 'g', "b");
    db_u32(&b, (uint32_t)start); send_msg(&h, &b, -1);
}
// Wait for the next Inhibit call (answering it with a lock) or for `secs`. Returns the lock's other end.
static int serve(int secs) {
    static uint8_t buf[65536]; static size_t have;
    int fds[4], nfds = 0;
    for (int t = 0; t < secs * 10; t++) {
        struct pollfd p = { bus, POLLIN, 0 };
        if (poll(&p, 1, 100) > 0) { ssize_t r = db_recv(bus, buf + have, sizeof buf - have, fds, &nfds); if (r > 0) have += (size_t)r; }
        while (have >= 16) {
            uint32_t blen, flen; memcpy(&blen, buf + 4, 4); memcpy(&flen, buf + 12, 4);
            size_t he = (16 + (size_t)flen + 7) & ~(size_t)7, total = he + blen;
            if (have < total) break;
            DbIn m; int got = db_parse(buf, total, he, &m) == 0 && m.type == 1 && !strcmp(m.member, "Inhibit");
            memmove(buf, buf + total, have - total); have -= total;
            if (got) {
                int pp[2]; if (pipe(pp)) return -1;
                DbMsg h, b = {{0}, 0}; head(&h, 2);
                db_field(&h, 6, 's', m.sender); db_field(&h, 8, 'g', "h");
                db_pad(&h, 8); h.b[h.n++] = 5; h.b[h.n++] = 1; h.b[h.n++] = 'u'; h.b[h.n++] = 0; db_u32(&h, m.serial);
                db_pad(&h, 8); h.b[h.n++] = 9; h.b[h.n++] = 1; h.b[h.n++] = 'u'; h.b[h.n++] = 0; db_u32(&h, 1);
                db_u32(&b, 0); send_msg(&h, &b, pp[1]); close(pp[1]);
                return pp[0];
            }
        }
    }
    return -1;
}
static int released(int lock, int secs) {          // the lock's other end sees end-of-file once USBeam closes it
    struct pollfd p = { lock, POLLIN, 0 };
    if (poll(&p, 1, secs * 1000) <= 0) return 0;
    char c; return read(lock, &c, 1) == 0;
}
static void *watch(void *a) { (void)a; return sleepwatch_main(NULL); }
int main(void) {
    bus = db_connect();
    if (bus < 0) { printf("FAIL: no private bus\n"); return 1; }
    const char *name[1] = { "org.freedesktop.login1" };
    db_call(bus, ser++, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", 0, NULL);
    {   // RequestName("org.freedesktop.login1", 0)
        DbMsg h, b = {{0}, 0}; head(&h, 1);
        db_field(&h, 1, 'o', "/org/freedesktop/DBus"); db_field(&h, 6, 's', "org.freedesktop.DBus");
        db_field(&h, 2, 's', "org.freedesktop.DBus"); db_field(&h, 3, 's', "RequestName"); db_field(&h, 8, 'g', "su");
        db_str(&b, name[0]); db_u32(&b, 0); send_msg(&h, &b, -1);
    }
    usleep(300000);
    pthread_t t; pthread_create(&t, NULL, watch, NULL);
    int lock = serve(5);
    check(lock >= 0, "takes a sleep lock when it starts");
    for (int round = 1; round <= 2 && lock >= 0; round++) {
        usleep(300000);
        emit_sleep(1);
        check(released(lock, 4), "releases the lock when sleep is announced");
        check(slept == round, "lets go of devices before sleeping");
        close(lock);
        emit_sleep(0);
        lock = serve(4);
        check(woke == round, "picks up again after waking");
        check(lock >= 0, "takes a new sleep lock after waking");
    }
    g_stop = 1; pthread_join(t, NULL);
    printf(fails ? "Sleep test FAILED\n" : "PASS: sleep test\n");
    return fails != 0;
}
