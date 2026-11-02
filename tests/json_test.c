// SPDX-License-Identifier: GPL-3.0-or-later
// JSON reading test: values are found by their key, also when the same word appears as a value
// first (a device counting down sends "state":"countdown" before "countdown":7).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static const unsigned char ui_html[1]; static const unsigned int ui_html_len = 0;   // (the page itself isn't needed here)
#include "../src/common/http.h"
static int fails;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "OK" : "FAIL", what); if (!ok) fails++; }
int main(void) {
    const char *d = "{\"busid\":\"1-3\",\"state\":\"countdown\",\"countdown\":7,\"input\":true,\"peer\":\"\"}";
    const char *c = json_find(d, "countdown");
    check(c && atoi(c) == 7, "countdown after \"state\":\"countdown\" reads 7");
    char st[16]; json_str(d, "state", st, sizeof st);
    check(!strcmp(st, "countdown"), "state reads countdown");
    check(json_bool(d, "input"), "a true value reads true");
    check(json_find(d, "missing") == NULL, "a missing key reads nothing");
    const char *sp = json_find("{\"a\":\"name\",\"name\" : 5}", "name");
    check(sp && sp[0] == '5', "a key with spaces before ':' is found after the same word as a value");
    printf(fails ? "JSON test FAILED\n" : "PASS: JSON test\n");
    return fails != 0;
}
