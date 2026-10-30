// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// app_win.c - the USBeam app for Windows (included by usbeam_host.c):
// settings, reconnect countdowns, the client side (via the usbip-win2 driver),
// automatic discovery, firewall setup and the app window (local web UI).

#define COBJMACROS
#include <netlistmgr.h>
#include "../common/version.h"
#include "../common/http.h"
#include "../common/client_types.h"
#include "ui_html.h"

#define DISCO_PORT 3241

static char g_name[64];
static int g_hold_secs = 10;               // countdown before a replugged device may be taken again
static volatile LONG g_restart;            // "Restart USBeam" from the gear menu
static char g_id[33];
static char g_boot_id[64];
static char g_cfg_path[MAX_PATH];
static wchar_t g_usbip_exe[MAX_PATH];
static HANDLE g_quit_event;
static CRITICAL_SECTION g_cl_cs;          // remote PCs + devices in use from them
static CRITICAL_SECTION g_nets_cs;        // this PC's networks (netpolicy.h)

static void w2u(const wchar_t *w, char *out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL); }

// ---------------------------------------------------------------- platform hooks for the shared client
typedef SOCKET plat_sock;
#define PLAT_BAD_SOCK INVALID_SOCKET
static plat_sock plat_connect(const char *ip, int ms) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&a, sizeof a);
    fd_set w; FD_ZERO(&w); FD_SET(s, &w);
    fd_set e; FD_ZERO(&e); FD_SET(s, &e);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    if (select(0, NULL, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { closesocket(s); return INVALID_SOCKET; }
    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD to = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    return s;
}
static int plat_send(plat_sock s, const void *b, int n) { return send_all(s, b, n) ? 0 : -1; }
static int plat_recv(plat_sock s, void *b, int n) { return recv_all(s, b, n) ? 0 : -1; }
static void plat_close(plat_sock s) { closesocket(s); }
static void cl_lock(void) { EnterCriticalSection(&g_cl_cs); }
static void cl_unlock(void) { LeaveCriticalSection(&g_cl_cs); }
static unsigned long long now_ms(void) { return GetTickCount64(); }
// This PC's networks, and what kind each one is (by adapter name).
static int plat_local_nets(LocalNet *out, int max) {
    ULONG sz = 32768;
    IP_ADAPTER_ADDRESSES *aa = malloc(sz);
    int n = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = aa; a && n < max; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            int kind = NET_HOME;
            if (wcsstr(a->Description, L"ZeroTier") || wcsstr(a->FriendlyName, L"ZeroTier")) kind = NET_ZT;
            else if (wcsstr(a->Description, L"Tailscale") || wcsstr(a->FriendlyName, L"Tailscale")) kind = NET_TS;
            else if (wcsstr(a->Description, L"Hyper-V") || wcsstr(a->FriendlyName, L"vEthernet") || wcsstr(a->Description, L"VirtualBox") || wcsstr(a->Description, L"VMware")) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                uint32_t m = u->OnLinkPrefixLength ? 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength) : 0;
                if ((ip & 0xFFC00000u) == 0x64400000u) kind = NET_TS;
                out[n].net = ip & m; out[n].mask = m; out[n].kind = kind; n++;
            }
        }
    }
    free(aa);
    return n;
}
static void plat_client_tick_begin(void);
static int plat_attach(const char *ip, const char *busid);
static int plat_port_alive(int port, const char *ip, const char *busid);
static void plat_detach(int port);
static const char *plat_client_note(void);
static void plat_tune(void) {}

#include "../common/client.h"

// ---------------------------------------------------------------- which networks USBeam may be used on
static void nets_lock(void) { EnterCriticalSection(&g_nets_cs); }
static void nets_unlock(void) { LeaveCriticalSection(&g_nets_cs); }
#include "../common/netpolicy.h"

// Windows' own Private / Public setting for each connected network (Network List Manager).
// COM is loaded on demand so USBeam needs no extra libraries.
typedef HRESULT (WINAPI *CoInitEx_t)(LPVOID, DWORD);
typedef HRESULT (WINAPI *CoCreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef void (WINAPI *SysFree_t)(BSTR);
typedef struct { char guid[40]; int cat; char name[96]; } NlmEntry;

static int nlm_list(NlmEntry *e, int max) {              // -1 = can't tell
    static CoInitEx_t pCoInitializeEx; static CoCreate_t pCoCreateInstance; static SysFree_t pSysFreeString;
    static int loaded;
    static _Thread_local int co;
    if (!loaded) {
        loaded = 1;
        HMODULE o = LoadLibraryW(L"ole32.dll"), a = LoadLibraryW(L"oleaut32.dll");
        pCoInitializeEx = o ? (CoInitEx_t)(void *)GetProcAddress(o, "CoInitializeEx") : NULL;
        pCoCreateInstance = o ? (CoCreate_t)(void *)GetProcAddress(o, "CoCreateInstance") : NULL;
        pSysFreeString = a ? (SysFree_t)(void *)GetProcAddress(a, "SysFreeString") : NULL;
    }
    if (!pCoInitializeEx || !pCoCreateInstance || !pSysFreeString) return -1;
    if (!co) { co = 1; pCoInitializeEx(NULL, COINIT_MULTITHREADED); }
    INetworkListManager *m = NULL;
    if (FAILED(pCoCreateInstance(&CLSID_NetworkListManager, NULL, CLSCTX_ALL, &IID_INetworkListManager, (void **)&m)) || !m) return -1;
    IEnumNetworkConnections *en = NULL;
    int n = 0;
    if (SUCCEEDED(INetworkListManager_GetNetworkConnections(m, &en)) && en) {
        INetworkConnection *c = NULL; ULONG got = 0;
        while (n < max && IEnumNetworkConnections_Next(en, 1, &c, &got) == S_OK && got && c) {
            GUID g; INetwork *net = NULL;
            if (SUCCEEDED(INetworkConnection_GetAdapterId(c, &g)) && SUCCEEDED(INetworkConnection_GetNetwork(c, &net)) && net) {
                NLM_NETWORK_CATEGORY cat = NLM_NETWORK_CATEGORY_PUBLIC;
                INetwork_GetCategory(net, &cat);
                BSTR nm = NULL;
                INetwork_GetName(net, &nm);
                snprintf(e[n].guid, sizeof e[n].guid, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                         (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                         g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
                e[n].cat = (int)cat;
                e[n].name[0] = 0;
                if (nm) { w2u(nm, e[n].name, sizeof e[n].name); pSysFreeString(nm); }
                n++;
                INetwork_Release(net);
            }
            INetworkConnection_Release(c);
            c = NULL;
        }
        IEnumNetworkConnections_Release(en);
    }
    INetworkListManager_Release(m);
    return n;
}

static int plat_networks(NetInfo *out, int max) {
    NlmEntry nl[MAXNETS];
    int nn = nlm_list(nl, MAXNETS);
    ULONG sz = 32768;
    IP_ADAPTER_ADDRESSES *aa = malloc(sz);
    int n = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = aa ? GetAdaptersAddresses(AF_INET, flags, NULL, aa, &sz) : ERROR_NOT_ENOUGH_MEMORY;
    if (rc == ERROR_BUFFER_OVERFLOW) { free(aa); aa = malloc(sz); rc = aa ? GetAdaptersAddresses(AF_INET, flags, NULL, aa, &sz) : ERROR_NOT_ENOUGH_MEMORY; }
    if (rc == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = aa; a && n < max; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            int kind = NET_HOME;
            if (wcsstr(a->Description, L"ZeroTier") || wcsstr(a->FriendlyName, L"ZeroTier")) kind = NET_ZT;
            else if (wcsstr(a->Description, L"Tailscale") || wcsstr(a->FriendlyName, L"Tailscale")) kind = NET_TS;
            else if (wcsstr(a->Description, L"Hyper-V") || wcsstr(a->FriendlyName, L"vEthernet") || wcsstr(a->Description, L"VirtualBox") || wcsstr(a->Description, L"VMware")) continue;
            int cat = -1; char nname[96] = "";
            for (int k = 0; k < nn; k++) if (!_stricmp(nl[k].guid, a->AdapterName)) { cat = nl[k].cat; snprintf(nname, sizeof nname, "%s", nl[k].name); }
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                if ((ip & 0xFFFF0000u) == 0xA9FE0000u) continue;            // 169.254.x.x: no network
                uint32_t m = u->OnLinkPrefixLength ? 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength) : 0;
                NetInfo *x = &out[n++];
                memset(x, 0, sizeof *x);
                x->kind = (ip & 0xFFC00000u) == 0x64400000u ? NET_TS : kind;
                x->net = ip & m; x->mask = m; x->ip = ip;
                snprintf(x->id, sizeof x->id, "%s", a->AdapterName);
                if (x->kind == NET_ZT) snprintf(x->name, sizeof x->name, "ZeroTier");
                else if (x->kind == NET_TS) snprintf(x->name, sizeof x->name, "Tailscale");
                else if (nname[0]) snprintf(x->name, sizeof x->name, "%s", nname);
                else w2u(a->FriendlyName, x->name, sizeof x->name);
                if (x->kind != NET_HOME) x->allowed = 1;                    // the user's own private networks
                else if (nn < 0) x->allowed = 1;                            // can't tell: the firewall still decides
                else if (cat == NLM_NETWORK_CATEGORY_PRIVATE || cat == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED) x->allowed = 1;
                else x->pub = 1;
            }
        }
    }
    free(aa);
    return n;
}

// ---------------------------------------------------------------- settings file
// Saved from several threads: one at a time, into a new file that then replaces the old one
// (a crash or power loss mid-save can't leave a half-written settings file).
static CRITICAL_SECTION g_save_cs;
static void save_config(void) {
    EnterCriticalSection(&g_save_cs);
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.new", g_cfg_path);
    FILE *f = fopen(tmp, "w");
    if (!f) { LeaveCriticalSection(&g_save_cs); return; }
    fprintf(f, "id %s\n", g_id);
    EnterCriticalSection(&g_devs_cs);
    for (int i = 0; i < MAX_DEVS; i++) {
        Dev *d = &g_devs[i];
        if (!d->used) continue;
        char u[600]; w2u(d->inst, u, sizeof u);
        if (d->hidden) fprintf(f, "hide %s\n", u);
        if (d->share_forced) fprintf(f, "share %s\n", u);
    }
    LeaveCriticalSection(&g_devs_cs);
    cl_lock();
    client_save_lines(f);
    cl_unlock();
    int ok = fflush(f) == 0;
    fclose(f);
    if (ok) MoveFileExA(tmp, g_cfg_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    else DeleteFileA(tmp);
    LeaveCriticalSection(&g_save_cs);
}
static void client_save(void) { save_config(); }

static void load_config(void) {
    FILE *f = fopen(g_cfg_path, "r");
    if (!f) return;
    char line[700], a[600];
    while (fgets(line, sizeof line, f)) {
        int hide = sscanf(line, "hide %599[^\r\n]", a) == 1, share = !hide && sscanf(line, "share %599[^\r\n]", a) == 1;
        if (hide || share) {
            wchar_t w[256]; MultiByteToWideChar(CP_UTF8, 0, a, -1, w, 256);
            Dev *d = dev_slot(w);
            if (d) { if (hide) d->hidden = 1; else d->share_forced = 1; }
        } else if (sscanf(line, "id %32s", a) == 1) snprintf(g_id, sizeof g_id, "%s", a);
        else client_load_line(line);
    }
    fclose(f);
}

// ---------------------------------------------------------------- background monitor (host side)
// Once a second: give replugged devices back to this PC (then a countdown before
// other PCs may take them again), finish countdowns, and fix devices left on WinUSB.
static DWORD WINAPI monitor_main(LPVOID arg) {
    for (;;) {
        Sleep(1000);
        refresh_devices();
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < MAX_DEVS; i++) {
            Dev *d = &g_devs[i];
            if (!d->used || d->in_use || d->busy) continue;
            if (d->present && d->on_winusb) {
                InterlockedExchange(&d->busy, 1);
                if (d->returning) say("[%s] %ls was plugged back in. Returning it to this PC...", d->busid, d->name);
                revert_driver(d);
                if (d->returning) {
                    d->returning = 0;
                    d->hold_until = GetTickCount64() + (ULONGLONG)g_hold_secs * 1000; d->last_shown = 0;
                    say("[%s] %ls works on this PC now. Other PCs may take it again in %d s (Cancel in the USBeam window).", d->busid, d->name, g_hold_secs);
                }
                InterlockedExchange(&d->busy, 0);
                continue;
            }
            if (d->hold_until && now >= d->hold_until) {
                d->hold_until = 0; d->blocked = 0;
                say("[%s] Countdown finished: %ls is available to other PCs again.", d->busid, d->name);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- host side: info for other PCs
static const char *local_state(Dev *d, int *countdown) {
    *countdown = 0;
    if (d->busy) return "switching";
    if (d->in_use) return "in_use";
    if (!d->wanted) return "hidden";
    if (d->drive_busy_until && GetTickCount64() < d->drive_busy_until) return "drive_busy";
    if (d->hold_until) {
        LONGLONG left = (LONGLONG)(d->hold_until - GetTickCount64());
        *countdown = left > 0 ? (int)((left + 999) / 1000) : 0;
        return "countdown";
    }
    if (d->blocked) return "kept";
    return "available";
}

// Another PC introduces itself ("USBEAM2\t<id>\t<name>"), as if its announcement had arrived.
static void handle_hello(SOCKET s, const char *peer, const uint8_t *op) {
    uint32_t len = (uint32_t)op[4] << 24 | (uint32_t)op[5] << 16 | (uint32_t)op[6] << 8 | op[7];
    char buf[260];
    DWORD to = 2000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    if (len == 0 || len > 256 || !recv_all(s, buf, (int)len)) return;
    buf[len] = 0;
    disco_received(buf, peer);
}

static void handle_info(SOCKET s) {
    refresh_devices();
    sb_t j = {0};
    sb_add(&j, "{\"name\":", 8); sb_jstr(&j, g_name);
    sb_add(&j, ",\"id\":", 6); sb_jstr(&j, g_id);
    sb_add(&j, ",\"hello\":true", 13);           // this version understands OP_REQ_HELLO
    sb_add(&j, ",\"devices\":[", 12);
    int first = 1;
    EnterCriticalSection(&g_devs_cs);
    for (int i = 0; i < MAX_DEVS; i++) {
        Dev *d = &g_devs[i];
        if (!d->used || !d->present || !d->removable || !d->wanted || !d->have_desc) continue;
        int cd; const char *st = local_state(d, &cd);
        if (!strcmp(st, "switching")) st = "in_use";
        char name[512]; w2u(d->name, name, sizeof name);
        if (!first) sb_add(&j, ",", 1);
        first = 0;
        sb_add(&j, "{\"busid\":", 9); sb_jstr(&j, d->busid);
        sb_add(&j, ",\"name\":", 8); sb_jstr(&j, name);
        int input = d->have_desc && devp_is_input((const unsigned char (*)[3])d->iface_class, d->num_ifaces);
        sb_printf(&j, ",\"vid\":\"%04x\",\"pid\":\"%04x\",\"state\":\"%s\",\"countdown\":%d,\"input\":%s,\"peer\":", d->vid, d->pid, st, cd, input ? "true" : "false");
        sb_jstr(&j, d->in_use ? d->peer : "");
        sb_add(&j, "}", 1);
    }
    LeaveCriticalSection(&g_devs_cs);
    sb_add(&j, "]}", 2);
    uint8_t h[12] = {0};
    put16(h, USBIP_VERSION); put16(h + 2, OP_REP_INFO); put32(h + 8, (uint32_t)j.len);
    send_all(s, h, 12);
    send_all(s, j.p, (int)j.len);
    sb_free(&j);
}

// ---------------------------------------------------------------- running helper programs (hidden)
static int run_hidden(const wchar_t *exe, const wchar_t *args, char *out, int outsz, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (out) { out[0] = 0; if (!CreatePipe(&rd, &wr, &sa, 0)) return -1; SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); }
    wchar_t cmd[1024];
    _snwprintf(cmd, 1024, L"\"%ls\" %ls", exe, args);
    cmd[1023] = 0;
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW | (out ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, out != NULL, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (wr) CloseHandle(wr);
    if (!ok) { if (rd) CloseHandle(rd); return -1; }
    int got = 0;
    if (out) {
        DWORD n;
        while (got < outsz - 1 && ReadFile(rd, out + got, (DWORD)(outsz - 1 - got), &n, NULL) && n) got += (int)n;
        out[got] = 0;
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// ---------------------------------------------------------------- client side: usbip-win2
static int have_usbip(void) { return GetFileAttributesW(g_usbip_exe) != INVALID_FILE_ATTRIBUTES; }

// Ports currently attached, parsed from `usbip port` (blocks: "Port NN: ..." then "-> usbip://host:3240/busid").
typedef struct { int port; char host[64], busid[32]; } Attached;
static int usbip_ports(Attached *a, int max) {
    static char out[32768];
    if (run_hidden(g_usbip_exe, L"port", out, sizeof out, 15000) < 0) return -1;
    int n = 0, cur = -1;
    for (char *line = strtok(out, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        int num;
        if (!strncmp(p, "Port ", 5) && sscanf(p + 5, "%d", &num) == 1) { cur = num; continue; }
        char *u = strstr(p, "usbip://");
        if (u && cur >= 0 && n < max) {
            char host[64] = "", busid[32] = "";
            if (sscanf(u + 8, "%63[^:]:%*d/%31s", host, busid) == 2) {
                a[n].port = cur;
                snprintf(a[n].host, sizeof a[n].host, "%s", host);
                snprintf(a[n].busid, sizeof a[n].busid, "%s", busid);
                n++;
            }
            cur = -1;
        }
    }
    return n;
}

// usbip-win2 reconnects by itself (for up to 2 hours) when a connection drops. USBeam turns
// that off and decides itself (Auto-reconnect): "--once" on attach, and "--stop-all" whenever
// a connection ends. Older usbip-win2 versions without these options still work.
static int g_no_once, g_no_stopall;
static int usbip_unknown_option(const char *out) {
    return strstr(out, "not expected") || strstr(out, "nrecognized") || strstr(out, "--help") || strstr(out, "xtraArgs");
}
static void usbip_stop_retries(void) {
    if (g_no_stopall || !have_usbip()) return;
    char out[1024] = "";
    run_hidden(g_usbip_exe, L"attach --stop-all", out, sizeof out, 15000);
    if (usbip_unknown_option(out)) { g_no_stopall = 1; logm("usbip-win2 has no --stop-all option (older version)"); }
}

static int usbip_attach(const char *host, const char *busid) {
    wchar_t args[256];
    char out[2048] = "";
    _snwprintf(args, 256, L"attach -r %hs -b %hs%ls", host, busid, g_no_once ? L"" : L" --once");
    args[255] = 0;
    run_hidden(g_usbip_exe, args, out, sizeof out, 60000);
    if (!g_no_once && usbip_unknown_option(out)) {
        g_no_once = 1;
        logm("usbip-win2 has no --once option (older version): attaching without it");
        _snwprintf(args, 256, L"attach -r %hs -b %hs", host, busid);
        args[255] = 0;
        out[0] = 0;
        run_hidden(g_usbip_exe, args, out, sizeof out, 60000);
    }
    char *p = strstr(out, "port ");
    if (strstr(out, "succes") && p) { logm("attach %s %s: %s", host, busid, out); return atoi(p + 5); }
    logm("attach %s %s failed: %s", host, busid, out);
    return -1;
}

static void usbip_detach(int port) {
    wchar_t args[64]; _snwprintf(args, 64, L"detach -p %d", port); args[63] = 0;
    char out[1024];
    run_hidden(g_usbip_exe, args, out, sizeof out, 15000);
    logm("detach port %d: %s", port, out);
}

static Attached g_att[64];
static int g_natt = -1;
static void plat_client_tick_begin(void) { g_natt = usbip_ports(g_att, 64); }
static int plat_attach(const char *ip, const char *busid) { return usbip_attach(ip, busid); }
static int plat_port_alive(int port, const char *ip, const char *busid) {
    if (g_natt < 0) return 1;                     // couldn't check: assume still attached
    for (int k = 0; k < g_natt; k++) if (g_att[k].port == port) return 1;
    return 0;
}
static void plat_detach(int port) { usbip_detach(port); }
static void plat_stop_retries(void) { usbip_stop_retries(); }
static int plat_attached(int *port, char (*ip)[64], char (*busid)[32], int max) {
    int n = 0;
    for (int k = 0; k < g_natt && n < max; k++) {
        port[n] = g_att[k].port;
        snprintf(ip[n], 64, "%s", g_att[k].host);
        snprintf(busid[n], 32, "%s", g_att[k].busid);
        n++;
    }
    return n;
}
static const char *plat_client_note(void) {
    return have_usbip() ? NULL : "To use devices from other PCs on this Windows PC, the usbip-win2 driver is needed (github.com/vadimgrn/usbip-win2). Install USBeam again: its installer includes it.";
}

static DWORD WINAPI client_main(LPVOID arg) {
    if (have_usbip()) usbip_stop_retries();      // nothing reconnects behind USBeam's back
    for (;;) {
        if (WaitForSingleObject(g_quit_event, 1000) == WAIT_OBJECT_0) break;
        client_tick();
    }
    return 0;
}

// ---------------------------------------------------------------- discovery
static int is_local_ip(const char *ip) {
    char host[256] = ""; gethostname(host, sizeof host);
    struct addrinfo hints = {0}, *res = NULL, *p;
    hints.ai_family = AF_INET;
    int r = 0;
    if (!getaddrinfo(host, NULL, &hints, &res)) {
        for (p = res; p; p = p->ai_next) { char b[64]; inet_ntop(AF_INET, &((struct sockaddr_in *)p->ai_addr)->sin_addr, b, sizeof b); if (!strcmp(b, ip)) r = 1; }
        freeaddrinfo(res);
    }
    return r;
}

static void disco_announce(SOCKET us) {
    char msg[128]; disco_message(msg, sizeof msg);
    ULONG sz = 32768;
    IP_ADAPTER_ADDRESSES *aa = malloc(sz);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
                if (!net_ip_allowed(ntohl(sin->sin_addr.s_addr))) continue;     // not a network USBeam is used on
                ULONG mask = u->OnLinkPrefixLength ? htonl(0xFFFFFFFFu << (32 - u->OnLinkPrefixLength)) : 0;
                struct sockaddr_in b = *sin;
                b.sin_addr.s_addr = sin->sin_addr.s_addr | ~mask;
                b.sin_port = htons(DISCO_PORT);
                sendto(us, msg, (int)strlen(msg), 0, (struct sockaddr *)&b, sizeof b);
            }
        }
    }
    free(aa);
}

static void firewall_sync(void);

// The ZeroTier / Tailscale address ranges this PC is on (for the firewall), e.g. "10.0.0.0/24,100.64.0.0/10".
static void vpn_ranges(char *out, size_t n) {
    out[0] = 0;
    nets_lock();
    for (int i = 0; i < g_nnets; i++) {
        NetInfo *x = &g_nets[i];
        if (x->kind == NET_HOME) continue;
        char r[40];
        if (x->kind == NET_TS) snprintf(r, sizeof r, "100.64.0.0/10");
        else { int bits = 0; for (uint32_t m = x->mask; m; m <<= 1) bits++;
               snprintf(r, sizeof r, "%u.%u.%u.%u/%d", x->net >> 24, (x->net >> 16) & 255, (x->net >> 8) & 255, x->net & 255, bits); }
        if (strstr(out, r)) continue;
        if (strlen(out) + strlen(r) + 2 >= n) break;
        if (out[0]) strcat(out, ",");
        strcat(out, r);
    }
    nets_unlock();
}

static DWORD WINAPI disco_main(LPVOID arg) {
    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL one = TRUE;
    setsockopt(us, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof one);
    setsockopt(us, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(DISCO_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(us, (struct sockaddr *)&a, sizeof a)) logm("Discovery port %d busy: other PCs must be added by address", DISCO_PORT);
    DWORD to = 1000; setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    // Announcements go out on their own non-blocking socket: a network that can't take
    // the message right now is skipped (next try in 3 s) and never holds anything up.
    SOCKET snd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1; ioctlsocket(snd, FIONBIO, &nb);
    setsockopt(snd, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof one);
    ULONGLONG next = 0;
    char fw_done[1024] = "-";
    for (;;) {
        if (WaitForSingleObject(g_quit_event, 0) == WAIT_OBJECT_0) break;
        if (GetTickCount64() >= next) {
            nets_refresh();                      // networks come and go (Wi-Fi, ZeroTier after boot...)
            char vr[1024]; vpn_ranges(vr, sizeof vr);
            if (strcmp(vr, fw_done)) { snprintf(fw_done, sizeof fw_done, "%s", vr); firewall_sync(); }
            disco_announce(snd);
            next = GetTickCount64() + 3000;
        }
        char buf[128]; struct sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(us, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 8) continue;
        buf[n] = 0;
        char ip[64]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        if (is_local_ip(ip) || !net_str_allowed(ip)) continue;
        if (disco_received(buf, ip) && disco_reply_due(ip)) {   // answer directly: one announcement getting through is enough
            char msg[128]; int len = disco_message(msg, sizeof msg);
            struct sockaddr_in to = from; to.sin_port = htons(DISCO_PORT);
            if (len > 0) sendto(snd, msg, len, 0, (struct sockaddr *)&to, sizeof to);
        }
    }
    closesocket(us);
    closesocket(snd);
    return 0;
}

// Firewall: other PCs may reach USBeam (USB/IP 3240/tcp, discovery 3241/udp) only
//   - on Wi-Fi / Ethernet networks Windows marks Private (or work/domain), never Public;
//   - on ZeroTier / Tailscale (often marked Public by Windows), but only from their own addresses.
static void firewall_sync(void) {
    wchar_t netsh[MAX_PATH], exe[MAX_PATH], args[2048];
    GetSystemDirectoryW(netsh, MAX_PATH); wcscat(netsh, L"\\netsh.exe");
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    run_hidden(netsh, L"advfirewall firewall delete rule name=\"USBeam\"", NULL, 0, 10000);
    run_hidden(netsh, L"advfirewall firewall delete rule name=\"USBeam (ZeroTier/Tailscale)\"", NULL, 0, 10000);
    _snwprintf(args, 2048, L"advfirewall firewall add rule name=\"USBeam\" dir=in action=allow program=\"%ls\" remoteip=localsubnet enable=yes profile=private,domain", exe);
    args[2047] = 0;
    int r = run_hidden(netsh, args, NULL, 0, 10000);
    char vr[1024]; vpn_ranges(vr, sizeof vr);
    int r2 = 0;
    if (vr[0]) {
        _snwprintf(args, 2048, L"advfirewall firewall add rule name=\"USBeam (ZeroTier/Tailscale)\" dir=in action=allow program=\"%ls\" remoteip=%hs enable=yes profile=any", exe, vr);
        args[2047] = 0;
        r2 = run_hidden(netsh, args, NULL, 0, 10000);
    }
    logm("Firewall: USBeam allowed on Private networks%s%s (%s)", vr[0] ? " and " : "", vr, (r == 0 && r2 == 0) ? "ok" : "could not be fully set");
}

// ---------------------------------------------------------------- app window API
static Dev *dev_by_busid(const char *busid) {
    for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].used && !strcmp(g_devs[i].busid, busid)) return &g_devs[i];
    return NULL;
}

static void add_local_json(sb_t *j) {
    sb_add(j, "\"local\":[", 9);
    EnterCriticalSection(&g_devs_cs);
    int first = 1;
    for (int i = 0; i < MAX_DEVS; i++) {
        Dev *d = &g_devs[i];
        if (!d->used || !d->present || !d->removable) continue;
        int cd; const char *st = local_state(d, &cd);
        int sys = !d->in_use && !d->share_forced && (d->is_radio || d->is_light);
        const char *note = d->is_net ? "network adapter, never shared" :
                           ((d->is_storage || d->is_light) && !d->share_forced && !d->in_use) ?
                               (d->is_radio ? "Bluetooth/wireless adapter: shared only if you choose Share" :
                                d->is_light ? "lighting controller: shared only if you choose Share" : "drive: shared only if you choose Share") : "";
        char name[512]; w2u(d->name, name, sizeof name);
        if (!first) sb_add(j, ",", 1);
        first = 0;
        sb_add(j, "{\"id\":", 6); sb_jstr(j, d->busid);
        sb_add(j, ",\"name\":", 8); sb_jstr(j, name);
        sb_printf(j, ",\"vid\":\"%04x\",\"pid\":\"%04x\",\"state\":\"%s\",\"countdown\":%d,\"fixed\":%s,\"peer\":",
                  d->vid, d->pid, st, cd, d->is_net ? "true" : "false");
        char pl[160] = ""; if (d->in_use) peer_label(d->peer, pl, sizeof pl, 0);
        sb_jstr(j, pl);
        sb_add(j, ",\"note\":", 8); sb_jstr(j, note);
        sb_printf(j, ",\"system\":%s}", sys ? "true" : "false");
    }
    LeaveCriticalSection(&g_devs_cs);
    sb_add(j, "]", 1);
}

static int ui_api(const char *method, const char *path, const char *body, sb_t *out);

// "Save diagnostics": everything useful for finding a problem, as one text (the app saves it as a file).
static void diagnostics(sb_t *out) {
    sb_t t = {0};
    SYSTEMTIME st; GetLocalTime(&st);
    OSVERSIONINFOW ov = { sizeof ov };
    typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);
    RtlGetVersion_t rgv = (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&ov);
    sb_printf(&t, "USBeam %s diagnostics, %04d-%02d-%02d %02d:%02d\nWindows %lu.%lu build %lu\nThis PC: %s (id %s)\nusbip-win2: %s\n\n",
              USBEAM_VERSION, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
              ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, g_name, g_id, have_usbip() ? "installed" : "not installed");
    sb_printf(&t, "%s", "State:\n");
    ui_api("GET", "/api/state", "", &t);
    sb_printf(&t, "%s", "\n\nLog (most recent last):\n");
    EnterCriticalSection(&g_log_cs);
    if (g_log) fflush(g_log);
    FILE *f = _wfopen(g_log_path, L"rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f), from = n > 200000 ? n - 200000 : 0;
        fseek(f, from, SEEK_SET);
        char buf[4096]; size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) sb_add(&t, buf, k);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
    sb_add(out, "{\"ok\":true,\"text\":", 18);
    sb_jstr(out, t.p ? t.p : "");
    sb_add(out, "}", 1);
    sb_free(&t);
}

static int ui_api(const char *method, const char *path, const char *body, sb_t *out) {
    if (!strcmp(method, "GET") && !strcmp(path, "/api/state")) {
        sb_printf(out, "%s", "{\"os\":\"windows\",\"version\":\"" USBEAM_VERSION "\",\"name\":"); sb_jstr(out, g_name);
        sb_add(out, ",", 1); add_local_json(out);
        sb_add(out, ",", 1); client_json(out);
        sb_add(out, ",", 1); nets_json(out);
        sb_add(out, ",", 1); prefs_json(out);
#if defined(__aarch64__) || defined(_M_ARM64)
        sb_printf(out, "%s", ",\"arch\":\"arm64\",\"install\":\"windows\"");
#else
        sb_printf(out, "%s", ",\"arch\":\"x64\",\"install\":\"windows\"");
#endif
        sb_add(out, "}", 1);
        return 200;
    }
    if (strcmp(method, "POST") || strcmp(path, "/api/action")) return 404;
    char op[32], id[64];
    json_str(body, "op", op, sizeof op);
    json_str(body, "id", id, sizeof id);
    Dev *d = id[0] ? dev_by_busid(id) : NULL;
    int code = 200;

    if (!strcmp(op, "forget_auto")) { client_forget_auto(); }
    else if (!strcmp(op, "quit")) { logm("Quit requested from the USBeam app"); SetEvent(g_quit_event); }
    else if (!strcmp(op, "restart")) { logm("Restart requested from the USBeam app"); InterlockedExchange(&g_restart, 1); SetEvent(g_quit_event); }
    else if (!strcmp(op, "diagnostics")) { diagnostics(out); return 200; }
    else if (!strcmp(op, "reset")) {
        EnterCriticalSection(&g_devs_cs);
        for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].used) { g_devs[i].hidden = 0; g_devs[i].share_forced = 0; g_devs[i].blocked = 0; g_devs[i].hold_until = 0; }
        LeaveCriticalSection(&g_devs_cs);
        nets_lock(); memset(g_nets_later, 0, sizeof g_nets_later); nets_unlock();
        int c2 = 200; client_action("reset_client", body, out, &c2);
        logm("USBeam was reset to its starting settings");
    }
    else if (!strcmp(op, "net_later")) { char nid[96]; json_str(body, "net", nid, sizeof nid); if (nid[0]) nets_later(nid); }
    else if (!strcmp(op, "local_cancel")) { if (d && d->hold_until) { d->hold_until = 0; d->blocked = 1; say("[%s] reconnect cancelled: %ls stays on this PC", d->busid, d->name); } }
    else if (!strcmp(op, "local_allow")) { if (d) { d->blocked = 0; d->hold_until = 0; } }
    else if (!strcmp(op, "local_hide")) { if (d) { d->hidden = 1; d->share_forced = 0; save_config(); } }
    else if (!strcmp(op, "local_share")) {
        if (d) {
            if (d->is_net) { sb_printf(out, "%s", "{\"error\":\"Network adapters can't be shared: USBeam itself may depend on them.\"}"); return 400; }
            d->hidden = 0; d->share_forced = d->is_storage || d->is_light; d->blocked = 0; d->hold_until = 0; save_config();
        }
    }
    else if (!strcmp(op, "local_disconnect")) { if (d && d->in_use) { d->blocked = 1; say("[%s] taking %ls back from %s", d->busid, d->name, d->peer); end_session(d); } }
    else if (client_action(op, body, out, &code)) { if (code != 200) return code; }
    else { sb_printf(out, "%s", "{\"error\":\"unknown action\"}"); return 400; }
    sb_printf(out, "%s", "{\"ok\":true}");
    return 200;
}

static DWORD WINAPI ui_main(LPVOID arg) {
    SOCKET ls = (SOCKET)(uintptr_t)arg;
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(50); continue; }
        DWORD to = 2000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
        ui_serve(c, ui_api);
        closesocket(c);
    }
    return 0;
}

// ---------------------------------------------------------------- Windows service
// USBeam's engine runs as a Windows service ("USBeam"), started with Windows.
// The USBeam app (tray icon + window) talks to it on 127.0.0.1:3242 using the
// secret key it writes to %ProgramData%\USBeam\ui-token.
static SERVICE_STATUS_HANDLE g_svc;
static SERVICE_STATUS g_svc_status;
static wchar_t g_data_dir[MAX_PATH];

static void svc_report(DWORD state) {
    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState = state;
    g_svc_status.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
    g_svc_status.dwWaitHint = state == SERVICE_RUNNING ? 0 : 15000;
    if (g_svc) SetServiceStatus(g_svc, &g_svc_status);
}

static DWORD WINAPI svc_ctrl(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx) {
    if (ctrl == SERVICE_CONTROL_PRESHUTDOWN) {          // a real shut down / restart
        client_forget_auto();
        svc_report(SERVICE_STOP_PENDING);
        SetEvent(g_quit_event);
        return NO_ERROR;
    }
    if (ctrl == SERVICE_CONTROL_STOP) { svc_report(SERVICE_STOP_PENDING); SetEvent(g_quit_event); return NO_ERROR; }
    if (ctrl == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void write_token_file(void) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\ui-token", g_data_dir);
    FILE *f = _wfopen(p, L"w");
    if (f) { fputs(g_ui_token, f); fclose(f); }
}

static int engine_run(void) {
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_devs_cs);
    InitializeCriticalSection(&g_hubdesc_cs);
    InitializeCriticalSection(&g_cl_cs);
    InitializeCriticalSection(&g_nets_cs);
    InitializeCriticalSection(&g_save_cs);
    g_quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    wchar_t pd[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) wcscpy(pd, L"C:\\ProgramData");
    _snwprintf(g_data_dir, MAX_PATH, L"%ls\\USBeam", pd);
    CreateDirectoryW(g_data_dir, NULL);
    wchar_t wp[MAX_PATH];
    _snwprintf(g_log_path, MAX_PATH, L"%ls\\usbeam.log", g_data_dir);
    log_open();
    _snwprintf(wp, MAX_PATH, L"%ls\\settings.txt", g_data_dir);
    WideCharToMultiByte(CP_ACP, 0, wp, -1, g_cfg_path, MAX_PATH, NULL, NULL);
    logm("======== USBeam service started (version " USBEAM_VERSION ")");
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) wcscpy(pf, L"C:\\Program Files");
    _snwprintf(g_usbip_exe, MAX_PATH, L"%ls\\USBip\\usbip.exe", pf);

    HMODULE nd = LoadLibraryW(L"newdev.dll");
    pDiInstallDevice = nd ? (DiInstallDevice_t)GetProcAddress(nd, "DiInstallDevice") : NULL;
    pDiUninstallDevice = nd ? (DiUninstallDevice_t)GetProcAddress(nd, "DiUninstallDevice") : NULL;
    if (!pDiInstallDevice || !pDiUninstallDevice) { logm("ERROR: newdev.dll functions missing"); return 1; }

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&excl, sizeof excl);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) { logm("ERROR: port %d is in use by another program", USBIP_PORT); return 1; }
    ui_make_token();
    SOCKET uis = ui_listen();
    if (uis == INVALID_SOCKET) { logm("ERROR: local port %d is in use", UI_PORT); return 1; }
    write_token_file();
    gethostname(g_name, sizeof g_name);
    prefs_init();

    {   // boot id: when Windows was started (stays the same through sleep and hibernate)
        ULONGLONG up = GetTickCount64() / 1000;
        long long boot = (long long)time(NULL) - (long long)up;
        snprintf(g_boot_id, sizeof g_boot_id, "%lld", boot / 300);
    }
    load_config();
    if (!g_id[0]) { make_random_id(g_id); save_config(); }
    refresh_devices();
    for (int i = 0; i < MAX_DEVS; i++) {     // safety net
        Dev *d = &g_devs[i];
        if (d->used && d->present && d->on_winusb) { say("Returning %ls to this PC (was left on the sharing driver).", d->name); revert_driver(d); }
    }
    nets_refresh();          // which networks USBeam may be used on (the firewall is set by the discovery thread)

    CloseHandle(CreateThread(NULL, 0, server_main, (LPVOID)(uintptr_t)ls, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, monitor_main, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, ui_main, (LPVOID)(uintptr_t)uis, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, client_main, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, disco_main, NULL, 0, NULL));
    svc_report(SERVICE_RUNNING);

    WaitForSingleObject(g_quit_event, INFINITE);

    logm("Stopping: returning devices to this PC and releasing remote devices...");
    for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].used && g_devs[i].in_use) end_session(&g_devs[i]);
    if (have_usbip()) {
        client_release_all();
        usbip_stop_retries();
        g_natt = usbip_ports(g_att, 64);         // Quit ends every connection, also ones USBeam didn't make
        for (int k = 0; k < g_natt; k++) usbip_detach(g_att[k].port);
    }
    Sleep(3000);       // let sessions finish returning devices
    {   // the app shows "starting…" until the service runs again
        wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%ls\\ui-token", g_data_dir); DeleteFileW(p);
    }
    logm("======== USBeam service stopped%s", g_restart ? " (restarting)" : "");
    if (g_log) fflush(g_log);
    return g_restart ? 3 : 0;       // 3: Windows starts the service again (failure actions)
}

static void WINAPI svc_main(DWORD argc, LPWSTR *argv) {
    g_svc = RegisterServiceCtrlHandlerExW(L"USBeam", svc_ctrl, NULL);
    svc_report(SERVICE_START_PENDING);
    int r = engine_run();
    g_svc_status.dwWin32ExitCode = r ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    g_svc_status.dwServiceSpecificExitCode = (DWORD)r;
    svc_report(SERVICE_STOPPED);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        if (!strcmp(argv[i], "--console")) return engine_run();     // developers: run in the foreground
    }
    SERVICE_TABLE_ENTRYW table[] = { { L"USBeam", svc_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherW(table)) {
        MessageBoxW(NULL, L"This is USBeam's background service. It is started by Windows; open the USBeam app instead.", L"USBeam", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    return 0;
}
