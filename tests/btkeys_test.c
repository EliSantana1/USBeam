// SPDX-License-Identifier: GPL-3.0-or-later
// Bluetooth pairings test (src/linux/btkeys.h): a Bluetooth adapter's pairings go with it from the
// Linux PC it's plugged into to another paired Linux PC and come back with what changed there:
// new, changed and removed pairings, exactly. Wrong pair keys, other PCs, broken or hostile data
// are refused without touching anything. Two "PCs" here: this process uses the adapter, a child
// process (its own pairings folder, over a real local connection) is the PC it's plugged into.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static const unsigned char ui_html[1]; static const unsigned int ui_html_len = 0;
#include "../src/common/http.h"
#include "../src/common/client_types.h"
typedef int plat_sock;
#define PLAT_BAD_SOCK (-1)
static char g_id[33] = "0123456789abcdef0123456789abcdef", g_boot_id[64] = "boot", g_name[64] = "test";
static int g_hold_secs = 10;
static int g_port;                       // the child "PC"'s port on 127.0.0.1
static unsigned long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static plat_sock plat_connect(const char *ip, int ms) {
    (void)ip; (void)ms;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return PLAT_BAD_SOCK; }
    return s;
}
static int plat_send(plat_sock s, const void *b, int n) { const char *p = b; while (n > 0) { ssize_t r = send(s, p, (size_t)n, MSG_NOSIGNAL); if (r <= 0) return -1; p += r; n -= (int)r; } return 0; }
static int plat_recv(plat_sock s, void *b, int n) { char *p = b; while (n > 0) { ssize_t r = recv(s, p, (size_t)n, 0); if (r <= 0) return -1; p += r; n -= (int)r; } return 0; }
static void plat_close(plat_sock s) { close(s); }
static void cl_lock(void) {}
static void cl_unlock(void) {}
static int plat_local_nets(LocalNet *o, int max) { (void)max; o[0].net = 0x7F000000; o[0].mask = 0xFF000000; o[0].kind = NET_HOME; return 1; }
static void plat_client_tick_begin(void) {}
static int plat_attach(const char *ip, const char *busid) { (void)ip; (void)busid; return 7; }
static int plat_port_alive(int port, const char *ip, const char *busid) { (void)port; (void)ip; (void)busid; return 0; }
static void plat_detach(int port) { (void)port; }
static const char *plat_client_note(void) { return NULL; }
static void plat_tune(void) {}
static int plat_attached(int *port, char (*ip)[64], char (*busid)[32], int max) { (void)port; (void)ip; (void)busid; (void)max; return 0; }
static void plat_stop_retries(void) {}
static const char *plat_attach_error(void) { return "error"; }
static int plat_restart_needed(void) { return 0; }
static void plat_set_bootstart(int on) { (void)on; }
static void plat_bt_before(const char *pc, const char *ip, const char *busid, int again);
static void plat_bt_after(const char *pc, const char *busid);
static void plat_bt_leaving(const char *pc, const char *busid);
static void plat_bt_tick(void);
#define PLAT_BT_SYNC 1
static void peer_label(const char *ip, char *out, size_t n, int cl_held) { (void)cl_held; snprintf(out, n, "%s", ip); }
static void logm(const char *f, ...) { va_list a; va_start(a, f); printf("  usbeam: "); vprintf(f, a); printf("\n"); va_end(a); }
static void client_save(void) {}
#include "../src/common/client.h"
static void pr_lock(void) {}
static void pr_unlock(void) {}
static void plat_random(void *buf, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (!f || fread(buf, 1, n, f) != n) abort(); fclose(f); }
static void plat_sock_timeout(plat_sock s, int ms) { struct timeval tv = { ms / 1000, (ms % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
static int plat_secret_write(const char *data, size_t n) { (void)data; (void)n; return 0; }
static int plat_secret_read(char *buf, size_t max) { (void)buf; (void)max; return -1; }
#include "../src/common/pair.h"

// ---- the PC the adapter is plugged into, as btkeys.h sees it
#define USBDEV "/nonexistent"
#define MAXH 8
typedef struct { char busid[32]; int exported; char peer[64]; time_t restart_until, reserve_until; char btaddr[18]; char bt_peer[40]; time_t bt_wait_until; } HostDev;
static HostDev g_h[MAXH];
static HostDev *hdev(const char *busid, int create) {
    for (int i = 0; i < MAXH; i++) if (g_h[i].busid[0] && !strcmp(g_h[i].busid, busid)) return &g_h[i];
    if (!create) return NULL;
    for (int i = 0; i < MAXH; i++) if (!g_h[i].busid[0]) { memset(&g_h[i], 0, sizeof g_h[i]); snprintf(g_h[i].busid, 32, "%s", busid); return &g_h[i]; }
    return NULL;
}
static int host_held(const HostDev *h) { return h && (h->restart_until || h->reserve_until); }
static int dev_present(const char *b) { (void)b; return 1; }
static int shareable(const char *b) { (void)b; return 1; }
static int offerable(const char *b) { (void)b; return 1; }
static int bound_to_stub(const char *b) { (void)b; return 0; }
static int g_rebinds;
static int write_sysfs(const char *p, const char *v) { (void)v; if (strstr(p, "drivers_probe")) g_rebinds++; return 0; }
static int dev_ifaces(const char *b, unsigned char ifc[][3], int max) { (void)b; (void)max; ifc[0][0] = 0xe0; ifc[0][1] = 1; ifc[0][2] = 1; return 1; }
static void host_lock(void) {}
static void host_unlock(void) {}
#include "../src/linux/btkeys.h"

static int fails;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "OK" : "FAIL", what); if (!ok) fails++; }
#define ADDR "B0:82:E2:27:40:6F"
#define DEV1 "50:5E:5C:EC:CD:A2"
#define DEV2 "4C:43:F6:DD:99:A8"
#define DEV3 "11:22:33:44:55:66"
static char root[PATH_MAX], hostdir[PATH_MAX], usedir[PATH_MAX];

static void put(const char *store, const char *dev, const char *text) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/" ADDR, store); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/" ADDR "/%s", store, dev); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/" ADDR "/%s/info", store, dev);
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}
static int has(const char *store, const char *dev, const char *text) {
    char p[PATH_MAX], b[256] = ""; snprintf(p, sizeof p, "%s/" ADDR "/%s/info", store, dev);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return !strcmp(b, text);
}
static int exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }

// The child "PC": serves requests until told to stop; its state is kept between requests.
static uint8_t g_host_key[32];
static void run_host(int ls) {
    snprintf(g_bt_store, sizeof g_bt_store, "%s", hostdir);
    memset(g_pr, 0, sizeof g_pr);
    snprintf(g_pr[0].id, sizeof g_pr[0].id, "%s", g_id); memcpy(g_pr[0].key, g_host_key, 32);
    HostDev *h = hdev("1-2", 1); snprintf(h->btaddr, sizeof h->btaddr, ADDR);
    BtConn *c = bt_conn(ADDR, 1); c->nconn = 1; snprintf(c->conn[0], 18, DEV1);   // the headset is connected there
    for (;;) {
        int s = accept(ls, NULL, NULL);
        if (s < 0) continue;
        uint8_t op[8];
        if (!plat_recv(s, op, 8)) {
            uint16_t code = (uint16_t)(op[2] << 8 | op[3]);
            if (code == OP_REQ_BT) bt_server(s, "127.0.0.1", op);
            else if (code == 0x7777) { close(s); _exit(0); }
            else if (code == 0x7778) {                 // test only: how the host side stands
                char line[128]; snprintf(line, sizeof line, "%s|%d", h->bt_peer, g_rebinds);
                plat_send(s, line, (int)strlen(line) + 1);
            } else if (code == 0x7779) h->bt_wait_until = 0, h->bt_peer[0] = 0;   // test only: forget who has them
        }
        close(s);
    }
}
static void host_cmd(uint16_t code, char *out, size_t n) {
    int s = plat_connect("127.0.0.1", 1000); uint8_t op[8] = {0}; op[2] = (uint8_t)(code >> 8); op[3] = (uint8_t)code;
    plat_send(s, op, 8);
    if (out) { size_t k = 0; while (k + 1 < n && recv(s, out + k, 1, 0) == 1 && out[k]) k++; out[k] = 0; }
    close(s);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/usbeam-bttest-%d", (int)getpid());
    snprintf(hostdir, sizeof hostdir, "%s/host", root); snprintf(usedir, sizeof usedir, "%s/use", root);
    mkdir(root, 0700); mkdir(hostdir, 0700); mkdir(usedir, 0700);
    snprintf(g_bt_store, sizeof g_bt_store, "%s", usedir);
    snprintf(g_bt_class, sizeof g_bt_class, "%s/no-adapters", root);   // never this PC's own Bluetooth adapters
    plat_random(g_host_key, 32);
    // This PC knows the other one by the same id here (one process, one id), with the same pair key.
    snprintf(g_pr[0].id, sizeof g_pr[0].id, "%s", g_id); memcpy(g_pr[0].key, g_host_key, 32);
    Remote *r = &g_r[0];
    snprintf(r->id, sizeof r->id, "%s", g_id); r->naddr = 1; snprintf(r->addr[0].ip, 64, "127.0.0.1"); r->addr[0].ok = 1; r->via = 0;

    put(hostdir, DEV1, "[LinkKey]\nKey=AAAA\n"); put(hostdir, DEV2, "[LinkKey]\nKey=BBBB\n");
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8) || getsockname(ls, (struct sockaddr *)&a, &al)) { perror("listen"); return 1; }
    g_port = ntohs(a.sin_port);
    pid_t child = fork();
    if (child == 0) run_host(ls);
    close(ls);
    char st[128];

    printf("1. Using the adapter: its pairings arrive first, exactly\n");
    plat_bt_before(g_id, "127.0.0.1", "1-2", 0);
    check(has(usedir, DEV1, "[LinkKey]\nKey=AAAA\n") && has(usedir, DEV2, "[LinkKey]\nKey=BBBB\n"), "both pairings are in place here");
    host_cmd(0x7778, st, sizeof st);
    check(!strncmp(st, g_id, strlen(g_id)), "its PC knows which PC has them");
    BtConn *bc = bt_conn(ADDR, 0);
    check(bc && bc->nwant == 1 && !strcmp(bc->want[0], DEV1), "the device connected there is to be connected here too");

    printf("2. While here: one pairing changed, one removed, one new\n");
    put(usedir, DEV1, "[LinkKey]\nKey=CCCC\n");
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/" ADDR "/" DEV2, usedir); bt_rmdir_all(p);
    put(usedir, DEV3, "[LinkKey]\nKey=DDDD\n");
    plat_bt_after(g_id, "1-2");
    plat_bt_tick();
    check(has(hostdir, DEV1, "[LinkKey]\nKey=CCCC\n"), "the changed pairing is back on its PC");
    snprintf(p, sizeof p, "%s/" ADDR "/" DEV2, hostdir);
    check(!exists(p), "the removed pairing is removed there too");
    check(has(hostdir, DEV3, "[LinkKey]\nKey=DDDD\n"), "the new pairing is there");
    snprintf(p, sizeof p, "%s/" ADDR, usedir);
    check(!exists(p), "this PC's copy is gone");
    host_cmd(0x7778, st, sizeof st);
    check(st[0] == '|', "its PC expects nothing more from this PC");
    check(g_bts[0].state == BTS_NONE, "nothing left to send");
    check(!strcmp(strchr(st, '|') + 1, "1"), "its PC (adapter already back there) re-reads its pairings once");

    printf("3. Sending again when nothing is expected: told to stop, nothing changes\n");
    put(usedir, DEV2, "[LinkKey]\nKey=EVIL\n");
    BtSess *b = bt_sess(g_id, "1-2", 1); b->state = BTS_SEND; snprintf(b->pc, 40, "%s", g_id); snprintf(b->busid, 32, "1-2"); snprintf(b->addr, 18, ADDR);
    plat_bt_tick();
    snprintf(p, sizeof p, "%s/" ADDR "/" DEV2, hostdir);
    check(!exists(p), "its PC didn't take them");
    check(g_bts[0].state == BTS_NONE, "this PC stopped trying");

    printf("4. A PC with the wrong pair key gets nothing and changes nothing\n");
    uint8_t good[32]; memcpy(good, g_pr[0].key, 32); g_pr[0].key[0] ^= 1;
    bt_forget(ADDR);
    plat_bt_before(g_id, "127.0.0.1", "1-2", 0);
    snprintf(p, sizeof p, "%s/" ADDR, usedir);
    check(!exists(p), "no pairings came");
    sb_t set = {0}; bt_pack(ADDR, &set);
    int r4 = bt_request(g_id, "127.0.0.1", BTC_PUT, "1-2", &set, NULL, NULL); sb_free(&set);
    check(r4 == 2, "sending pairings is refused");
    check(has(hostdir, DEV1, "[LinkKey]\nKey=CCCC\n") && has(hostdir, DEV3, "[LinkKey]\nKey=DDDD\n"), "its PC's pairings are untouched");
    memcpy(g_pr[0].key, good, 32);

    printf("5. Not paired: nothing at all\n");
    PairRec keep = g_pr[0]; memset(&g_pr[0], 0, sizeof g_pr[0]);
    uint8_t *blob = NULL; size_t bn = 0;
    check(bt_request(g_id, "127.0.0.1", BTC_GET, "1-2", NULL, &blob, &bn) == 1 && !blob, "a PC that isn't paired here can't ask");
    g_pr[0] = keep;

    printf("6. Adapter restarts while here: its pairings aren't fetched again\n");
    plat_bt_before(g_id, "127.0.0.1", "1-2", 0);
    put(usedir, DEV3, "[LinkKey]\nKey=FFFF\n");
    plat_bt_before(g_id, "127.0.0.1", "1-2", 1);
    check(has(usedir, DEV3, "[LinkKey]\nKey=FFFF\n"), "what changed here meanwhile is kept");

    printf("7. Couldn't send them back before using it again: they're used, not overwritten\n");
    plat_bt_after(g_id, "1-2");
    int saved = g_port; g_port = 1;                    // its PC can't be reached
    plat_bt_tick();
    check(g_bts[0].state == BTS_SEND, "still to be sent");
    plat_bt_before(g_id, "127.0.0.1", "1-2", 0);
    check(has(usedir, DEV3, "[LinkKey]\nKey=FFFF\n"), "this PC's newer pairings stay");
    g_port = saved;
    plat_bt_after(g_id, "1-2"); plat_bt_tick();
    check(has(hostdir, DEV3, "[LinkKey]\nKey=FFFF\n"), "and reach its PC once it can be reached");

    printf("8. Kept through a restart of this PC\n");
    plat_bt_before(g_id, "127.0.0.1", "1-2", 0);
    char *buf = NULL; size_t bl = 0; FILE *mf = open_memstream(&buf, &bl);
    bt_save_lines(mf); fclose(mf);
    memset(g_bts, 0, sizeof g_bts);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) bt_load_line(line);
    free(buf);
    check(g_bts[0].state == BTS_SEND && !strcmp(g_bts[0].addr, ADDR), "after a restart its pairings still go back");
    plat_bt_tick();
    check(g_bts[0].state == BTS_NONE, "and they do");

    printf("9. Broken or hostile data is refused whole\n");
    put(usedir, DEV3, "[LinkKey]\nKey=FFFF\n");
    sb_t s9 = {0}; bt_pack(ADDR, &s9);
    int ch = 0;
    check(bt_apply(ADDR, (const uint8_t *)s9.p, s9.len, &ch) == 0, "a good set is taken");
    check(bt_apply("AA:BB:CC:DD:EE:FF", (const uint8_t *)s9.p, s9.len, &ch) != 0, "a set for another adapter is refused");
    check(bt_apply(ADDR, (const uint8_t *)s9.p, s9.len - 1, &ch) != 0, "a cut-off set is refused");
    sb_t bad = {0}; uint8_t l[4];
sb_add(&bad, "USBEAMBT1", 9); sb_add(&bad, ADDR, 17); sb_add(&bad, "\0", 1); sb_add(&bad, "../../../etc/xxxx", 17); bt_put32(l, 4); sb_add(&bad, (const char *)l, 4); sb_add(&bad, "evil", 4);
    check(bt_apply(ADDR, (const uint8_t *)bad.p, bad.len, &ch) != 0, "a device name that is a path is refused");
    sb_free(&bad);
    sb_add(&bad, "USBEAMBT1", 9); sb_add(&bad, ADDR, 17); sb_add(&bad, "\0", 1); sb_add(&bad, DEV1, 17); bt_put32(l, 3); sb_add(&bad, (const char *)l, 4); sb_add(&bad, "a\0b", 3);
    check(bt_apply(ADDR, (const uint8_t *)bad.p, bad.len, &ch) != 0, "binary data is refused");
    sb_free(&bad);
    sb_add(&bad, "USBEAMBT1", 9); sb_add(&bad, ADDR, 17); sb_add(&bad, "\0", 1); sb_add(&bad, DEV1, 17); bt_put32(l, 0x7FFFFFFF); sb_add(&bad, (const char *)l, 4);
    check(bt_apply(ADDR, (const uint8_t *)bad.p, bad.len, &ch) != 0, "an impossible size is refused");
    sb_free(&bad);
    sb_add(&bad, "USBEAMBT1", 9); sb_add(&bad, ADDR, 17); sb_add(&bad, "\xC8", 1);
    check(bt_apply(ADDR, (const uint8_t *)bad.p, bad.len, &ch) != 0, "too many connected devices is refused");
    sb_free(&bad);
    sb_add(&bad, "USBEAMBT1", 9); sb_add(&bad, ADDR, 17); sb_add(&bad, "\x01", 1); sb_add(&bad, "../../../etc/xxxx", 17);
    check(bt_apply(ADDR, (const uint8_t *)bad.p, bad.len, &ch) != 0, "a connected device that isn't an address is refused");
    sb_free(&bad); sb_free(&s9);
    check(has(usedir, DEV3, "[LinkKey]\nKey=FFFF\n"), "nothing was changed by them");
    check(!bt_valid_busid("1-2/../x") && !bt_valid_busid("../1-2") && !bt_valid_busid("1-.2") && !bt_valid_busid("1-2.") && bt_valid_busid("1-11.4"), "only real bus ids are accepted");
    check(!bt_valid_addr("b0:82:e2:27:40:6f") && !bt_valid_addr(ADDR "0") && bt_valid_addr(ADDR), "only real adapter addresses are accepted");

    host_cmd(0x7777, NULL, 0);
    waitpid(child, NULL, 0);
    char cmd[PATH_MAX + 16]; snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) printf("(couldn't remove %s)\n", root);
    printf(fails ? "Bluetooth pairings test FAILED\n" : "PASS: Bluetooth pairings test\n");
    return fails != 0;
}
