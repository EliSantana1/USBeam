// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// pair.h - pairing two PCs with a 6-digit code, and the requests only paired PCs can make
// (Give to, permission to use a device, unpair). Shared by Windows and Linux; included after client.h.
//
// How it works:
//   - Pairing is only possible while "Pair a PC" is open on a PC (2 minutes at most).
//   - One PC (the one being paired with) shows a random 6-digit code; the other types it.
//   - The code is turned into a shared key with CPace, a password-authenticated key exchange
//     selected by the IRTF crypto forum (CFRG). Someone listening on the network learns nothing
//     about the code or the key. Someone pretending to be one of the PCs gets one guess per try,
//     out of a million codes; after 3 tries the pairing ends and must be started again by hand.
//   - Each pair of PCs keeps one 32-byte key, in a file only the system can read.
//   - Later requests (Give to, permission to use a device, unpair) prove they come from a paired
//     PC by answering a random challenge with that key (BLAKE2b). Replayed answers don't work.
//   Crypto: Monocypher 4 (src/common/monocypher), X25519 + Elligator 2 + BLAKE2b.
//
// Platform hooks (defined before including):
//   void plat_random(void *buf, size_t n);               // cryptographically secure random bytes
//   void plat_sock_timeout(plat_sock s, int ms);         // send and receive time limit
//   int  plat_secret_write(const char *data, size_t n);  // replace the pairing file (only the system can read it); 0 = ok
//   int  plat_secret_read(char *buf, size_t max);        // read the pairing file: length, or -1
//   void pr_lock(void); void pr_unlock(void);            // pairing state. Never held while taking cl_lock or using the network.

#ifndef USBEAM_PAIR_H
#define USBEAM_PAIR_H

#include "monocypher/monocypher.h"
#include "monocypher/monocypher.c"

#define OP_REQ_PAIR 0x8FE4        // USBeam extension: pairing steps
#define OP_REP_PAIR 0x0FE4
#define OP_REQ_AUTH 0x8FE5        // USBeam extension: a request from a paired PC (challenge + answer)
#define OP_REP_AUTH 0x0FE5
#define PR_MAX 16                 // paired PCs
#define PR_OPEN_MS 120000         // how long "Pair a PC" accepts pairing
#define PR_TRIES 3                // code tries per pairing
#define PR_PERMIT_MS 20000        // how long a paired PC's request opens the door (Only paired PCs)

enum { PRC_GIVE = 1, PRC_PERMIT = 2, PRC_UNPAIR = 3, PRC_PING = 4 };

typedef struct { char id[40], name[64]; uint8_t key[32]; long long when; } PairRec;
static PairRec g_pr[PR_MAX];

// The pairing in progress. "b" = this PC shows the code, "a" = this PC types it.
static struct {
    unsigned long long open_until;          // "Pair a PC" is open here until then
    int b_active, b_tries, b_have;
    char b_peer_id[40], b_peer_name[64], b_code[8];
    uint8_t b_sid[16], b_Ya[32], b_Yb[32], b_isk[64];
    int a_active, a_tries;
    char a_peer_id[40], a_peer_name[64], a_ip[64];
    uint8_t a_sid[16];
    char msg[200]; int msg_ok;              // result shown in the window
} g_pm;

static struct { char ip[64], busid[32]; unsigned long long until; } g_permit[16];
static int g_permit_next;

// ---------------------------------------------------------------- small helpers
static void pr_fixed(uint8_t *dst, const char *s, size_t n) {   // zero-padded field
    memset(dst, 0, n);
    size_t l = strlen(s); if (l > n - 1) l = n - 1;
    memcpy(dst, s, l);
}
static void pr_unfix(char *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n - 1); dst[n - 1] = 0;
    for (char *c = dst; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';   // no tabs, newlines or control characters
}
static void pr_h_str(crypto_blake2b_ctx *c, const char *s) {    // length-prefixed: no ambiguity between fields
    size_t l = strlen(s); if (l > 255) l = 255;
    uint8_t b = (uint8_t)l;
    crypto_blake2b_update(c, &b, 1);
    crypto_blake2b_update(c, (const uint8_t *)s, l);
}
static void pr_reply(plat_sock s, uint16_t code, const void *p, uint32_t n) {
    uint8_t h[8];
    cl_op(h, code);
    h[4] = (uint8_t)(n >> 24); h[5] = (uint8_t)(n >> 16); h[6] = (uint8_t)(n >> 8); h[7] = (uint8_t)n;
    if (!plat_send(s, h, 8) && n) plat_send(s, p, (int)n);
}
// Sends a request and reads the reply on the same connection. 0 = ok.
static int pr_exchange(plat_sock s, uint16_t code, const void *p, uint32_t n, uint16_t want, uint8_t *rep, uint32_t max, uint32_t *got) {
    pr_reply(s, code, p, n);
    uint8_t h[8];
    if (plat_recv(s, h, 8) || cl_g16(h + 2) != want) return -1;
    uint32_t len = cl_g32(h + 4);
    if (len == 0 || len > max || plat_recv(s, rep, (int)len)) return -1;
    *got = len;
    return 0;
}
static PairRec *pr_find(const char *id) {                // call with pr_lock held
    for (int i = 0; i < PR_MAX; i++) if (g_pr[i].id[0] && !strcmp(g_pr[i].id, id)) return &g_pr[i];
    return NULL;
}
static int pair_is_paired(const char *id) {
    pr_lock();
    int r = id && id[0] && pr_find(id) != NULL;
    pr_unlock();
    return r;
}

// A random 6-digit code, never an easy one (all the same digit, only two different digits,
// or a straight run like 123456 / 987654).
static void pr_make_code(char out[8]) {
    for (;;) {
        uint32_t r;
        plat_random(&r, sizeof r);
        if (r >= 4294000000u) continue;           // keeps every code equally likely
        snprintf(out, 8, "%06u", (unsigned)(r % 1000000u));
        int seen[10] = {0}, distinct = 0, up = 1, down = 1;
        for (int i = 0; i < 6; i++) {
            int d = out[i] - '0';
            if (!seen[d]++) distinct++;
            if (i && d != out[i - 1] - '0' + 1) up = 0;
            if (i && d != out[i - 1] - '0' - 1) down = 0;
        }
        if (distinct > 2 && !up && !down) return;
    }
}

// ---------------------------------------------------------------- CPace (X25519 + Elligator 2 + BLAKE2b)
// The generator point comes from the code and the session; each side picks a secret y, sends y*G,
// and both get y_a*y_b*G. Without the code, the other side's message is useless.
static void pr_cpace_gen(uint8_t G[32], const char *code, const uint8_t sid[16], const char *ida, const char *idb) {
    crypto_blake2b_ctx c;
    uint8_t h[64];
    crypto_blake2b_init(&c, 64);
    pr_h_str(&c, "USBeam CPace v1 generator");
    pr_h_str(&c, code);
    crypto_blake2b_update(&c, sid, 16);
    pr_h_str(&c, ida);
    pr_h_str(&c, idb);
    crypto_blake2b_final(&c, h);
    crypto_elligator_map(G, h);
    crypto_wipe(h, sizeof h);
}
static void pr_cpace_share(uint8_t Y[32], uint8_t y[32], const uint8_t G[32]) {
    plat_random(y, 32);
    crypto_x25519(Y, y, G);
}
static int pr_cpace_isk(uint8_t isk[64], const uint8_t y[32], const uint8_t Yother[32], const uint8_t Ya[32], const uint8_t Yb[32], const uint8_t sid[16]) {
    uint8_t K[32], zero[32] = {0};
    crypto_x25519(K, y, Yother);
    int bad = crypto_verify32(K, zero) == 0;      // a low-order point from the other side: refuse
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 64);
    pr_h_str(&c, "USBeam CPace v1 key");
    crypto_blake2b_update(&c, sid, 16);
    crypto_blake2b_update(&c, K, 32);
    crypto_blake2b_update(&c, Ya, 32);
    crypto_blake2b_update(&c, Yb, 32);
    crypto_blake2b_final(&c, isk);
    crypto_wipe(K, sizeof K);
    return bad ? -1 : 0;
}
static void pr_tag(uint8_t out[32], const uint8_t isk[64], char who, const uint8_t Ya[32], const uint8_t Yb[32]) {
    uint8_t m[65];
    m[0] = (uint8_t)who; memcpy(m + 1, Ya, 32); memcpy(m + 33, Yb, 32);
    crypto_blake2b_keyed(out, 32, isk, 32, m, sizeof m);
}
static void pr_pairkey(uint8_t out[32], const uint8_t isk[64], const char *ida, const char *idb) {
    crypto_blake2b_ctx c;
    crypto_blake2b_keyed_init(&c, 32, isk + 32, 32);
    pr_h_str(&c, "USBeam pair key v1");
    pr_h_str(&c, ida);
    pr_h_str(&c, idb);
    crypto_blake2b_final(&c, out);
}
// The answer to a challenge: proves the request comes from a PC holding the pair key.
static void pr_mac(uint8_t out[32], const uint8_t key[32], const uint8_t ch[16], const char *idc, const char *ids, int cmd, const char *arg) {
    crypto_blake2b_ctx c;
    uint8_t b = (uint8_t)cmd;
    crypto_blake2b_keyed_init(&c, 32, key, 32);
    pr_h_str(&c, "USBeam auth v1");
    crypto_blake2b_update(&c, ch, 16);
    pr_h_str(&c, idc);
    pr_h_str(&c, ids);
    crypto_blake2b_update(&c, &b, 1);
    pr_h_str(&c, arg);
    crypto_blake2b_final(&c, out);
}

// ---------------------------------------------------------------- saved pairings
static void pair_save(void) {
    sb_t b = {0};
    pr_lock();
    for (int i = 0; i < PR_MAX; i++) {
        PairRec *p = &g_pr[i];
        if (!p->id[0]) continue;
        char hex[65];
        for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", p->key[k]);
        sb_printf(&b, "pair\t%s\t%s\t%lld\t%s\n", p->id, hex, p->when, p->name);
        crypto_wipe(hex, sizeof hex);
    }
    pr_unlock();
    if (plat_secret_write(b.p ? b.p : "", b.len)) logm("Could not save the list of paired PCs");
    if (b.p) crypto_wipe(b.p, b.len);
    sb_free(&b);
}
static int pr_hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static void pair_load(void) {
    static char buf[16384];
    int n = plat_secret_read(buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    pr_lock();
    int k = 0;
    for (char *line = buf; *line && k < PR_MAX; ) {
        char *end = strchr(line, '\n'); if (end) *end = 0;
        char *f[5] = {0}; int nf = 0;
        for (char *p = line; p && nf < 5; ) { f[nf++] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        if (nf == 5 && !strcmp(f[0], "pair") && strlen(f[2]) == 64 && f[1][0]) {
            PairRec *r = &g_pr[k];
            int ok = 1;
            for (int i = 0; i < 32; i++) {
                int hi = pr_hexval(f[2][2 * i]), lo = pr_hexval(f[2][2 * i + 1]);
                if (hi < 0 || lo < 0) ok = 0; else r->key[i] = (uint8_t)(hi << 4 | lo);
            }
            if (ok) {
                snprintf(r->id, sizeof r->id, "%s", f[1]);
                snprintf(r->name, sizeof r->name, "%s", f[4]);
                r->when = atoll(f[3]);
                k++;
            } else crypto_wipe(r, sizeof *r);
        }
        if (!end) break;
        line = end + 1;
    }
    pr_unlock();
    crypto_wipe(buf, sizeof buf);
}
static int pr_store(const char *id, const char *name, const uint8_t key[32]) {   // call with pr_lock held; 0 = ok
    PairRec *r = pr_find(id);
    for (int i = 0; !r && i < PR_MAX; i++) if (!g_pr[i].id[0]) r = &g_pr[i];
    if (!r) return -1;
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->name, sizeof r->name, "%s", name[0] ? name : id);
    memcpy(r->key, key, 32);
    r->when = (long long)time(NULL);
    return 0;
}
static void pr_end_b(void) {           // call with pr_lock held
    crypto_wipe(g_pm.b_isk, sizeof g_pm.b_isk);
    crypto_wipe(g_pm.b_code, sizeof g_pm.b_code);
    g_pm.b_active = g_pm.b_have = g_pm.b_tries = 0;
}
static void pr_end_a(void) { g_pm.a_active = g_pm.a_tries = 0; }
static void pr_msg(int ok, const char *fmt, ...) {      // call with pr_lock held
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_pm.msg, sizeof g_pm.msg, fmt, ap);
    va_end(ap);
    g_pm.msg_ok = ok;
}

// ---------------------------------------------------------------- for other parts of USBeam
static void pair_info_json(sb_t *j) {          // added to this PC's info for other PCs
    pr_lock();
    int open = now_ms() < g_pm.open_until;
    pr_unlock();
    sb_printf(j, ",\"pair\":true,\"pair_open\":%s,\"paired_only\":%s", open ? "true" : "false", g_paired_only ? "true" : "false");
}
static void pr_permit(const char *ip, const char *busid) {
    pr_lock();
    int k = -1;
    for (int i = 0; i < 16; i++) if (!strcmp(g_permit[i].ip, ip) && !strcmp(g_permit[i].busid, busid)) k = i;
    if (k < 0) k = g_permit_next++ % 16;
    snprintf(g_permit[k].ip, sizeof g_permit[k].ip, "%s", ip);
    snprintf(g_permit[k].busid, sizeof g_permit[k].busid, "%.31s", busid);
    g_permit[k].until = now_ms() + PR_PERMIT_MS;
    pr_unlock();
}
static int pr_permitted(const char *ip, const char *busid) {
    pr_lock();
    unsigned long long now = now_ms();
    int ok = 0;
    for (int i = 0; i < 16; i++)
        if (g_permit[i].until > now && !strcmp(g_permit[i].ip, ip) && (!busid || !strcmp(g_permit[i].busid, busid))) ok = 1;
    pr_unlock();
    return ok;
}
// "Only paired PCs can use my devices": a device may be connected only right after a paired PC asked for it.
static int pair_import_ok(const char *ip, const char *busid) { return !g_paired_only || pr_permitted(ip, busid); }
static int pair_kick_ok(const char *ip) { return !g_paired_only || pr_permitted(ip, NULL); }
static void pair_reset(void) {                 // part of "Reset USBeam"
    pr_lock();
    crypto_wipe(g_pr, sizeof g_pr);
    pr_end_b(); pr_end_a();
    g_pm.open_until = 0; g_pm.msg[0] = 0;
    memset(g_permit, 0, sizeof g_permit);
    pr_unlock();
    g_paired_only = 0;
    pair_save();
}

// ---------------------------------------------------------------- answering other PCs
static void pr_srv_pair(plat_sock s, const char *peer, const uint8_t *m, uint32_t n) {
    unsigned long long now = now_ms();
    char logline[220] = "";
    int save = 0;
    if (m[0] == 1 && n >= 1 + 40 + 64) {                       // START: idA, nameA
        uint8_t r[1 + 16 + 40 + 64] = {0};
        char id[40], name[64];
        pr_unfix(id, m + 1, 40); pr_unfix(name, m + 41, 64);
        pr_lock();
        if (now >= g_pm.open_until) r[0] = 1;                  // Pair a PC isn't open here
        else if (!id[0] || !strcmp(id, g_id)) r[0] = 3;
        else if (g_pm.a_active || (g_pm.b_active && strcmp(g_pm.b_peer_id, id))) r[0] = 2;   // busy with another PC
        else {
            pr_end_b();
            g_pm.b_active = 1;
            snprintf(g_pm.b_peer_id, sizeof g_pm.b_peer_id, "%s", id);
            snprintf(g_pm.b_peer_name, sizeof g_pm.b_peer_name, "%s", name[0] ? name : peer);
            plat_random(g_pm.b_sid, 16);
            pr_make_code(g_pm.b_code);
            g_pm.msg[0] = 0;
            memcpy(r + 1, g_pm.b_sid, 16);
            pr_fixed(r + 17, g_id, 40);
            pr_fixed(r + 57, g_name, 64);
            snprintf(logline, sizeof logline, "%s (%s) asked to pair with this PC", g_pm.b_peer_name, peer);
        }
        pr_unlock();
        pr_reply(s, OP_REP_PAIR, r, r[0] ? 1 : (uint32_t)sizeof r);
    } else if (m[0] == 2 && n >= 1 + 16 + 32) {                // TRY: sid, Ya
        uint8_t r[1 + 32 + 32] = {0};
        pr_lock();
        if (!g_pm.b_active || now >= g_pm.open_until || memcmp(g_pm.b_sid, m + 1, 16)) r[0] = 3;
        else if (g_pm.b_tries >= PR_TRIES) r[0] = 4;
        else {
            g_pm.b_tries++;
            uint8_t G[32], y[32], Yb[32];
            pr_cpace_gen(G, g_pm.b_code, g_pm.b_sid, g_pm.b_peer_id, g_id);
            pr_cpace_share(Yb, y, G);
            memcpy(g_pm.b_Ya, m + 17, 32);
            if (pr_cpace_isk(g_pm.b_isk, y, g_pm.b_Ya, g_pm.b_Ya, Yb, g_pm.b_sid)) r[0] = 3;
            else {
                memcpy(g_pm.b_Yb, Yb, 32);
                g_pm.b_have = 1;
                memcpy(r + 1, Yb, 32);
                pr_tag(r + 33, g_pm.b_isk, 'B', g_pm.b_Ya, Yb);
            }
            crypto_wipe(y, sizeof y);
        }
        pr_unlock();
        pr_reply(s, OP_REP_PAIR, r, r[0] ? 1 : (uint32_t)sizeof r);
    } else if (m[0] == 3 && n >= 1 + 16 + 32) {                // CONFIRM: sid, tagA
        uint8_t r[1] = {5};
        pr_lock();
        if (g_pm.b_active && g_pm.b_have && now < g_pm.open_until && !memcmp(g_pm.b_sid, m + 1, 16)) {
            uint8_t t[32];
            pr_tag(t, g_pm.b_isk, 'A', g_pm.b_Ya, g_pm.b_Yb);
            if (crypto_verify32(t, m + 17) == 0) {
                uint8_t key[32];
                pr_pairkey(key, g_pm.b_isk, g_pm.b_peer_id, g_id);
                if (pr_store(g_pm.b_peer_id, g_pm.b_peer_name, key)) { r[0] = 6; pr_msg(0, "This PC is paired with too many PCs. Unpair one first."); }
                else {
                    r[0] = 0; save = 1;
                    pr_msg(1, "Paired with %s.", g_pm.b_peer_name);
                    snprintf(logline, sizeof logline, "Paired with %s (%s)", g_pm.b_peer_name, peer);
                }
                crypto_wipe(key, sizeof key);
                pr_end_b();
                g_pm.open_until = 0;
            }
        }
        pr_unlock();
        pr_reply(s, OP_REP_PAIR, r, 1);
    } else if (m[0] == 4 && n >= 1 + 16) {                     // STOP: the other PC cancelled, or the code was wrong 3 times
        uint8_t r[1] = {0};
        pr_lock();
        if (g_pm.b_active && !memcmp(g_pm.b_sid, m + 1, 16)) {
            if (g_pm.b_tries >= PR_TRIES) { pr_msg(0, "The code was typed wrong %d times on %s. Pairing stopped.", PR_TRIES, g_pm.b_peer_name); g_pm.open_until = 0; }
            else pr_msg(0, "%s stopped pairing.", g_pm.b_peer_name);
            pr_end_b();
        }
        pr_unlock();
        pr_reply(s, OP_REP_PAIR, r, 1);
    }
    if (logline[0]) logm("%s", logline);
    if (save) pair_save();
}

// Another PC gives this PC a device: use it here like a one-off Use (no Auto-reconnect).
static int pr_on_give(const char *peer, const char *id, const char *from, const char *busid, char *msg, size_t mn) {
    const char *note = plat_client_note();
    if (note) { snprintf(msg, mn, "%s", note); return 3; }
    if (!busid[0] || strlen(busid) > 31) { snprintf(msg, mn, "No device given."); return 3; }
    cl_lock();
    Remote *r = remote_seen(id, from, peer);
    Use *u = r ? use_new(r->id, busid) : NULL;
    if (u) {
        u->want = 1; u->next_try = 0; u->release = 0; u->ended_at = 0;
        u->give_until = now_ms() + 30000;
        r->next_poll = 0;
        for (int k = 0; k < r->ndev; k++) if (!strcmp(r->dev[k].busid, busid) && r->dev[k].name[0]) {
            snprintf(u->name, sizeof u->name, "%s", r->dev[k].name);
            u->input = r->dev[k].input;
        }
        fail_clear(r->id, busid);
    }
    cl_unlock();
    if (!u) { snprintf(msg, mn, "This PC can't take more devices right now."); return 3; }
    logm("[%s] %s gave this device to this PC", busid, from);
    return 0;
}

static void pr_srv_auth(plat_sock s, const char *peer, const uint8_t *m, uint32_t n) {
    if (n < 42) return;
    char id[40], arg[64], from[64] = "";
    pr_unfix(id, m, 40);
    int cmd = m[40], alen = m[41];
    if (alen > 63 || n < 42u + (uint32_t)alen) return;
    memcpy(arg, m + 42, (size_t)alen); arg[alen] = 0;
    for (char *c = arg; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    uint8_t key[32];
    int have = 0;
    pr_lock();
    PairRec *p = pr_find(id);
    if (p) { memcpy(key, p->key, 32); snprintf(from, sizeof from, "%s", p->name); have = 1; }
    pr_unlock();
    if (!have) { uint8_t r = 1; pr_reply(s, OP_REP_AUTH, &r, 1); return; }   // not paired with this PC
    uint8_t ch[17], mac[32], want[32];
    ch[0] = 0; plat_random(ch + 1, 16);
    pr_reply(s, OP_REP_AUTH, ch, sizeof ch);
    int got = !plat_recv(s, mac, 32);
    pr_mac(want, key, ch + 1, id, g_id, cmd, arg);
    crypto_wipe(key, sizeof key);
    if (!got) return;
    uint8_t r[1 + 160] = {0};
    if (crypto_verify32(mac, want)) {
        r[0] = 2;
        pr_reply(s, OP_REP_AUTH, r, 1);
        logm("%s sent a request as %s, but it failed the pairing check", peer, from);
        return;
    }
    pr_permit(peer, cmd == PRC_PERMIT ? arg : "");
    char msg[160] = "";
    int st = 0, save = 0;
    if (cmd == PRC_GIVE) st = pr_on_give(peer, id, from, arg, msg, sizeof msg);
    else if (cmd == PRC_UNPAIR) {
        pr_lock();
        p = pr_find(id);
        if (p) { crypto_wipe(p, sizeof *p); save = 1; }
        pr_unlock();
        logm("%s unpaired from this PC", from);
    } else if (cmd != PRC_PERMIT && cmd != PRC_PING) { st = 3; snprintf(msg, sizeof msg, "Unknown request. Is USBeam up to date on both PCs?"); }
    r[0] = (uint8_t)st;
    size_t ml = strlen(msg);
    memcpy(r + 1, msg, ml);
    pr_reply(s, OP_REP_AUTH, r, (uint32_t)(1 + ml));
    if (save) pair_save();
}

// Called for every request from another PC: 1 if it was a pairing request (answered).
static int pair_server(plat_sock s, const char *peer, uint16_t code, const uint8_t *op) {
    if (code != OP_REQ_PAIR && code != OP_REQ_AUTH) return 0;
    plat_sock_timeout(s, 3000);
    uint32_t len = cl_g32(op + 4);
    uint8_t m[512];
    if (len == 0 || len > sizeof m || plat_recv(s, m, (int)len)) return 1;
    if (code == OP_REQ_PAIR) pr_srv_pair(s, peer, m, len);
    else pr_srv_auth(s, peer, m, len);
    return 1;
}

// ---------------------------------------------------------------- asking other PCs
// A request to a paired PC. Returns its answer (0 = done), 1 = it isn't paired with this PC,
// 2 = it refused the proof, 3 = it couldn't do it (msg says why), -1 = no answer, -2 = not paired here.
static int pr_auth(const char *pc, const char *ip, int cmd, const char *arg, char *msg, size_t mn) {
    if (msg && mn) msg[0] = 0;
    uint8_t key[32];
    pr_lock();
    PairRec *p = pr_find(pc);
    if (p) memcpy(key, p->key, 32);
    pr_unlock();
    if (!p) return -2;
    uint8_t req[42 + 64], rep[1 + 160];
    size_t al = strlen(arg); if (al > 63) al = 63;
    pr_fixed(req, g_id, 40);
    req[40] = (uint8_t)cmd; req[41] = (uint8_t)al;
    memcpy(req + 42, arg, al);
    char a2[64]; memcpy(a2, arg, al); a2[al] = 0;
    int st = -1;
    uint32_t got = 0;
    plat_sock s = plat_connect(ip, 2500);
    if (s != PLAT_BAD_SOCK) {
        plat_sock_timeout(s, 5000);
        if (!pr_exchange(s, OP_REQ_AUTH, req, (uint32_t)(42 + al), OP_REP_AUTH, rep, sizeof rep, &got)) {
            if (rep[0] != 0) st = rep[0];
            else if (got >= 17) {
                uint8_t mac[32], h[8];
                pr_mac(mac, key, rep + 1, g_id, pc, cmd, a2);
                if (!plat_send(s, mac, 32) && !plat_recv(s, h, 8) && cl_g16(h + 2) == OP_REP_AUTH) {
                    uint32_t len = cl_g32(h + 4);
                    if (len >= 1 && len <= sizeof rep && !plat_recv(s, rep, (int)len)) {
                        st = rep[0];
                        if (msg && mn && len > 1) {
                            size_t ml = len - 1; if (ml > mn - 1) ml = mn - 1;
                            memcpy(msg, rep + 1, ml); msg[ml] = 0;
                            for (char *c = msg; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
                        }
                    }
                }
            }
        }
        plat_close(s);
    }
    crypto_wipe(key, sizeof key);
    return st;
}
// Before connecting a device from a paired PC: ask it to let this PC in (needed when it allows only paired PCs).
static void pair_before_attach(const char *pc, const char *ip, const char *busid) {
    if (pair_is_paired(pc)) pr_auth(pc, ip, PRC_PERMIT, busid, NULL, 0);
}
static void pair_auth_ping(const char *pc, const char *ip) {
    if (pair_is_paired(pc)) pr_auth(pc, ip, PRC_PING, "", NULL, 0);
}
static void pr_stop_remote(const char *ip, const uint8_t sid[16]) {    // tell the other PC to stop its pairing
    uint8_t m[17], rep[8]; uint32_t got;
    m[0] = 4; memcpy(m + 1, sid, 16);
    plat_sock s = plat_connect(ip, 1500);
    if (s == PLAT_BAD_SOCK) return;
    plat_sock_timeout(s, 2000);
    pr_exchange(s, OP_REQ_PAIR, m, sizeof m, OP_REP_PAIR, rep, sizeof rep, &got);
    plat_close(s);
}
static int pr_err(sb_t *out, int *code, const char *fmt, ...) {
    char t[300];
    va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    sb_add(out, "{\"error\":", 9); sb_jstr(out, t); sb_add(out, "}", 1);
    *code = 400;
    return 1;
}
static int pr_remote_addr(const char *pc, char ip[64], char name[64]) {   // 0 = reachable
    ip[0] = 0; name[0] = 0;
    cl_lock();
    Remote *r = rm_find(pc);
    if (r) {
        snprintf(name, 64, "%s", r->name[0] ? r->name : pc);
        if (r->via >= 0) snprintf(ip, 64, "%s", r->addr[r->via].ip);
    }
    cl_unlock();
    if (!name[0]) {                                   // a paired PC not seen yet: its name from the pairing
        pr_lock(); PairRec *p = pr_find(pc); if (p) snprintf(name, 64, "%s", p->name); pr_unlock();
    }
    return ip[0] ? 0 : -1;
}

// ---------------------------------------------------------------- app window: actions and state
static int pair_action(const char *op, const char *body, sb_t *out, int *code) {
    char pc[64] = "", busid[64] = "", val[32] = "";
    json_str(body, "pc", pc, sizeof pc);
    json_str(body, "busid", busid, sizeof busid);
    json_str(body, "value", val, sizeof val);
    if (!strcmp(op, "pair_open")) {                  // "Pair a PC" opened
        pr_lock();
        g_pm.open_until = now_ms() + PR_OPEN_MS;
        if (!g_pm.a_active && !g_pm.b_active) g_pm.msg[0] = 0;
        pr_unlock();
        return 1;
    }
    if (!strcmp(op, "pair_close")) {                 // closed or cancelled
        char ip[64] = ""; uint8_t sid[16];
        pr_lock();
        if (g_pm.a_active) { snprintf(ip, sizeof ip, "%s", g_pm.a_ip); memcpy(sid, g_pm.a_sid, 16); }
        pr_end_a(); pr_end_b();
        g_pm.open_until = 0; g_pm.msg[0] = 0;
        pr_unlock();
        if (ip[0]) pr_stop_remote(ip, sid);
        return 1;
    }
    if (!strcmp(op, "pair_with")) {                  // this PC types the code shown on the other one
        char ip[64], name[64];
        if (pr_remote_addr(pc, ip, name)) return pr_err(out, code, "%s can't be reached right now.", name[0] ? name : "That PC");
        uint8_t m[1 + 40 + 64], rep[1 + 16 + 40 + 64] = {0};
        uint32_t got = 0;
        m[0] = 1; pr_fixed(m + 1, g_id, 40); pr_fixed(m + 41, g_name, 64);
        int st = -1;
        plat_sock s = plat_connect(ip, 2500);
        if (s != PLAT_BAD_SOCK) {
            plat_sock_timeout(s, 4000);
            if (!pr_exchange(s, OP_REQ_PAIR, m, sizeof m, OP_REP_PAIR, rep, sizeof rep, &got)) st = rep[0];
            plat_close(s);
        }
        if (st < 0) return pr_err(out, code, "%s didn't answer. Is USBeam up to date there?", name);
        if (st == 1) return pr_err(out, code, "Open Pair a PC on %s too, then try again.", name);
        if (st == 2) return pr_err(out, code, "%s is pairing with another PC right now.", name);
        if (st != 0 || got < sizeof rep) return pr_err(out, code, "%s can't pair right now.", name);
        pr_lock();
        pr_end_b();
        g_pm.a_active = 1; g_pm.a_tries = 0;
        memcpy(g_pm.a_sid, rep + 1, 16);
        pr_unfix(g_pm.a_peer_id, rep + 17, 40);
        pr_unfix(g_pm.a_peer_name, rep + 57, 64);
        if (!g_pm.a_peer_name[0]) snprintf(g_pm.a_peer_name, sizeof g_pm.a_peer_name, "%s", name);
        snprintf(g_pm.a_ip, sizeof g_pm.a_ip, "%s", ip);
        g_pm.msg[0] = 0;
        if (g_pm.open_until < now_ms() + 30000) g_pm.open_until = now_ms() + 30000;
        pr_unlock();
        return 1;
    }
    if (!strcmp(op, "pair_code")) {                  // the code typed here
        int digits = (int)strlen(val) == 6;
        for (int i = 0; val[i]; i++) if (!isdigit((unsigned char)val[i])) digits = 0;
        if (!digits) return pr_err(out, code, "Type the 6 digits shown on the other PC.");
        char peer_id[40], peer_name[64], ip[64]; uint8_t sid[16];
        pr_lock();
        int active = g_pm.a_active;
        if (active) {
            snprintf(peer_id, sizeof peer_id, "%s", g_pm.a_peer_id); snprintf(peer_name, sizeof peer_name, "%s", g_pm.a_peer_name);
            snprintf(ip, sizeof ip, "%s", g_pm.a_ip); memcpy(sid, g_pm.a_sid, 16);
        }
        pr_unlock();
        if (!active) return pr_err(out, code, "The pairing has ended. Start again.");
        uint8_t G[32], y[32], Ya[32], isk[64], t[32];
        uint8_t m[1 + 16 + 32], rep[1 + 32 + 32] = {0};
        uint32_t got = 0;
        pr_cpace_gen(G, val, sid, g_id, peer_id);
        pr_cpace_share(Ya, y, G);
        m[0] = 2; memcpy(m + 1, sid, 16); memcpy(m + 17, Ya, 32);
        int st = -1;
        plat_sock s = plat_connect(ip, 2500);
        if (s != PLAT_BAD_SOCK) {
            plat_sock_timeout(s, 4000);
            if (!pr_exchange(s, OP_REQ_PAIR, m, sizeof m, OP_REP_PAIR, rep, sizeof rep, &got)) st = rep[0];
            plat_close(s);
        }
        int match = st == 0 && got == sizeof rep && !pr_cpace_isk(isk, y, rep + 1, Ya, rep + 1, sid);
        crypto_wipe(y, sizeof y);
        if (match) { pr_tag(t, isk, 'B', Ya, rep + 1); match = crypto_verify32(t, rep + 33) == 0; }
        if (st < 0) { crypto_wipe(isk, sizeof isk); return pr_err(out, code, "%s didn't answer. Try again.", peer_name); }
        if (st == 3 || st == 4) {
            pr_lock(); pr_end_a(); pr_msg(0, "The pairing ended on %s. Start again.", peer_name); pr_unlock();
            crypto_wipe(isk, sizeof isk);
            return pr_err(out, code, "The pairing ended on %s. Start again.", peer_name);
        }
        if (!match) {                                 // wrong code (or someone in between)
            crypto_wipe(isk, sizeof isk);
            pr_lock();
            int left = PR_TRIES - ++g_pm.a_tries;
            if (left <= 0) { pr_end_a(); pr_msg(0, "Wrong code %d times. Pairing stopped; start again.", PR_TRIES); g_pm.open_until = 0; }
            pr_unlock();
            if (left <= 0) { pr_stop_remote(ip, sid); return pr_err(out, code, "Wrong code %d times. Pairing stopped; start again.", PR_TRIES); }
            return pr_err(out, code, "That code is wrong. %d %s left.", left, left == 1 ? "try" : "tries");
        }
        uint8_t c[1 + 16 + 32], r2[8] = {0};
        c[0] = 3; memcpy(c + 1, sid, 16);
        pr_tag(c + 17, isk, 'A', Ya, rep + 1);
        st = -1;
        s = plat_connect(ip, 2500);
        if (s != PLAT_BAD_SOCK) {
            plat_sock_timeout(s, 4000);
            if (!pr_exchange(s, OP_REQ_PAIR, c, sizeof c, OP_REP_PAIR, r2, sizeof r2, &got)) st = r2[0];
            plat_close(s);
        }
        int ok = 0;
        if (st == 0) {
            uint8_t key[32];
            pr_pairkey(key, isk, g_id, peer_id);
            pr_lock();
            ok = pr_store(peer_id, peer_name, key) == 0;
            pr_end_a();
            if (ok) { pr_msg(1, "Paired with %s.", peer_name); g_pm.open_until = 0; }
            else pr_msg(0, "This PC is paired with too many PCs. Unpair one first.");
            pr_unlock();
            crypto_wipe(key, sizeof key);
        }
        crypto_wipe(isk, sizeof isk);
        if (ok) { pair_save(); logm("Paired with %s (%s)", peer_name, ip); return 1; }
        if (st == 0) return pr_err(out, code, "This PC is paired with too many PCs. Unpair one first.");
        if (st == 6) { pr_lock(); pr_end_a(); pr_unlock(); return pr_err(out, code, "%s is paired with too many PCs. Unpair one there first.", peer_name); }
        return pr_err(out, code, "%s didn't confirm the pairing. Try again.", peer_name);
    }
    if (!strcmp(op, "unpair")) {
        char ip[64], name[64];
        int reach = pr_remote_addr(pc, ip, name) == 0;
        if (reach) pr_auth(pc, ip, PRC_UNPAIR, "", NULL, 0);    // best effort: it also forgets this PC
        pr_lock();
        PairRec *p = pr_find(pc);
        if (p) crypto_wipe(p, sizeof *p);
        pr_unlock();
        pair_save();
        logm("Unpaired from %s", name[0] ? name : pc);
        return 1;
    }
    if (!strcmp(op, "set_pairedonly")) {
        g_paired_only = json_bool(body, "on");
        logm(g_paired_only ? "Only paired PCs can use this PC's devices now" : "Every PC on an allowed network can use this PC's shared devices now");
        client_save();
        return 1;
    }
    if (!strcmp(op, "give")) {                       // give one of this PC's devices to a paired PC
        char ip[64], name[64], msg[160];
        if (!busid[0]) return pr_err(out, code, "No device chosen.");
        if (!pair_is_paired(pc)) return pr_err(out, code, "Pair with that PC first.");
        if (pr_remote_addr(pc, ip, name)) return pr_err(out, code, "%s can't be reached right now.", name);
        int st = pr_auth(pc, ip, PRC_GIVE, busid, msg, sizeof msg);
        if (st == 0) { logm("[%s] given to %s", busid, name); return 1; }
        if (st == -1) return pr_err(out, code, "%s didn't answer. Is USBeam up to date there?", name);
        if (st == 1) return pr_err(out, code, "%s isn't paired with this PC anymore. Unpair it here and pair them again.", name);
        if (st == 2) return pr_err(out, code, "%s didn't accept this PC's pairing. Unpair it here and pair them again.", name);
        return pr_err(out, code, "%s couldn't take it: %s", name, msg[0] ? msg : "unknown reason.");
    }
    return 0;
}

static void pair_json(sb_t *j) {
    pr_lock();
    unsigned long long now = now_ms();
    int open = now < g_pm.open_until;
    long left = open ? (long)((g_pm.open_until - now + 999) / 1000) : 0;
    const char *role = g_pm.b_active ? "b" : g_pm.a_active ? "a" : "";
    sb_printf(j, ",\"pairing\":{\"open\":%s,\"left\":%ld,\"role\":\"%s\",\"peer\":", open ? "true" : "false", left, role);
    sb_jstr(j, g_pm.b_active ? g_pm.b_peer_name : g_pm.a_active ? g_pm.a_peer_name : "");
    sb_add(j, ",\"code\":", 8); sb_jstr(j, g_pm.b_active ? g_pm.b_code : "");
    sb_printf(j, ",\"tries_left\":%d,\"msg\":", PR_TRIES - (g_pm.a_active ? g_pm.a_tries : g_pm.b_tries));
    sb_jstr(j, g_pm.msg);
    sb_printf(j, ",\"ok\":%s}", g_pm.msg_ok ? "true" : "false");
    sb_add(j, ",\"paired\":[", 11);
    int first = 1;
    for (int i = 0; i < PR_MAX; i++) {
        PairRec *p = &g_pr[i];
        if (!p->id[0]) continue;
        if (!first) sb_add(j, ",", 1);
        first = 0;
        sb_add(j, "{\"id\":", 6); sb_jstr(j, p->id);
        sb_add(j, ",\"name\":", 8); sb_jstr(j, p->name);
        sb_printf(j, ",\"when\":%lld}", p->when);
    }
    sb_add(j, "]", 1);
    sb_printf(j, ",\"paired_only\":%s", g_paired_only ? "true" : "false");
    pr_unlock();
}

#endif
