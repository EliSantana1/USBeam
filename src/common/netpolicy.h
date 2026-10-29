// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// netpolicy.h - which networks USBeam may be used on (shared by Windows and Linux).
//
// USBeam is meant for laptops as well as desktops, at home and on the road, so it is
// only reachable on networks the user trusts:
//   - ZeroTier and Tailscale: always (private networks made of the user's own PCs).
//   - Wi-Fi / Ethernet:
//       Windows: networks Windows marks Private (or work/domain), never Public.
//       Linux:   networks the user allowed in the USBeam window (asked once per network).
// Everything else is ignored: no answers to other PCs, no announcements, no firewall opening.
//
// The platform provides, before including this file:
//   int  plat_networks(NetInfo *out, int max);   // this PC's networks right now
//   void nets_lock(void); void nets_unlock(void);

#define MAXNETS 16
typedef struct {
    uint32_t net, mask;     // host byte order
    int kind;               // NET_HOME / NET_ZT / NET_TS
    int allowed;            // USBeam may be used here
    int pub;                // Windows: marked Public
    int pending;            // Linux: still identifying the network (not asked yet)
    char id[96], name[96];
} NetInfo;

static NetInfo g_nets[MAXNETS];
static int g_nnets, g_nets_ready;
static char g_nets_later[MAXNETS][96];      // "Not now" for this run

static int plat_networks(NetInfo *out, int max);

// Rescan this PC's networks. Returns a fingerprint of the allowed ones (to notice changes).
static unsigned long nets_refresh(void) {
    NetInfo t[MAXNETS];
    int n = plat_networks(t, MAXNETS);
    unsigned long sig = 1;
    for (int i = 0; i < n; i++) if (t[i].allowed) sig = sig * 31 + (((unsigned long)t[i].net * 2654435761u) ^ t[i].mask ^ (unsigned long)t[i].kind);
    nets_lock();
    memcpy(g_nets, t, sizeof(NetInfo) * (size_t)n);
    g_nnets = n; g_nets_ready = 1;
    nets_unlock();
    return sig;
}

// May a PC at this address (host byte order) use USBeam here?
static int net_ip_allowed(uint32_t ip) {
    if ((ip >> 24) == 127) return 1;
    int ok = 0;
    nets_lock();
    for (int i = 0; i < g_nnets && !ok; i++) {
        NetInfo *x = &g_nets[i];
        if (!x->allowed) continue;
        if ((ip & x->mask) == x->net) ok = 1;
        if (x->kind == NET_TS && (ip & 0xFFC00000u) == 0x64400000u) ok = 1;     // Tailscale uses single addresses
    }
    nets_unlock();
    return ok;
}
static int net_str_allowed(const char *ip) {
    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) return 0;
    return net_ip_allowed(ntohl(a.s_addr));
}

static void nets_later(const char *id) {
    nets_lock();
    for (int i = 0; i < MAXNETS; i++) if (!g_nets_later[i][0] || !strcmp(g_nets_later[i], id)) { snprintf(g_nets_later[i], sizeof g_nets_later[i], "%s", id); break; }
    nets_unlock();
}
static int nets_is_later(const char *id) {      // call with nets_lock held
    for (int i = 0; i < MAXNETS; i++) if (g_nets_later[i][0] && !strcmp(g_nets_later[i], id)) return 1;
    return 0;
}

// "networks":[{id, name, kind, subnet, allowed, public, pending, later}] for the app window.
static void nets_json(sb_t *j) {
    static const char *KIND[] = { "auto", "home", "zerotier", "tailscale" };
    sb_add(j, "\"networks\":[", 12);
    nets_lock();
    int first = 1;
    for (int i = 0; i < g_nnets; i++) {
        NetInfo *x = &g_nets[i];
        int dup = 0;
        for (int k = 0; k < i; k++) if (!strcmp(g_nets[k].id, x->id)) dup = 1;
        if (dup) continue;
        int bits = 0; for (uint32_t m = x->mask; m; m <<= 1) bits++;
        if (!first) sb_add(j, ",", 1);
        first = 0;
        sb_add(j, "{\"id\":", 6); sb_jstr(j, x->id);
        sb_add(j, ",\"name\":", 8); sb_jstr(j, x->name);
        sb_printf(j, ",\"kind\":\"%s\",\"subnet\":\"%u.%u.%u.%u/%d\",\"allowed\":%s,\"public\":%s,\"pending\":%s,\"later\":%s}",
                  KIND[x->kind & 3], x->net >> 24, (x->net >> 16) & 255, (x->net >> 8) & 255, x->net & 255, bits,
                  x->allowed ? "true" : "false", x->pub ? "true" : "false", x->pending ? "true" : "false",
                  nets_is_later(x->id) ? "true" : "false");
    }
    nets_unlock();
    sb_add(j, "]", 1);
}
