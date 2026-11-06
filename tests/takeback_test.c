// SPDX-License-Identifier: GPL-3.0-or-later
// "Take back" test: when the PC a device is plugged into takes it back, the PC that was using it
// (with Auto-reconnect on) lets it go instead of grabbing it again; other cases still reconnect,
// and Use on this PC still works. Runs the shared client logic (client.h) with a stand-in platform.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static const unsigned char ui_html[1]; static const unsigned int ui_html_len = 0;
#include "../src/common/http.h"
#include "../src/common/client_types.h"
typedef int plat_sock;
#define PLAT_BAD_SOCK (-1)
static char g_id[33] = "this-pc", g_boot_id[64] = "boot", g_name[64] = "test";
static int g_hold_secs = 10;
static unsigned long long g_now = 1000000;
static int alive = 1, attaches = 0;
static plat_sock plat_connect(const char *ip, int ms) { (void)ip; (void)ms; return PLAT_BAD_SOCK; }
static int plat_send(plat_sock s, const void *b, int n) { (void)s; (void)b; (void)n; return -1; }
static int plat_recv(plat_sock s, void *b, int n) { (void)s; (void)b; (void)n; return -1; }
static void plat_close(plat_sock s) { (void)s; }
static void cl_lock(void) {}
static void cl_unlock(void) {}
static unsigned long long now_ms(void) { return g_now; }
static int plat_local_nets(LocalNet *o, int max) { (void)max; o[0].net = 0x0A000000; o[0].mask = 0xFF000000; o[0].kind = NET_HOME; return 1; }
static void plat_client_tick_begin(void) {}
static int plat_attach(const char *ip, const char *busid) { (void)ip; (void)busid; attaches++; alive = 1; return 7; }
static int plat_port_alive(int port, const char *ip, const char *busid) { (void)port; (void)ip; (void)busid; return alive; }
static void plat_detach(int port) { (void)port; }
static const char *plat_client_note(void) { return NULL; }
static void plat_tune(void) {}
static int plat_attached(int *port, char (*ip)[64], char (*busid)[32], int max) { (void)port; (void)ip; (void)busid; (void)max; return 0; }
static void plat_stop_retries(void) {}
static const char *plat_attach_error(void) { return "error"; }
static int plat_restart_needed(void) { return 0; }
static void plat_set_bootstart(int on) { (void)on; }
static void peer_label(const char *ip, char *out, size_t n, int cl_held) { (void)cl_held; snprintf(out, n, "%s", ip); }
static void logm(const char *f, ...) { va_list a; va_start(a, f); printf("  usbeam: "); vprintf(f, a); printf("\n"); va_end(a); }
static void client_save(void) {}
#include "../src/common/client.h"
static void pr_lock(void) {}
static void pr_unlock(void) {}
static void plat_random(void *buf, size_t n) { memset(buf, 7, n); }
static void plat_sock_timeout(plat_sock s, int ms) { (void)s; (void)ms; }
static int plat_secret_write(const char *data, size_t n) { (void)data; (void)n; return 0; }
static int plat_secret_read(char *buf, size_t max) { (void)buf; (void)max; return -1; }
#include "../src/common/pair.h"

static int fails;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "OK" : "FAIL", what); if (!ok) fails++; }
static Remote *host;
// The PC the device is plugged into, as this PC sees it.
static void setup(void) {
    memset(g_r, 0, sizeof g_r); memset(g_use, 0, sizeof g_use);
    host = &g_r[0];
    snprintf(host->id, sizeof host->id, "host-pc"); snprintf(host->name, sizeof host->name, "Host");
    host->naddr = 1; snprintf(host->addr[0].ip, sizeof host->addr[0].ip, "10.0.0.2"); host->addr[0].kind = NET_HOME; host->addr[0].ok = 1;
    host->via = 0; host->next_poll = ~0ULL; host->ndev = 1;
    snprintf(host->dev[0].busid, 32, "1-3"); snprintf(host->dev[0].state, 16, "in_use");
    Use *u = use_new("host-pc", "1-3"); u->port = 5; u->want = 1; u->autorc = 1; snprintf(u->ip, sizeof u->ip, "10.0.0.2");
    alive = 1; attaches = 0;
}
// The host ends the session; a moment later it lists the device as available again.
static void host_ends(const char *taken_from) {
    alive = 0; g_now += 1000; client_tick();
    snprintf(host->dev[0].state, 16, "available"); snprintf(host->dev[0].taken, 40, "%s", taken_from);
    for (int k = 0; k < 3; k++) { g_now += 3500; client_tick(); }
}
static int using_it(void) { Use *u = use_find("host-pc", "1-3"); return u && u->used; }
int main(void) {
    printf("1. Its own PC takes it back from this PC\n");
    setup(); host_ends(g_id);
    check(attaches == 0, "this PC doesn't take it straight back");
    check(!using_it(), "Auto-reconnect for it is off here");
    char body[160];
    sb_t out = {0}; int code = 200;
    snprintf(body, sizeof body, "{\"op\":\"use\",\"pc\":\"host-pc\",\"busid\":\"1-3\"}");
    client_action("use", body, &out, &code); g_now += 3500; client_tick();
    check(attaches == 1, "Use on this PC still works afterwards");
    sb_free(&out);

    printf("2. It was taken back from another PC (not this one)\n");
    setup(); host_ends("other-pc");
    check(attaches == 1, "Auto-reconnect brings it back here as usual");

    printf("3. The connection just dropped (nothing taken back)\n");
    setup(); host_ends("");
    check(attaches == 1, "Auto-reconnect brings it back here as usual");

    printf("4. Taken back long ago, then the connection drops later for another reason\n");
    setup(); snprintf(host->dev[0].taken, 40, "%s", g_id);
    alive = 0; g_now += 1000; client_tick();          // ended now; "taken_from" is old news from before
    Use *u = use_find("host-pc", "1-3"); if (u) u->ended_at = g_now - 120000;
    snprintf(host->dev[0].state, 16, "available");
    for (int k = 0; k < 3; k++) { g_now += 3500; client_tick(); }
    check(attaches == 1, "Auto-reconnect isn't blocked by an old take-back");

    printf(fails ? "Take-back test FAILED\n" : "PASS: take-back test\n");
    return fails != 0;
}
