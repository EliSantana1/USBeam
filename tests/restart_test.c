// SPDX-License-Identifier: GPL-3.0-or-later
// Restart test: a device that restarts itself right after this PC connected it (a Bluetooth chip
// reloading its firmware drops off USB for a moment) is connected again; a device whose PC stops
// keeping it (released, unplugged, countdown, an older USBeam) is not grabbed again; a device that
// keeps restarting is given up on; a connection that ends later is handled as before.
// Runs the shared client logic (client.h) with a stand-in platform.
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
static int alive = 1, attaches = 0, attach_ok = 1;
static plat_sock plat_connect(const char *ip, int ms) { (void)ip; (void)ms; return PLAT_BAD_SOCK; }
static int plat_send(plat_sock s, const void *b, int n) { (void)s; (void)b; (void)n; return -1; }
static int plat_recv(plat_sock s, void *b, int n) { (void)s; (void)b; (void)n; return -1; }
static void plat_close(plat_sock s) { (void)s; }
static void cl_lock(void) {}
static void cl_unlock(void) {}
static unsigned long long now_ms(void) { return g_now; }
static int plat_local_nets(LocalNet *o, int max) { (void)max; o[0].net = 0x0A000000; o[0].mask = 0xFF000000; o[0].kind = NET_HOME; return 1; }
static void plat_client_tick_begin(void) {}
static int plat_attach(const char *ip, const char *busid) { (void)ip; (void)busid; attaches++; if (!attach_ok) return -1; alive = 1; return 7; }
static int plat_port_alive(int port, const char *ip, const char *busid) { (void)port; (void)ip; (void)busid; return alive; }
static void plat_detach(int port) { (void)port; }
static const char *plat_client_note(void) { return NULL; }
static void plat_tune(void) {}
static int plat_attached(int *port, char (*ip)[64], char (*busid)[32], int max) { (void)port; (void)ip; (void)busid; (void)max; return 0; }
static void plat_stop_retries(void) {}
static const char *plat_attach_error(void) { return "error"; }
static int plat_restart_needed(void) { return 0; }
static void plat_set_bootstart(int on) { (void)on; }
static void plat_bt_before(const char *pc, const char *ip, const char *busid, int again) { (void)pc; (void)ip; (void)busid; (void)again; }
static void plat_bt_after(const char *pc, const char *busid) { (void)pc; (void)busid; }
static void plat_bt_leaving(const char *pc, const char *busid) { (void)pc; (void)busid; }
static void plat_bt_tick(void) {}
#define PLAT_BT_SYNC 0
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
static void listed_as(const char *state);
// The PC the device is plugged into, as this PC sees it: the device was just connected here.
static void setup(int autorc) {
    memset(g_r, 0, sizeof g_r); memset(g_use, 0, sizeof g_use);
    host = &g_r[0];
    snprintf(host->id, sizeof host->id, "host-pc"); snprintf(host->name, sizeof host->name, "Host");
    host->naddr = 1; snprintf(host->addr[0].ip, sizeof host->addr[0].ip, "10.0.0.2"); host->addr[0].kind = NET_HOME; host->addr[0].ok = 1;
    host->via = 0; host->next_poll = ~0ULL;
    listed_as("in_use");
    Use *u = use_new("host-pc", "1-3"); u->port = 5; u->want = 1; u->autorc = autorc; u->attached_at = g_now;
    snprintf(u->ip, sizeof u->ip, "10.0.0.2");
    alive = 1; attaches = 0; attach_ok = 1;
}
static void listed_as(const char *state) {   // NULL: not listed (off the bus)
    host->ndev = state ? 1 : 0;
    snprintf(host->dev[0].busid, 32, "1-3"); snprintf(host->dev[0].state, 16, "%s", state ? state : "");
}
static void wait_s(double s) { for (double t = 0; t < s; t += 0.5) { g_now += 500; client_tick(); } }
static Use *use(void) { return use_find("host-pc", "1-3"); }
static int using_it(void) { Use *u = use(); return u && u->used; }
static int connected(void) { Use *u = use(); return u && u->used && u->port >= 0; }
static int error_shown(void) { return fail_get("host-pc", "1-3", g_now) != NULL; }

int main(void) {
    printf("1. It restarts right after connecting; its PC keeps it for this PC\n");
    setup(0);
    g_now += 2000; alive = 0; listed_as(NULL); attach_ok = 0;
    wait_s(3);
    check(using_it() && !connected(), "this PC keeps wanting it while it's off the bus");
    check(!error_shown(), "no error is shown meanwhile");
    listed_as("in_use"); attach_ok = 1;
    wait_s(3);
    check(connected(), "connected again once it's back");

    printf("2. It restarts right after connecting, but its PC stops keeping it (countdown)\n");
    setup(0);
    g_now += 2000; alive = 0; listed_as("countdown"); attach_ok = 1;
    wait_s(25);
    check(attaches == 0, "this PC doesn't grab it again");
    check(!using_it(), "and lets it go");

    printf("3. Same with Auto-reconnect on: Auto-reconnect works as before\n");
    setup(1);
    g_now += 2000; alive = 0; listed_as("countdown");
    wait_s(5);
    check(attaches == 0, "not grabbed during the countdown");
    listed_as("available");
    wait_s(5);
    check(connected(), "Auto-reconnect connects it once it's available");

    printf("4. An older USBeam on its PC: released, then available to everyone\n");
    setup(0);
    g_now += 2000; alive = 0; listed_as("available");
    wait_s(25);
    check(attaches == 0, "this PC doesn't grab it again");
    check(!using_it(), "and lets it go");

    printf("5. The connection ends long after connecting (as before this change)\n");
    setup(0);
    g_now += 30000; alive = 0; listed_as("in_use");
    wait_s(25);
    check(attaches == 0 && !using_it(), "a one-off Use is let go, not connected again");

    printf("6. It keeps restarting every time\n");
    setup(0);
    for (int k = 0; k < 6; k++) { g_now += 2000; alive = 0; listed_as("in_use"); wait_s(3); }
    wait_s(25);
    check(attaches == RESTART_TRIES, "connected again only a few times");
    check(!using_it(), "then given up on");

    printf("7. It restarts and never comes back\n");
    setup(0);
    g_now += 2000; alive = 0; listed_as(NULL); attach_ok = 0;
    wait_s(25);
    check(!using_it(), "given up on after a while");
    check(!error_shown(), "without an error message");

    printf("8. Release clicked here right after connecting\n");
    setup(0);
    g_now += 2000; use()->release = 1;
    wait_s(1); alive = 0; wait_s(25);
    check(attaches == 0 && !using_it(), "released, not connected again");

    printf(fails ? "Restart test FAILED\n" : "PASS: restart test\n");
    return fails != 0;
}
