// SPDX-License-Identifier: GPL-3.0-or-later
// Windows smooth-input test: USBeam's relay (src/windows/relay_win.h) on its own. Opens a local
// port for usbip-win2's driver that leads to the "other PC" at 127.0.0.1:3240 (tests/fakehid.c),
// prints the port, and keeps relaying for 90 s.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#define USBIP_PORT 3240
static void logm(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); fflush(stdout); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static BOOL recv_all(SOCKET s, void *b, int n) { char *p = b; while (n > 0) { int r = recv(s, p, n, 0); if (r <= 0) return FALSE; p += r; n -= r; } return TRUE; }
static BOOL send_all(SOCKET s, const void *b, int n) { const char *p = b; while (n > 0) { int r = send(s, p, n, 0); if (r <= 0) return FALSE; p += r; n -= r; } return TRUE; }
static SOCKET plat_connect(const char *ip, int ms) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD to = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return INVALID_SOCKET; }
    return s;
}
#include "../src/windows/relay_win.h"

int main(void) {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    uint16_t port = relay_open("127.0.0.1", "1-1");
    printf("PORT %u\n", (unsigned)port); fflush(stdout);
    Sleep(90000);
    return 0;
}
