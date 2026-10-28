// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// client.h - the client side of USBeam, shared by Windows and Linux:
// remote PCs (one entry per PC, with all the addresses it is reachable at),
// network choice (Automatic / Home network / ZeroTier / Tailscale), polling,
// using/releasing devices, auto-reconnect, discovery messages, settings and
// the JSON for the app window.
//
// The including program provides these platform hooks first:
//   typedef ... plat_sock;  #define PLAT_BAD_SOCK ...
//   plat_sock plat_connect(const char *ip, int ms);        // connect to ip:3240 with timeout
//   int  plat_send(plat_sock s, const void *b, int n);     // 0 = ok
//   int  plat_recv(plat_sock s, void *b, int n);           // 0 = ok
//   void plat_close(plat_sock s);
//   void cl_lock(void); void cl_unlock(void);
//   unsigned long long now_ms(void);
//   int  plat_local_nets(LocalNet *out, int max);          // this PC's IPv4 networks + kind
//   void plat_client_tick_begin(void);                     // refresh attached-port info (once per tick)
//   int  plat_attach(const char *ip, const char *busid);   // returns local port, or -1
//   int  plat_port_alive(int port, const char *ip, const char *busid);
//   void plat_detach(int port);
//   const char *plat_client_note(void);                    // NULL, or why this PC can't use remote devices
//   void plat_tune(void);                                  // after attaching (Linux: drive speed)
//   void logm(const char *fmt, ...);
//   char g_id[33];                                         // this PC's USBeam id
//   char g_boot_id[64];                                    // changes on every real power-on

#ifndef USBEAM_CLIENT_H
#define USBEAM_CLIENT_H

#define OP_REQ_INFO 0x8FE1        // USBeam extension: device names and states as JSON
#define OP_REP_INFO 0x0FE1
#define CL_MAXR 16                // remote PCs
#define CL_MAXA 6                 // addresses per PC
#define CL_MAXRD 32               // devices per PC
#define CL_MAXW 32                // devices this PC uses

#include "client_types.h"
static const char *NET_KEY[] = { "auto", "home", "zerotier", "tailscale" };
static const char *NET_LABEL[] = { "Automatic", "Home network", "ZeroTier", "Tailscale" };
typedef struct { char busid[32], name[128], vid[8], pid[8], state[16], peer[64]; int countdown; } RDev;
typedef struct { char ip[64]; int kind, ok; } RAddr;
typedef struct {
    char id[40], name[64];
    RAddr addr[CL_MAXA]; int naddr;
    int manual;                 // added by address (kept in settings)
    int via;                    // index into addr[] in use, -1 = none
    unsigned long long next_poll;
    int ndev; RDev dev[CL_MAXRD];
} Remote;
typedef struct {
    int used;
    char pc[40], busid[32], ip[64];
    int port;                   // local port while attached, -1 otherwise
    int want;                   // user clicked Use (keep it attached)
    int autorc, release;
    unsigned long long next_try;
} Use;

static Remote g_r[CL_MAXR];
static Use g_use[CL_MAXW];
static int g_netpref = NET_AUTO;

void logm(const char *fmt, ...);

// ---------------------------------------------------------------- networks
static uint32_t ip_u32(const char *ip) {
    unsigned a, b, c, d;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return a << 24 | b << 16 | c << 8 | d;
}
// Which kind of network an address of another PC belongs to.
static int net_kind(const char *ip) {
    uint32_t a = ip_u32(ip);
    if ((a & 0xFFC00000u) == 0x64400000u) return NET_TS;          // 100.64.0.0/10: Tailscale
    LocalNet ln[16];
    int n = plat_local_nets(ln, 16);
    for (int i = 0; i < n; i++) if ((a & ln[i].mask) == ln[i].net) return ln[i].kind;
    return NET_HOME;
}

// ---------------------------------------------------------------- tables (call with cl_lock held)
static Remote *rm_find(const char *id) {
    for (int i = 0; i < CL_MAXR; i++) if (g_r[i].id[0] && !strcmp(g_r[i].id, id)) return &g_r[i];
    return NULL;
}
static Remote *rm_by_ip(const char *ip) {
    for (int i = 0; i < CL_MAXR; i++) if (g_r[i].id[0]) for (int k = 0; k < g_r[i].naddr; k++) if (!strcmp(g_r[i].addr[k].ip, ip)) return &g_r[i];
    return NULL;
}
static Remote *rm_new(const char *id) {
    for (int i = 0; i < CL_MAXR; i++) if (!g_r[i].id[0]) {
        memset(&g_r[i], 0, sizeof g_r[i]);
        snprintf(g_r[i].id, sizeof g_r[i].id, "%s", id);
        g_r[i].via = -1;
        return &g_r[i];
    }
    return NULL;
}
static void rm_add_addr(Remote *r, const char *ip) {
    for (int k = 0; k < r->naddr; k++) if (!strcmp(r->addr[k].ip, ip)) return;
    if (r->naddr >= CL_MAXA) return;
    RAddr *a = &r->addr[r->naddr++];
    memset(a, 0, sizeof *a);
    snprintf(a->ip, sizeof a->ip, "%s", ip);
    a->kind = net_kind(ip);
    r->next_poll = 0;
}
static Use *use_find(const char *pc, const char *busid) {
    for (int i = 0; i < CL_MAXW; i++) if (g_use[i].used && !strcmp(g_use[i].pc, pc) && !strcmp(g_use[i].busid, busid)) return &g_use[i];
    return NULL;
}
static Use *use_new(const char *pc, const char *busid) {
    Use *u = use_find(pc, busid);
    if (u) return u;
    for (int i = 0; i < CL_MAXW; i++) if (!g_use[i].used) {
        memset(&g_use[i], 0, sizeof g_use[i]);
        g_use[i].used = 1; g_use[i].port = -1;
        snprintf(g_use[i].pc, sizeof g_use[i].pc, "%s", pc);
        snprintf(g_use[i].busid, sizeof g_use[i].busid, "%s", busid);
        return &g_use[i];
    }
    return NULL;
}

// A PC was seen (discovery message or info reply) at `ip` with USBeam id `id`.
// Keeps exactly one entry per PC, merging an entry that was added by address.
static Remote *remote_seen(const char *id, const char *name, const char *ip) {
    if (!id || !id[0] || !strcmp(id, g_id)) return NULL;
    Remote *r = rm_find(id);
    Remote *byip = rm_by_ip(ip);
    if (!r && byip && !strncmp(byip->id, "ip:", 3)) {            // entry added by address: adopt the real id
        snprintf(byip->id, sizeof byip->id, "%s", id);
        r = byip;
    } else if (r && byip && byip != r && !strncmp(byip->id, "ip:", 3)) {
        for (int k = 0; k < byip->naddr; k++) rm_add_addr(r, byip->addr[k].ip);
        r->manual |= byip->manual;
        byip->id[0] = 0;
    }
    if (!r) {
        if (!(r = rm_new(id))) return NULL;
        logm("Found USBeam on %s (%s)", name && name[0] ? name : "?", ip);
    }
    if (name && name[0]) snprintf(r->name, sizeof r->name, "%s", name);
    rm_add_addr(r, ip);
    return r;
}

// Address to use for a PC under the current network choice, or -1.
static int rm_pick(Remote *r) {
    int best = -1, bestrank = 99;
    for (int k = 0; k < r->naddr; k++) {
        RAddr *a = &r->addr[k];
        if (!a->ok) continue;
        if (g_netpref != NET_AUTO && a->kind != g_netpref) continue;
        int rank = a->kind == NET_HOME ? 0 : a->kind == NET_ZT ? 1 : 2;   // Automatic: home, then ZeroTier, then Tailscale
        if (rank < bestrank) { best = k; bestrank = rank; }
    }
    return best;
}

// ---------------------------------------------------------------- polling another PC
typedef struct { char id[40], name[64]; int ndev; RDev dev[CL_MAXRD]; } PollResult;

static uint16_t cl_g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t cl_g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void cl_op(uint8_t *h, uint16_t code) { memset(h, 0, 8); h[0] = 0x01; h[1] = 0x11; h[2] = code >> 8; h[3] = code & 0xFF; }

static int poll_pc(const char *ip, PollResult *out) {
    memset(out, 0, sizeof *out);
    plat_sock s = plat_connect(ip, 1500);
    if (s == PLAT_BAD_SOCK) return -1;
    uint8_t h[12];
    cl_op(h, OP_REQ_INFO);
    if (!plat_send(s, h, 8) && !plat_recv(s, h, 8) && cl_g16(h + 2) == OP_REP_INFO && !plat_recv(s, h + 8, 4)) {
        uint32_t len = cl_g32(h + 8);
        if (len > (1u << 20)) { plat_close(s); return -1; }
        char *js = malloc(len + 1);
        if (plat_recv(s, js, (int)len)) { free(js); plat_close(s); return -1; }
        js[len] = 0;
        plat_close(s);
        json_str(js, "name", out->name, sizeof out->name);
        json_str(js, "id", out->id, sizeof out->id);
        const char *p = strstr(js, "\"devices\":[");
        while (p && out->ndev < CL_MAXRD && (p = strchr(p, '{'))) {
            const char *e = strchr(p, '}');
            if (!e) break;
            char obj[1024]; size_t n = (size_t)(e - p + 1); if (n >= sizeof obj) n = sizeof obj - 1;
            memcpy(obj, p, n); obj[n] = 0;
            RDev *d = &out->dev[out->ndev++];
            json_str(obj, "busid", d->busid, sizeof d->busid);
            json_str(obj, "name", d->name, sizeof d->name);
            json_str(obj, "vid", d->vid, sizeof d->vid);
            json_str(obj, "pid", d->pid, sizeof d->pid);
            json_str(obj, "state", d->state, sizeof d->state);
            json_str(obj, "peer", d->peer, sizeof d->peer);
            const char *c = json_find(obj, "countdown"); d->countdown = c ? atoi(c) : 0;
            p = e + 1;
        }
        free(js);
        return 0;
    }
    plat_close(s);
    // Not a USBeam PC: fall back to the standard USB/IP device list.
    s = plat_connect(ip, 1500);
    if (s == PLAT_BAD_SOCK) return -1;
    cl_op(h, 0x8005);
    if (plat_send(s, h, 8) || plat_recv(s, h, 12)) { plat_close(s); return -1; }
    uint32_t n = cl_g32(h + 8);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t d[312], ifc[4];
        if (plat_recv(s, d, 312)) break;
        for (int k = 0; k < d[311]; k++) plat_recv(s, ifc, 4);
        if (out->ndev >= CL_MAXRD) continue;
        RDev *r = &out->dev[out->ndev++];
        snprintf(r->busid, sizeof r->busid, "%.31s", (char *)d + 256);
        snprintf(r->vid, sizeof r->vid, "%04x", cl_g16(d + 300));
        snprintf(r->pid, sizeof r->pid, "%04x", cl_g16(d + 302));
        snprintf(r->state, sizeof r->state, "available");
    }
    plat_close(s);
    return 0;
}

// ---------------------------------------------------------------- the client loop (once a second)
static void client_save(void);

static void client_tick(void) {
    unsigned long long now = now_ms();
    // 1. Poll every address of every PC that is due.
    char ips[CL_MAXR * CL_MAXA][64]; int nip = 0;
    cl_lock();
    for (int i = 0; i < CL_MAXR; i++) {
        Remote *r = &g_r[i];
        if (!r->id[0] || now < r->next_poll) continue;
        r->next_poll = now + 2500;
        for (int k = 0; k < r->naddr; k++) snprintf(ips[nip++], 64, "%s", r->addr[k].ip);
    }
    cl_unlock();
    static PollResult pr;
    for (int i = 0; i < nip; i++) {
        int ok = poll_pc(ips[i], &pr) == 0;
        cl_lock();
        Remote *r = rm_by_ip(ips[i]);
        if (ok && pr.id[0]) r = remote_seen(pr.id, pr.name, ips[i]);
        if (r) {
            for (int k = 0; k < r->naddr; k++) if (!strcmp(r->addr[k].ip, ips[i])) r->addr[k].ok = ok;
            if (ok && !pr.id[0] && pr.name[0] == 0 && !r->name[0]) snprintf(r->name, sizeof r->name, "%s", ips[i]);
            r->via = rm_pick(r);
            if (ok && r->via >= 0 && !strcmp(r->addr[r->via].ip, ips[i])) { r->ndev = pr.ndev; memcpy(r->dev, pr.dev, sizeof pr.dev); }
            if (r->via < 0) r->ndev = 0;
        }
        cl_unlock();
    }
    // 2. Devices used here: keep attached, release, retry.
    int any = 0;
    cl_lock();
    for (int i = 0; i < CL_MAXW; i++) if (g_use[i].used) any = 1;
    cl_unlock();
    if (!any || plat_client_note()) return;
    plat_client_tick_begin();
    int changed = 0;
    for (int i = 0; i < CL_MAXW; i++) {
        cl_lock();
        Use u = g_use[i];
        cl_unlock();
        if (!u.used) continue;
        if (u.release) {
            if (u.port >= 0) plat_detach(u.port);
            logm("[%s] released", u.busid);
            cl_lock(); g_use[i].used = 0; cl_unlock();
            changed = 1;
            continue;
        }
        if (u.port >= 0) {
            if (plat_port_alive(u.port, u.ip, u.busid)) continue;
            logm("[%s] connection ended", u.busid);
            cl_lock();
            g_use[i].port = -1;
            if (!g_use[i].autorc) { g_use[i].used = 0; changed = 1; }
            cl_unlock();
            continue;
        }
        if (!u.want || now < u.next_try) continue;
        char ip[64] = "";
        cl_lock();
        Remote *r = rm_find(u.pc);
        if (r && r->via >= 0) snprintf(ip, sizeof ip, "%s", r->addr[r->via].ip);
        int listed = 0;
        if (r) for (int k = 0; k < r->ndev; k++) if (!strcmp(r->dev[k].busid, u.busid) && !strcmp(r->dev[k].state, "available")) listed = 1;
        cl_unlock();
        if (!ip[0] || !listed) { cl_lock(); g_use[i].next_try = now + 2000; cl_unlock(); continue; }
        int port = plat_attach(ip, u.busid);
        cl_lock();
        if (g_use[i].used && !strcmp(g_use[i].busid, u.busid) && !strcmp(g_use[i].pc, u.pc)) {
            g_use[i].port = port;
            snprintf(g_use[i].ip, sizeof g_use[i].ip, "%s", ip);
            if (port < 0) {
                g_use[i].next_try = now + 3000;
                if (!g_use[i].autorc) { g_use[i].used = 0; changed = 1; }    // a one-off Use that failed
            }
        }
        cl_unlock();
        if (port >= 0) { logm("[%s] attached from %s on port %d", u.busid, ip, port); plat_tune(); }
    }
    if (changed) client_save();
}

// Release everything (quitting).
static void client_release_all(void) {
    plat_client_tick_begin();
    for (int i = 0; i < CL_MAXW; i++) if (g_use[i].used && g_use[i].port >= 0) plat_detach(g_use[i].port);
}

// ---------------------------------------------------------------- discovery messages
static int disco_message(char *buf, int n) {
    return snprintf(buf, (size_t)n, "USBEAM2\t%s\t%s", g_id, g_name);
}
static void disco_received(const char *msg, const char *ip) {
    char id[40] = "", name[64] = "";
    if (strncmp(msg, "USBEAM2\t", 8) || sscanf(msg + 8, "%32[^\t]\t%63[^\r\n]", id, name) < 1) return;
    if (!strcmp(id, g_id)) return;
    cl_lock();
    remote_seen(id, name, ip);
    cl_unlock();
}

// ---------------------------------------------------------------- settings lines
// Auto-reconnect choices survive sleep, hibernate and USBeam restarts, but not a
// shutdown or restart of the PC: each is saved with the current boot id.
static void client_save_lines(FILE *f) {
    fprintf(f, "net %s\n", NET_KEY[g_netpref]);
    for (int i = 0; i < CL_MAXR; i++)
        if (g_r[i].id[0] && g_r[i].manual) for (int k = 0; k < g_r[i].naddr; k++) fprintf(f, "pc %s\n", g_r[i].addr[k].ip);
    for (int i = 0; i < CL_MAXW; i++)
        if (g_use[i].used && g_use[i].autorc) fprintf(f, "auto %s %s %s %d\n", g_boot_id, g_use[i].pc, g_use[i].busid, g_use[i].want);
}
static int client_load_line(const char *line) {
    char a[64], b[64], c[64];
    if (sscanf(line, "net %63s", a) == 1) { for (int i = 0; i < 4; i++) if (!strcmp(a, NET_KEY[i])) g_netpref = i; return 1; }
    if (sscanf(line, "pc %63s", a) == 1) {
        Remote *r = rm_by_ip(a);
        if (!r) { char id[40]; snprintf(id, sizeof id, "ip:%.36s", a); r = rm_new(id); }
        if (r) { rm_add_addr(r, a); r->manual = 1; }
        return 1;
    }
    int w = 0;
    if (sscanf(line, "auto %63s %63s %63s %d", a, b, c, &w) >= 3) {
        if (!strcmp(a, g_boot_id)) { Use *u = use_new(b, c); if (u) { u->autorc = 1; u->want = w; } }
        return 1;
    }
    return 0;
}
// The PC is shutting down or restarting: forget auto-reconnect choices.
__attribute__((unused)) static void client_forget_auto(void) {
    cl_lock();
    for (int i = 0; i < CL_MAXW; i++) if (g_use[i].used) g_use[i].autorc = 0;
    cl_unlock();
    client_save();
}

// ---------------------------------------------------------------- app window: JSON + actions
static void client_json(sb_t *j) {
    // networks this PC is on (for the "Connect over" list)
    int have[4] = { 1, 0, 0, 0 };
    LocalNet ln[16]; int nl = plat_local_nets(ln, 16);
    for (int i = 0; i < nl; i++) if (ln[i].kind >= 1 && ln[i].kind <= 3) have[ln[i].kind] = 1;
    sb_printf(j, "\"net\":\"%s\",\"nets\":[", NET_KEY[g_netpref]);
    int first = 1;
    for (int i = 0; i < 4; i++) if (have[i] || i == g_netpref) {
        if (!first) sb_add(j, ",", 1);
        first = 0;
        sb_printf(j, "{\"key\":\"%s\",\"label\":\"%s\"}", NET_KEY[i], NET_LABEL[i]);
    }
    sb_add(j, "]", 1);
    const char *note = plat_client_note();
    if (note) { sb_add(j, ",\"client_note\":", 15); sb_jstr(j, note); }
    sb_add(j, ",\"pcs\":[", 8);
    cl_lock();
    first = 1;
    for (int i = 0; i < CL_MAXR; i++) {
        Remote *r = &g_r[i];
        if (!r->id[0]) continue;
        int via = r->via;
        int has_mine = 0;
        for (int k = 0; k < CL_MAXW; k++) if (g_use[k].used && !strcmp(g_use[k].pc, r->id)) has_mine = 1;
        int on_net = g_netpref == NET_AUTO;
        for (int k = 0; k < r->naddr; k++) if (r->addr[k].kind == g_netpref) on_net = 1;
        if (via < 0 && !has_mine && !(r->manual && on_net)) continue;      // not reachable on the chosen network
        if (!first) sb_add(j, ",", 1);
        first = 0;
        sb_add(j, "{\"id\":", 6); sb_jstr(j, r->id);
        sb_add(j, ",\"name\":", 8); sb_jstr(j, r->name[0] ? r->name : (r->naddr ? r->addr[0].ip : "?"));
        sb_printf(j, ",\"online\":%s", via >= 0 ? "true" : "false");
        if (via >= 0) {
            char vt[128]; snprintf(vt, sizeof vt, "via %s · %s", NET_LABEL[r->addr[via].kind], r->addr[via].ip);
            sb_add(j, ",\"via\":", 7); sb_jstr(j, vt);
        } else sb_printf(j, ",\"via\":\"%s\"", g_netpref == NET_AUTO ? "not reachable" : "not reachable on this network");
        sb_add(j, ",\"devices\":[", 12);
        int f2 = 1;
        for (int k = 0; k < r->ndev; k++) {
            RDev *d = &r->dev[k];
            Use *u = use_find(r->id, d->busid);
            const char *st = d->state;
            if (u && u->port >= 0) st = "mine";
            else if (u && u->want && !u->release && !strcmp(d->state, "available")) st = "connecting";
            else if (u && u->want && u->autorc && strcmp(d->state, "in_use")) st = !strcmp(d->state, "countdown") ? "countdown" : "waiting";
            else if (!strcmp(d->state, "in_use")) st = "busy";
            if (!f2) sb_add(j, ",", 1);
            f2 = 0;
            sb_add(j, "{\"busid\":", 9); sb_jstr(j, d->busid);
            sb_add(j, ",\"name\":", 8); sb_jstr(j, d->name);
            sb_add(j, ",\"vid\":", 7); sb_jstr(j, d->vid);
            sb_add(j, ",\"pid\":", 7); sb_jstr(j, d->pid);
            sb_printf(j, ",\"state\":\"%s\",\"countdown\":%d,\"auto\":%s,\"peer\":", st, d->countdown, (u && u->autorc) ? "true" : "false");
            sb_jstr(j, d->peer);
            sb_add(j, "}", 1);
        }
        for (int k = 0; k < CL_MAXW; k++) {     // devices in use / waiting that the PC doesn't list right now
            Use *u = &g_use[k];
            if (!u->used || !u->want || strcmp(u->pc, r->id)) continue;
            int listed = 0;
            for (int m = 0; m < r->ndev; m++) if (!strcmp(r->dev[m].busid, u->busid)) listed = 1;
            if (listed) continue;
            if (!f2) sb_add(j, ",", 1);
            f2 = 0;
            sb_add(j, "{\"busid\":", 9); sb_jstr(j, u->busid);
            sb_printf(j, ",\"name\":\"\",\"vid\":\"\",\"pid\":\"\",\"state\":\"%s\",\"countdown\":0,\"auto\":%s,\"peer\":\"\"}",
                      u->port >= 0 ? "mine" : "waiting", u->autorc ? "true" : "false");
        }
        sb_add(j, "]}", 2);
    }
    cl_unlock();
    sb_add(j, "]", 1);
}

// Returns 1 if it handled the action (writes any error into out and sets *code).
static int client_action(const char *op, const char *body, sb_t *out, int *code) {
    char pc[64], busid[64], host[64], val[32];
    json_str(body, "pc", pc, sizeof pc);
    json_str(body, "busid", busid, sizeof busid);
    json_str(body, "host", host, sizeof host);
    json_str(body, "value", val, sizeof val);
    if (!strcmp(op, "set_net")) {
        for (int i = 0; i < 4; i++) if (!strcmp(val, NET_KEY[i])) g_netpref = i;
        cl_lock();
        for (int i = 0; i < CL_MAXR; i++) if (g_r[i].id[0]) { g_r[i].via = rm_pick(&g_r[i]); if (g_r[i].via < 0) g_r[i].ndev = 0; g_r[i].next_poll = 0; }
        cl_unlock();
        client_save();
        return 1;
    }
    if (!strcmp(op, "add_host")) {
        if (!ip_u32(host)) { sb_printf(out, "%s", "{\"error\":\"Please enter an IP address, for example 192.168.0.20\"}"); *code = 400; return 1; }
        cl_lock();
        Remote *r = rm_by_ip(host);
        if (!r) { char id[40]; snprintf(id, sizeof id, "ip:%.36s", host); r = rm_new(id); }
        if (r) { rm_add_addr(r, host); r->manual = 1; }
        cl_unlock();
        client_save();
        return 1;
    }
    if (!strcmp(op, "use") || !strcmp(op, "release") || !strcmp(op, "auto")) {
        const char *note = plat_client_note();
        if (note && strcmp(op, "release")) { sb_add(out, "{\"error\":", 9); sb_jstr(out, note); sb_add(out, "}", 1); *code = 400; return 1; }
        int on = json_bool(body, "on");
        cl_lock();
        Use *u = (!strcmp(op, "use") || (!strcmp(op, "auto") && on)) ? use_new(pc, busid) : use_find(pc, busid);
        if (u) {
            if (!strcmp(op, "release")) u->release = 1;
            else if (!strcmp(op, "auto")) { u->autorc = on; if (!on && !u->want) u->used = 0; }
            else { u->want = 1; u->next_try = 0; u->release = 0; }
        }
        cl_unlock();
        client_save();
        return 1;
    }
    return 0;
}

#endif
