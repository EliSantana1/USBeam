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
    uint32_t ip;            // this PC's own address on it
    int kind;               // NET_HOME / NET_ZT / NET_TS
    int allowed;            // USBeam may be used here
    int pub;                // Windows: marked Public
    int pending;            // Linux: still identifying the network (not asked yet)
    int blocked;            // a VPN app's network the user chose "Never allow" for
    char id[96], name[96];
    char app[48];           // the VPN app this network comes from ("" for Wi-Fi / Ethernet)
} NetInfo;

static NetInfo g_nets[MAXNETS];
static int g_nnets, g_nets_ready;
static char g_nets_later[MAXNETS][96];      // "Not now" for this run

static int plat_networks(NetInfo *out, int max);

// VPN apps other than ZeroTier and Tailscale. Their networks are named after the app and are off
// until the user allows that app (once for all its networks: a VPN gets a new address each time it
// connects). "Never allow" keeps them off; both are undone in the gear menu.
// Looked up in the network's name (Linux) or adapter description and name (Windows).
static const char *vpn_app(const char *s) {
    static const char *T[][2] = {
        {"wgpia", "Private Internet Access"}, {"private internet access", "Private Internet Access"}, {"pia ", "Private Internet Access"},
        {"nordlynx", "NordVPN"}, {"nordvpn", "NordVPN"}, {"mullvad", "Mullvad VPN"}, {"proton", "Proton VPN"},
        {"expressvpn", "ExpressVPN"}, {"surfshark", "Surfshark"}, {"windscribe", "Windscribe"},
        {"cloudflarewarp", "Cloudflare WARP"}, {"cloudflare warp", "Cloudflare WARP"}, {"ipvanish", "IPVanish"},
        {"cyberghost", "CyberGhost"}, {"hotspot shield", "Hotspot Shield"}, {"forticlient", "FortiClient"},
        {"fortinet", "FortiClient"}, {"globalprotect", "GlobalProtect"}, {"palo alto", "GlobalProtect"},
        {"cisco anyconnect", "Cisco Secure Client"}, {"cisco secure", "Cisco Secure Client"}, {"openvpn", "OpenVPN"},
    };
    char low[256]; size_t n = 0;
    for (; s && s[n] && n < sizeof low - 2; n++) low[n] = (char)tolower((unsigned char)s[n]);
    low[n] = ' '; low[n + 1] = 0;                   // so "pia " also matches at the very end
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (strstr(low, T[i][0])) return T[i][1];
    // Other tunnels: Linux tun/tap/wg/ppp interfaces, Windows WireGuard/Wintun/TAP adapters.
    if (!strncmp(low, "tun", 3) || !strncmp(low, "tap", 3) || !strncmp(low, "wg", 2) || !strncmp(low, "ppp", 3) ||
        !strncmp(low, "ipsec", 5) || strstr(low, "wireguard") || strstr(low, "wintun") || strstr(low, "tap-windows") || strstr(low, "vpn"))
        return "VPN (app unknown)";
    return "";
}
#define MAXAPPS 16
static struct { char app[48]; int mode; } g_netapps[MAXAPPS];   // mode: 1 = allowed, 2 = never allow
static int netapp_mode(const char *app) {        // call with nets_lock held
    for (int i = 0; i < MAXAPPS; i++) if (g_netapps[i].app[0] && !strcmp(g_netapps[i].app, app)) return g_netapps[i].mode;
    return 0;
}
static void netapp_set(const char *app, int mode) {     // 0 = ask again
    if (!app || !app[0]) return;
    nets_lock();
    int done = 0;
    for (int i = 0; i < MAXAPPS; i++) if (g_netapps[i].app[0] && !strcmp(g_netapps[i].app, app)) { if (mode) g_netapps[i].mode = mode; else g_netapps[i].app[0] = 0; done = 1; }
    if (mode && !done) for (int i = 0; i < MAXAPPS; i++) if (!g_netapps[i].app[0]) {
        snprintf(g_netapps[i].app, sizeof g_netapps[i].app, "%s", app); g_netapps[i].mode = mode; break;
    }
    nets_unlock();
}
// Settings file lines: "netapp allow <app>" / "netapp block <app>".
static void netapp_save_lines(FILE *f) {
    nets_lock();
    for (int i = 0; i < MAXAPPS; i++) if (g_netapps[i].app[0]) fprintf(f, "netapp %s %s\n", g_netapps[i].mode == 2 ? "block" : "allow", g_netapps[i].app);
    nets_unlock();
}
static int netapp_load_line(const char *line) {
    char m[8], app[48];
    if (sscanf(line, "netapp %7s %47[^\r\n]", m, app) != 2) return 0;
    netapp_set(app, !strcmp(m, "block") ? 2 : 1);
    return 1;
}

// Rescan this PC's networks. Returns a fingerprint of the allowed ones (to notice changes).
static unsigned long nets_refresh(void) {
    NetInfo t[MAXNETS];
    int n = plat_networks(t, MAXNETS);
    nets_lock();
    for (int i = 0; i < n; i++) if (t[i].app[0] && t[i].kind == NET_HOME) {
        int mode = netapp_mode(t[i].app);
        t[i].allowed = mode == 1; t[i].blocked = mode == 2; t[i].pub = 0; t[i].pending = 0;
    }
    nets_unlock();
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
        sb_printf(j, ",\"kind\":\"%s\",\"subnet\":\"%u.%u.%u.%u/%d\",\"allowed\":%s,\"public\":%s,\"pending\":%s,\"later\":%s",
                  KIND[x->kind & 3], x->net >> 24, (x->net >> 16) & 255, (x->net >> 8) & 255, x->net & 255, bits,
                  x->allowed ? "true" : "false", x->pub ? "true" : "false", x->pending ? "true" : "false",
                  nets_is_later(x->id) ? "true" : "false");
        sb_add(j, ",\"app\":", 7); sb_jstr(j, x->app);
        sb_printf(j, ",\"blocked\":%s}", x->blocked ? "true" : "false");
    }
    sb_add(j, "],\"netapps\":[", 13);
    first = 1;
    for (int i = 0; i < MAXAPPS; i++) if (g_netapps[i].app[0]) {
        if (!first) sb_add(j, ",", 1);
        first = 0;
        sb_add(j, "{\"app\":", 7); sb_jstr(j, g_netapps[i].app);
        sb_printf(j, ",\"blocked\":%s}", g_netapps[i].mode == 2 ? "true" : "false");
    }
    nets_unlock();
    sb_add(j, "]", 1);
}

// "Desktop (10.0.0.5)", "this PC (10.0.0.7)" or just the address, for "In use by ...".
static void peer_label(const char *ip, char *out, size_t n, int cl_held) {
    char name[64] = "";
    struct in_addr a;
    if (!ip || !ip[0]) { snprintf(out, n, "%s", ""); return; }
    if (inet_pton(AF_INET, ip, &a) == 1) {
        uint32_t h = ntohl(a.s_addr);
        nets_lock();
        for (int i = 0; i < g_nnets; i++) if (g_nets[i].ip == h) snprintf(name, sizeof name, "this PC");
        nets_unlock();
    }
    if (!name[0]) {
        if (!cl_held) cl_lock();
        for (int i = 0; i < CL_MAXR && !name[0]; i++)
            for (int k = 0; k < g_r[i].naddr; k++)
                if (g_r[i].id[0] && !strcmp(g_r[i].addr[k].ip, ip) && g_r[i].name[0] && strcmp(g_r[i].name, ip)) { snprintf(name, sizeof name, "%s", g_r[i].name); break; }
        if (!cl_held) cl_unlock();
    }
    if (name[0]) snprintf(out, n, "%s (%s)", name, ip);
    else snprintf(out, n, "%s", ip);
}
