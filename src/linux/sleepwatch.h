// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// sleepwatch.h - get ready before Linux sleeps or hibernates, and pick up again after waking.
//
// The standard way (systemd-logind's "delay" inhibitor): USBeam holds a lock that makes logind
// wait (a few seconds at most) before sleeping. logind then announces PrepareForSleep(true);
// USBeam lets go of devices and releases the lock, and the PC sleeps. After waking logind
// announces PrepareForSleep(false) and USBeam takes a new lock.
// Spoken over the system D-Bus directly (a few small messages), so USBeam needs no extra library.
// Without logind or D-Bus nothing changes: a dropped connection is still noticed within ~20 s.
//
// The including program provides: void power_sleep(void); void power_wake(void); g_stop; logm().

#include <sys/un.h>
#include <sys/uio.h>

typedef struct { uint8_t b[1024]; size_t n; } DbMsg;
static void db_pad(DbMsg *m, size_t a) { while (m->n % a) m->b[m->n++] = 0; }
static void db_u32(DbMsg *m, uint32_t v) { db_pad(m, 4); memcpy(m->b + m->n, &v, 4); m->n += 4; }
static void db_str(DbMsg *m, const char *s) {           // 's' / 'o'
    size_t l = strlen(s);
    db_u32(m, (uint32_t)l); memcpy(m->b + m->n, s, l + 1); m->n += l + 1;
}
static void db_field(DbMsg *m, uint8_t code, char type, const char *v) {
    db_pad(m, 8);
    m->b[m->n++] = code; m->b[m->n++] = 1; m->b[m->n++] = (uint8_t)type; m->b[m->n++] = 0;
    if (type == 'g') { size_t l = strlen(v); m->b[m->n++] = (uint8_t)l; memcpy(m->b + m->n, v, l + 1); m->n += l + 1; }
    else db_str(m, v);
}

// A method call with up to 4 string arguments. Returns 0 if sent.
static int db_call(int fd, uint32_t serial, const char *dest, const char *path, const char *iface,
                   const char *member, int nargs, const char **args) {
    DbMsg h = {{0}, 0}, body = {{0}, 0};
    char sig[8] = "";
    for (int i = 0; i < nargs; i++) { db_str(&body, args[i]); sig[i] = 's'; }
    h.b[0] = 'l'; h.b[1] = 1; h.b[2] = 0; h.b[3] = 1; h.n = 4;
    db_u32(&h, (uint32_t)body.n); db_u32(&h, serial);
    db_u32(&h, 0);                                       // header fields length, filled in below
    size_t start = h.n;
    db_field(&h, 1, 'o', path);
    if (dest) db_field(&h, 6, 's', dest);
    db_field(&h, 2, 's', iface);
    db_field(&h, 3, 's', member);
    if (nargs) db_field(&h, 8, 'g', sig);
    uint32_t flen = (uint32_t)(h.n - start); memcpy(h.b + 12, &flen, 4);
    db_pad(&h, 8);
    if (h.n + body.n > sizeof h.b) return -1;
    memcpy(h.b + h.n, body.b, body.n); h.n += body.n;
    return send(fd, h.b, h.n, MSG_NOSIGNAL) == (ssize_t)h.n ? 0 : -1;
}

// One received message, the parts USBeam looks at.
typedef struct { int type; uint32_t serial, reply_to, nfds; char member[64], iface[96], sender[64]; int boolarg; } DbIn;
static int db_parse(const uint8_t *b, size_t total, size_t hdr_end, DbIn *o) {
    memset(o, 0, sizeof *o); o->boolarg = -1;
    if (b[0] != 'l') return -1;                          // only little-endian messages are expected here
    o->type = b[1];
    memcpy(&o->serial, b + 8, 4);
    uint32_t flen; memcpy(&flen, b + 12, 4);
    size_t p = 16, end = 16 + flen;
    while (p < end) {
        p = (p + 7) & ~(size_t)7;
        if (p + 4 > end) break;
        uint8_t code = b[p++], sl = b[p++];
        char t = (char)b[p]; p += sl + 1;                // the variant's one-letter signature
        if (t == 's' || t == 'o') {
            p = (p + 3) & ~(size_t)3;
            uint32_t l; memcpy(&l, b + p, 4); p += 4;
            if (p + l + 1 > end) return -1;
            if (code == 3) snprintf(o->member, sizeof o->member, "%.*s", (int)l, b + p);
            if (code == 2) snprintf(o->iface, sizeof o->iface, "%.*s", (int)l, b + p);
            if (code == 7) snprintf(o->sender, sizeof o->sender, "%.*s", (int)l, b + p);
            p += l + 1;
        } else if (t == 'g') { p += b[p] + 2; }
        else if (t == 'u') { p = (p + 3) & ~(size_t)3; uint32_t v; memcpy(&v, b + p, 4); p += 4; if (code == 5) o->reply_to = v; if (code == 9) o->nfds = v; }
        else return -1;
    }
    if (hdr_end + 4 <= total) { uint32_t v; memcpy(&v, b + hdr_end, 4); o->boolarg = v ? 1 : 0; }   // a 'b' or 'h' argument
    return 0;
}

// Read whatever arrived; fds sent along (the inhibitor lock) are collected in *fds.
static ssize_t db_recv(int fd, uint8_t *buf, size_t n, int *fds, int *nfds) {
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = { buf, n };
    struct msghdr mh = {0};
    mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    ssize_t r = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); r > 0 && c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k; i++) {
                int x; memcpy(&x, CMSG_DATA(c) + i * sizeof(int), sizeof x);
                if (*nfds < 4) fds[(*nfds)++] = x; else close(x);
            }
        }
    return r;
}

static int db_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    const char *path = getenv("USBEAM_TEST") ? getenv("USBEAM_TEST_DBUS") : NULL;   // tests: a private bus
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path ? path : "/run/dbus/system_bus_socket");
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    // Sign in as this process's user, and ask for file descriptors to be passed (the lock is one).
    char uid[16], hex[40] = "", line[256];
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    for (char *c = uid; *c; c++) snprintf(hex + strlen(hex), 3, "%02x", (unsigned char)*c);
    snprintf(line, sizeof line, "AUTH EXTERNAL %s\r\n", hex);
    if (send(fd, "", 1, MSG_NOSIGNAL) != 1 || send(fd, line, strlen(line), MSG_NOSIGNAL) < 0) { close(fd); return -1; }
    ssize_t r = recv(fd, line, sizeof line - 1, 0);
    if (r <= 0 || strncmp(line, "OK ", 3)) { close(fd); return -1; }
    send(fd, "NEGOTIATE_UNIX_FD\r\n", 19, MSG_NOSIGNAL);
    r = recv(fd, line, sizeof line - 1, 0);
    if (r <= 0 || strncmp(line, "AGREE_UNIX_FD", 13)) { close(fd); return -1; }
    if (send(fd, "BEGIN\r\n", 7, MSG_NOSIGNAL) != 7) { close(fd); return -1; }
    tv.tv_sec = 1;                                       // from now on: wake up every second to check g_stop
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static void *sleepwatch_main(void *arg) {
    (void)arg;
    static uint8_t buf[65536];
    int warned = 0, asleep = 0;
    while (!g_stop) {
        int fd = db_connect();
        if (fd < 0) {
            if (!warned++) logm("Sleep handling: system D-Bus not reachable (devices are still released ~20 s after a connection drops)");
            for (int k = 0; k < 30 && !g_stop; k++) sleep(1);
            continue;
        }
        uint32_t serial = 1, inhibit_serial = 0;
        int lock = -1;
        static const char *LOGIN = "org.freedesktop.login1", *LPATH = "/org/freedesktop/login1", *LMGR = "org.freedesktop.login1.Manager";
        const char *match = "type='signal',sender='org.freedesktop.login1',path='/org/freedesktop/login1',"
                            "interface='org.freedesktop.login1.Manager',member='PrepareForSleep'";
        const char *inh[4] = { "sleep", "USBeam", "Releasing USB devices used from or by other PCs", "delay" };
        int ok = db_call(fd, serial++, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", 0, NULL) == 0 &&
                 db_call(fd, serial++, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", 1, &match) == 0;
        inhibit_serial = serial;
        ok = ok && db_call(fd, serial++, LOGIN, LPATH, LMGR, "Inhibit", 4, inh) == 0;
        size_t have = 0;
        int fds[4], nfds = 0;
        while (ok && !g_stop) {
            ssize_t r = db_recv(fd, buf + have, sizeof buf - have, fds, &nfds);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            if (r > 0) have += (size_t)r;
            for (;;) {                                   // every complete message in the buffer
                if (have < 16) break;
                uint32_t blen, flen; memcpy(&blen, buf + 4, 4); memcpy(&flen, buf + 12, 4);
                size_t hdr_end = (16 + (size_t)flen + 7) & ~(size_t)7, total = hdr_end + blen;
                if (total > sizeof buf) { ok = 0; break; }
                if (have < total) break;
                DbIn m;
                int mine = -1;                           // the fds sent with this message arrive in order
                int parsed = db_parse(buf, total, hdr_end, &m) == 0;
                for (uint32_t k = 0; parsed && k < m.nfds && nfds > 0; k++) {
                    int x = fds[0]; memmove(fds, fds + 1, sizeof(int) * (size_t)--nfds);
                    if (mine < 0) mine = x; else close(x);
                }
                if (parsed) {
                    if ((m.type == 2 || m.type == 3) && m.reply_to == inhibit_serial) {
                        if (m.type == 2 && mine >= 0) {
                            if (lock >= 0) close(lock);
                            lock = mine; mine = -1;
                            if (warned) logm("Sleep handling: ready");
                            warned = 0;
                        } else if (!warned++) logm("Sleep handling: logind refused the sleep lock");
                    } else if (m.type == 4 && !strcmp(m.member, "PrepareForSleep") && !strcmp(m.iface, LMGR)) {
                        if (m.boolarg == 1) {
                            power_sleep(); asleep = 1;   // let go of devices, then let the PC sleep
                            if (lock >= 0) { close(lock); lock = -1; }
                        } else if (m.boolarg == 0) {
                            power_wake(); asleep = 0;
                            inhibit_serial = serial;
                            if (db_call(fd, serial++, LOGIN, LPATH, LMGR, "Inhibit", 4, inh) != 0) ok = 0;
                        }
                    }
                }
                if (mine >= 0) close(mine);              // fds that came with anything else
                memmove(buf, buf + total, have - total); have -= total;
            }
        }
        if (lock >= 0) close(lock);
        close(fd);
        if (asleep) { power_wake(); asleep = 0; }       // lost the bus while asleep: don't stay asleep
        for (int k = 0; k < 5 && !g_stop; k++) sleep(1);  // D-Bus restarted? connect again
    }
    return NULL;
}
