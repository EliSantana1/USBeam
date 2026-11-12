// SPDX-License-Identifier: GPL-3.0-or-later
// Pairing test: two USBeam PCs (two processes, the real pairing code in src/common/pair.h,
// talking over real network connections on this PC) pair with a 6-digit code, then use what
// pairing allows. Checks: wrong codes, the 3-tries limit, pairing only while "Pair a PC" is open,
// easy codes never chosen, a forged key refused, Give To, "Only paired PCs", unpair,
// and that pairings survive a restart (saved and loaded again).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static const unsigned char ui_html[1]; static const unsigned int ui_html_len = 0;
#include "../src/common/http.h"
#include "../src/common/client_types.h"
typedef int plat_sock;
#define PLAT_BAD_SOCK (-1)
static char g_id[33], g_boot_id[64] = "boot", g_name[64];
static int g_hold_secs = 10;
static int g_port_a, g_port_b;
static const char *g_me_ip, *g_other_ip;          // this PC's and the other PC's (pretend) address
static char g_secret_path[64];
static pthread_mutex_t g_cmx = PTHREAD_MUTEX_INITIALIZER, g_pmx = PTHREAD_MUTEX_INITIALIZER;

static plat_sock plat_connect(const char *ip, int ms) {
    (void)ms;
    int port = !strcmp(ip, "10.0.0.1") ? g_port_a : !strcmp(ip, "10.0.0.2") ? g_port_b : 0;
    if (!port) return PLAT_BAD_SOCK;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return PLAT_BAD_SOCK; }
    return s;
}
static int plat_send(plat_sock s, const void *b, int n) {
    const char *p = b;
    while (n > 0) { ssize_t w = send(s, p, (size_t)n, MSG_NOSIGNAL); if (w <= 0) return -1; p += w; n -= (int)w; }
    return 0;
}
static int plat_recv(plat_sock s, void *b, int n) {
    char *p = b;
    while (n > 0) { ssize_t r = recv(s, p, (size_t)n, 0); if (r <= 0) return -1; p += r; n -= (int)r; }
    return 0;
}
static void plat_close(plat_sock s) { close(s); }
static void cl_lock(void) { pthread_mutex_lock(&g_cmx); }
static void cl_unlock(void) { pthread_mutex_unlock(&g_cmx); }
static unsigned long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000; }
static int plat_local_nets(LocalNet *o, int max) { (void)max; o[0].net = 0x0A000000; o[0].mask = 0xFF000000; o[0].kind = NET_HOME; return 1; }
static void plat_client_tick_begin(void) {}
static int plat_attach(const char *ip, const char *busid) { (void)ip; (void)busid; return -1; }
static int plat_port_alive(int port, const char *ip, const char *busid) { (void)port; (void)ip; (void)busid; return 0; }
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
static void plat_bt_tick(void) {}
#define PLAT_BT_SYNC 0
static void peer_label(const char *ip, char *out, size_t n, int cl_held) { (void)cl_held; snprintf(out, n, "%s", ip); }
static void logm(const char *f, ...) { va_list a; va_start(a, f); printf("    [%s] ", g_name); vprintf(f, a); printf("\n"); va_end(a); fflush(stdout); }
static void client_save(void) {}
#include "../src/common/client.h"
static void pr_lock(void) { pthread_mutex_lock(&g_pmx); }
static void pr_unlock(void) { pthread_mutex_unlock(&g_pmx); }
static void plat_random(void *buf, size_t n) { if (getrandom(buf, n, 0) != (ssize_t)n) abort(); }
static void plat_sock_timeout(plat_sock s, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
static int plat_secret_write(const char *data, size_t n) {
    int fd = open(g_secret_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, data, n) == (ssize_t)n;
    close(fd);
    return ok ? 0 : -1;
}
static int plat_secret_read(char *buf, size_t max) {
    int fd = open(g_secret_path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, max);
    close(fd);
    return (int)r;
}
#include "../src/common/pair.h"

static int fails;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "OK" : "FAIL", what); fflush(stdout); if (!ok) fails++; }

// Each PC answers other PCs, like USBeam's own server.
static void *server(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int s = accept(ls, NULL, NULL);
        if (s < 0) continue;
        uint8_t op[8];
        if (!plat_recv(s, op, 8)) pair_server(s, g_other_ip, cl_g16(op + 2), op);
        close(s);
    }
    return NULL;
}
static void know_other(const char *id, const char *name, const char *ip) {
    cl_lock();
    Remote *r = &g_r[0];
    snprintf(r->id, sizeof r->id, "%s", id); snprintf(r->name, sizeof r->name, "%s", name);
    r->naddr = 1; snprintf(r->addr[0].ip, sizeof r->addr[0].ip, "%s", ip); r->addr[0].kind = NET_HOME; r->addr[0].ok = 1;
    r->via = 0; r->next_poll = ~0ULL; r->pair_cap = 1;
    cl_unlock();
}
static char g_out[400];
static int act(const char *op, const char *body) {      // an action from the app window: 200 or 400
    sb_t out = {0}; int code = 200;
    char b[300]; snprintf(b, sizeof b, "{\"op\":\"%s\"%s%s}", op, body[0] ? "," : "", body);
    if (!client_action(op, b, &out, &code)) code = 404;
    snprintf(g_out, sizeof g_out, "%s", out.p ? out.p : "");
    sb_free(&out);
    return code;
}

// ---------------------------------------------------------------- the PC that shows the code (B)
static void run_b(int in, int out) {
    FILE *fi = fdopen(in, "r"), *fo = fdopen(out, "w");
    char line[200];
    while (fgets(line, sizeof line, fi)) {
        char cmd[40] = "", arg[100] = "";
        sscanf(line, "%39s %99s", cmd, arg);
        char rep[200] = "ok";
        if (!strcmp(cmd, "open")) act("pair_open", "");
        else if (!strcmp(cmd, "close")) act("pair_close", "");
        else if (!strcmp(cmd, "code")) { pr_lock(); snprintf(rep, sizeof rep, "%s", g_pm.b_active ? g_pm.b_code : "-"); pr_unlock(); }
        else if (!strcmp(cmd, "isopen")) { pr_lock(); snprintf(rep, sizeof rep, "%d", now_ms() < g_pm.open_until); pr_unlock(); }
        else if (!strcmp(cmd, "msg")) { pr_lock(); snprintf(rep, sizeof rep, "%s", g_pm.msg[0] ? g_pm.msg : "-"); pr_unlock(); }
        else if (!strcmp(cmd, "key")) {
            pr_lock(); PairRec *p = pr_find(arg);
            if (p) for (int k = 0; k < 32; k++) snprintf(rep + 2 * k, 3, "%02x", p->key[k]); else snprintf(rep, sizeof rep, "-");
            pr_unlock();
        }
        else if (!strcmp(cmd, "given")) { cl_lock(); Use *u = use_find("id-a", arg); snprintf(rep, sizeof rep, "%d", u && u->used && u->want && u->give_until); cl_unlock(); }
        else if (!strcmp(cmd, "pairedonly")) g_paired_only = atoi(arg);
        else if (!strcmp(cmd, "importok")) snprintf(rep, sizeof rep, "%d", pair_import_ok("10.0.0.1", arg));
        else if (!strcmp(cmd, "kickok")) snprintf(rep, sizeof rep, "%d", pair_kick_ok(arg[0] ? arg : "10.0.0.1"));
        else if (!strcmp(cmd, "info")) { sb_t j = {0}; pair_info_json(&j); snprintf(rep, sizeof rep, "%s", j.p); sb_free(&j); }
        else if (!strcmp(cmd, "reload")) {          // as after an update: forget everything in memory, load the file again
            pr_lock(); memset(g_pr, 0, sizeof g_pr); pr_unlock();
            pair_load();
            pr_lock(); snprintf(rep, sizeof rep, "%d", pr_find("id-a") != NULL); pr_unlock();
        }
        else if (!strcmp(cmd, "mode")) { struct stat st; snprintf(rep, sizeof rep, "%o", stat(g_secret_path, &st) ? 0 : (unsigned)(st.st_mode & 0777)); }
        else if (!strcmp(cmd, "quit")) break;
        fprintf(fo, "%s\n", rep); fflush(fo);
    }
    unlink(g_secret_path);
    _exit(0);
}

static FILE *g_to_b, *g_from_b;
static char g_brep[200];
static const char *b(const char *cmd) {
    fprintf(g_to_b, "%s\n", cmd); fflush(g_to_b);
    if (!fgets(g_brep, sizeof g_brep, g_from_b)) snprintf(g_brep, sizeof g_brep, "(no answer)");
    g_brep[strcspn(g_brep, "\n")] = 0;
    return g_brep;
}
static int listener(int *port) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0}; socklen_t al = sizeof a;
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8) || getsockname(ls, (struct sockaddr *)&a, &al)) { perror("listen"); exit(2); }
    *port = ntohs(a.sin_port);
    return ls;
}
static void wrong_code(char *out, const char *right) {     // a code that differs from the right one
    snprintf(out, 8, "%s", right);
    out[0] = out[0] == '9' ? '1' : (char)(out[0] + 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int lsa = listener(&g_port_a), lsb = listener(&g_port_b);
    int p1[2], p2[2];
    if (pipe(p1) || pipe(p2)) return 2;
    pid_t pid = fork();
    if (pid == 0) {                            // B
        close(lsa); close(p1[1]); close(p2[0]);
        snprintf(g_id, sizeof g_id, "id-b"); snprintf(g_name, sizeof g_name, "PC-B");
        g_me_ip = "10.0.0.2"; g_other_ip = "10.0.0.1";
        snprintf(g_secret_path, sizeof g_secret_path, "/tmp/usbeam-pairtest-b-%d", (int)getpid());
        know_other("id-a", "PC-A", "10.0.0.1");
        pthread_t t; pthread_create(&t, NULL, server, (void *)(intptr_t)lsb);
        run_b(p1[0], p2[1]);
    }
    close(lsb); close(p1[0]); close(p2[1]);
    g_to_b = fdopen(p1[1], "w"); g_from_b = fdopen(p2[0], "r");
    snprintf(g_id, sizeof g_id, "id-a"); snprintf(g_name, sizeof g_name, "PC-A");
    g_me_ip = "10.0.0.1"; g_other_ip = "10.0.0.2";
    snprintf(g_secret_path, sizeof g_secret_path, "/tmp/usbeam-pairtest-a-%d", (int)getpid());
    know_other("id-b", "PC-B", "10.0.0.2");
    pthread_t t; pthread_create(&t, NULL, server, (void *)(intptr_t)lsa);
    char code[8], bad[8], body[120];

    printf("1. Codes are random and never easy\n");
    int weak = 0, seen0 = 0;
    for (int i = 0; i < 20000; i++) {
        char c[8]; pr_make_code(c);
        int same = 1, up = 1, down = 1;
        for (int k = 1; k < 6; k++) { if (c[k] != c[0]) same = 0; if (c[k] != c[k - 1] + 1) up = 0; if (c[k] != c[k - 1] - 1) down = 0; }
        if (same || up || down || strlen(c) != 6) weak++;
        if (c[0] == '0') seen0++;
    }
    check(weak == 0, "no 000000, 111111, 123456, 654321 and the like in 20000 codes");
    check(seen0 > 1500 && seen0 < 2500, "every digit is used (about 1 code in 10 starts with 0)");

    printf("2. Pairing is refused unless Pair a PC is open on the other PC\n");
    snprintf(body, sizeof body, "\"pc\":\"id-b\"");
    check(act("pair_with", body) == 400 && strstr(g_out, "Open Pair a PC"), "asks to open Pair a PC on the other PC first");

    printf("3. Pairing with the right code (after one typo)\n");
    b("open");
    check(!strcmp(b("isopen"), "1"), "Pair a PC is open on PC-B");
    act("pair_open", "");
    check(act("pair_with", body) == 200, "PC-A starts pairing with PC-B");
    snprintf(code, sizeof code, "%s", b("code"));
    check(strlen(code) == 6, "PC-B shows a 6-digit code");
    wrong_code(bad, code);
    snprintf(body, sizeof body, "\"value\":\"%s\"", bad);
    check(act("pair_code", body) == 400 && strstr(g_out, "2 tries left"), "a wrong code is refused (2 tries left)");
    snprintf(body, sizeof body, "\"value\":\"%s\"", code);
    check(act("pair_code", body) == 200, "the right code pairs them");
    char ka[65] = "-";
    pr_lock(); PairRec *pa = pr_find("id-b"); if (pa) for (int k = 0; k < 32; k++) snprintf(ka + 2 * k, 3, "%02x", pa->key[k]); pr_unlock();
    check(pa != NULL, "PC-A remembers PC-B");
    check(!strcmp(b("key id-a"), ka), "both PCs have the same key");
    check(!strcmp(b("isopen"), "0"), "PC-B closed pairing after success");
    check(strstr(b("msg"), "Paired with PC-A") != NULL, "PC-B shows Paired with PC-A");
    check(!strcmp(b("mode"), "600"), "PC-B's saved pairing file is readable by its owner only");

    printf("4. Give To, and a forged key\n");
    snprintf(body, sizeof body, "\"pc\":\"id-b\",\"busid\":\"1-3\"");
    check(act("give", body) == 200, "PC-A gives device 1-3 to PC-B");
    check(!strcmp(b("given 1-3"), "1"), "PC-B is connecting it (no prompt needed)");
    pr_lock(); pa = pr_find("id-b"); pa->key[5] ^= 1; pr_unlock();
    snprintf(body, sizeof body, "\"pc\":\"id-b\",\"busid\":\"1-4\"");
    check(act("give", body) == 400 && !strcmp(b("given 1-4"), "0"), "a wrong key is refused");
    pr_lock(); pa = pr_find("id-b"); pa->key[5] ^= 1; pr_unlock();

    printf("5. Only paired PCs can use my devices\n");
    b("pairedonly 1");
    check(strstr(b("info"), "\"paired_only\":true") != NULL, "PC-B tells other PCs");
    check(!strcmp(b("importok 2-1"), "0"), "a connection nobody asked for is refused");
    check(!strcmp(b("kickok 10.0.0.9"), "0"), "an unpaired PC may not release devices");
    pair_before_attach("id-b", "10.0.0.2", "2-1");
    check(!strcmp(b("importok 2-1"), "1"), "after a paired PC asked for it, it may connect");
    check(!strcmp(b("importok 2-2"), "0"), "only that device");
    check(!strcmp(b("kickok 10.0.0.1"), "1"), "the paired PC may release devices");
    b("pairedonly 0");
    check(!strcmp(b("importok 9-9"), "1"), "switched off: every PC may connect as before");

    printf("6. Pairings survive a restart or update\n");
    check(!strcmp(b("reload"), "1"), "PC-B still knows PC-A after loading its file again");
    snprintf(body, sizeof body, "\"pc\":\"id-b\",\"busid\":\"1-5\"");
    check(act("give", body) == 200, "and Give To still works with the loaded key");

    printf("7. Three wrong codes end the pairing\n");
    b("open"); act("pair_open", "");
    snprintf(body, sizeof body, "\"pc\":\"id-b\"");
    check(act("pair_with", body) == 200, "a new pairing starts");
    snprintf(code, sizeof code, "%s", b("code"));
    wrong_code(bad, code);
    snprintf(body, sizeof body, "\"value\":\"%s\"", bad);
    act("pair_code", body); act("pair_code", body);
    check(act("pair_code", body) == 400 && strstr(g_out, "3 times"), "the third wrong code stops it");
    snprintf(body, sizeof body, "\"value\":\"%s\"", code);
    check(act("pair_code", body) == 400, "the right code doesn't work after that");
    check(!strcmp(b("isopen"), "0"), "PC-B closed Pair a PC (needs opening again by hand)");
    check(!strcmp(b("key id-a"), ka), "the existing pairing is untouched");

    printf("8. A broken key exchange is refused\n");
    b("open"); act("pair_open", "");
    snprintf(body, sizeof body, "\"pc\":\"id-b\"");
    act("pair_with", body);
    uint8_t m[1 + 16 + 32] = {0}, rep[65]; uint32_t got = 0;
    m[0] = 2; pr_lock(); memcpy(m + 1, g_pm.a_sid, 16); pr_unlock();      // all-zero point
    plat_sock s = plat_connect("10.0.0.2", 1000);
    int st = (s != PLAT_BAD_SOCK && !pr_exchange(s, OP_REQ_PAIR, m, sizeof m, OP_REP_PAIR, rep, sizeof rep, &got)) ? rep[0] : -1;
    if (s != PLAT_BAD_SOCK) close(s);
    check(st == 3, "a zero (low-order) point ends the attempt");
    act("pair_close", ""); b("close");

    printf("9. Unpair\n");
    snprintf(body, sizeof body, "\"pc\":\"id-b\"");
    check(act("unpair", body) == 200 && !pair_is_paired("id-b"), "PC-A forgets PC-B");
    check(!strcmp(b("key id-a"), "-"), "PC-B forgets PC-A too");
    snprintf(body, sizeof body, "\"pc\":\"id-b\",\"busid\":\"1-6\"");
    check(act("give", body) == 400 && strstr(g_out, "Pair with that PC first"), "Give To needs pairing again");

    b("quit");
    waitpid(pid, NULL, 0);
    unlink(g_secret_path);
    printf(fails ? "FAIL: pairing test (%d)\n" : "PASS: pairing test\n", fails);
    return fails ? 1 : 0;
}
