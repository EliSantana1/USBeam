// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// http.h - tiny local web server for the USBeam app window, plus small JSON
// helpers. Header-only; include once per program after the platform socket
// headers. Listens on 127.0.0.1 only and requires a per-run secret token, so
// other computers and web pages can't control USBeam.

#ifndef USBEAM_HTTP_H
#define USBEAM_HTTP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef _WIN32
typedef SOCKET ub_sock;
#define UB_BAD_SOCK INVALID_SOCKET
#define ub_closesock closesocket
#else
typedef int ub_sock;
#define UB_BAD_SOCK (-1)
#define ub_closesock close
#endif

#define UI_PORT 3242

// ---------------------------------------------------------------- string builder
typedef struct { char *p; size_t len, cap; } sb_t;

static void sb_add(sb_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        b->p = realloc(b->p, nc); b->cap = nc;
    }
    memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void sb_printf(sb_t *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    sb_add(b, tmp, (size_t)n);
}
// Append a JSON string literal (with quotes), escaping as needed. Input is UTF-8.
static void sb_jstr(sb_t *b, const char *s) {
    sb_add(b, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sb_add(b, e, 2); }
        else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); sb_add(b, e, 6); }
        else sb_add(b, (const char *)&c, 1);
    }
    sb_add(b, "\"", 1);
}
static void sb_free(sb_t *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

// ---------------------------------------------------------------- tiny JSON reading (flat objects only)
static const char *json_find(const char *js, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}
static int json_str(const char *js, const char *key, char *out, size_t n) {
    const char *p = json_find(js, key);
    out[0] = 0;
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}
static int json_bool(const char *js, const char *key) {
    const char *p = json_find(js, key);
    return p && !strncmp(p, "true", 4);
}

// ---------------------------------------------------------------- server
static char g_ui_token[40];

static void ui_make_token(void) {
    unsigned char r[16];
#ifdef _WIN32
    HCRYPTPROV prov;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) { CryptGenRandom(prov, sizeof r, r); CryptReleaseContext(prov, 0); }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(r, 1, sizeof r, f) != sizeof r) memset(r, 7, sizeof r); fclose(f); }
#endif
    for (int i = 0; i < 16; i++) snprintf(g_ui_token + 2 * i, 3, "%02x", r[i]);
}

static ub_sock ui_listen(void) {
    ub_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == UB_BAD_SOCK) return s;
#ifdef _WIN32
    BOOL excl = TRUE; setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&excl, sizeof excl);
#else
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(UI_PORT);
    a.sin_addr.s_addr = htonl(0x7F000001);          // 127.0.0.1 only
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { ub_closesock(s); return UB_BAD_SOCK; }
    return s;
}

static void http_send(ub_sock c, int code, const char *ctype, const char *body, size_t n) {
    char h[512];
    const char *msg = code == 200 ? "OK" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 400 ? "Bad Request" : "Error";
    int hn = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
        code, msg, ctype, n);
    send(c, h, hn, 0);
    size_t off = 0;
    while (off < n) { int r = send(c, body + off, (int)(n - off), 0); if (r <= 0) break; off += (size_t)r; }
}

// Handler: fill `out` with a JSON body; return HTTP status.
typedef int (*ui_api_fn)(const char *method, const char *path, const char *body, sb_t *out);

extern const unsigned char ui_html[];
extern const unsigned int ui_html_len;

// Case-insensitive header lookup within the request head. Returns value length or -1.
static int http_header(const char *req, int head_len, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    const char *p = req, *end = req + head_len;
    while (p < end) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > end) break;
        if ((size_t)(eol - p) > nl + 1 && p[nl] == ':') {
            size_t i = 0;
            while (i < nl && tolower((unsigned char)p[i]) == tolower((unsigned char)name[i])) i++;
            if (i == nl) {
                const char *v = p + nl + 1;
                while (*v == ' ') v++;
                size_t vl = (size_t)(eol - v);
                if (vl >= n) vl = n - 1;
                memcpy(out, v, vl); out[vl] = 0;
                return (int)vl;
            }
        }
        p = eol + 2;
    }
    out[0] = 0;
    return -1;
}

// Serve one connection (blocking, short-lived).
static void ui_serve(ub_sock c, ui_api_fn api) {
    char buf[65536];
    int got = 0, hdr_end = -1;
    while (got < (int)sizeof buf - 1) {
        int r = recv(c, buf + got, (int)sizeof buf - 1 - got, 0);
        if (r <= 0) break;
        got += r; buf[got] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) {
            hdr_end = (int)(e - buf) + 4;
            char clv[32];
            int need = http_header(buf, hdr_end, "Content-Length", clv, sizeof clv) > 0 ? atoi(clv) : 0;
            if (need < 0 || need > (int)sizeof buf - 1 - hdr_end) { http_send(c, 400, "text/plain", "", 0); return; }
            if (got >= hdr_end + need) break;
        }
    }
    if (hdr_end < 0) return;
    char method[8] = "", path[512] = "";
    sscanf(buf, "%7s %511s", method, path);

    // Only accept requests addressed to 127.0.0.1/localhost (blocks DNS-rebinding tricks).
    char host[128], want1[64], want2[64];
    http_header(buf, hdr_end, "Host", host, sizeof host);
    snprintf(want1, sizeof want1, "127.0.0.1:%d", UI_PORT);
    snprintf(want2, sizeof want2, "localhost:%d", UI_PORT);
    if (strcmp(host, want1) && strcmp(host, want2)) { http_send(c, 403, "text/plain", "forbidden", 9); return; }

    if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strncmp(path, "/?", 2))) {
        http_send(c, 200, "text/html; charset=utf-8", (const char *)ui_html, ui_html_len);
        return;
    }
    if (!strncmp(path, "/api/", 5)) {
        char tok[80];
        http_header(buf, hdr_end, "X-USBeam-Token", tok, sizeof tok);
        if (!g_ui_token[0] || strcmp(tok, g_ui_token)) { http_send(c, 403, "application/json", "{\"error\":\"bad token\"}", 21); return; }
        sb_t out = {0};
        int code = api(method, path, buf + hdr_end, &out);
        if (!out.p) sb_add(&out, "{}", 2);
        http_send(c, code, "application/json", out.p, out.len);
        sb_free(&out);
        return;
    }
    http_send(c, 404, "text/plain", "not found", 9);
}

// Random 32-hex-character id for this installation (identifies one PC across addresses).
static void make_random_id(char out[33]) {
    char save[40]; memcpy(save, g_ui_token, sizeof save);
    ui_make_token();
    memcpy(out, g_ui_token, 33);
    memcpy(g_ui_token, save, sizeof save);
}


#endif
