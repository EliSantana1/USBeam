// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// netdrop.c - network-drop test helper (run by tests/netdrop.sh, as root, in two network namespaces).
// Both ends use USBeam's own connection settings (src/linux/netsock.h) and keep sending small
// messages, like a game controller in use. When the network goes dead, each end must notice
// within ~20 s and report how long it took.
//
//   netdrop server <ip> <port>     netdrop client <ip> <port>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "../src/linux/netsock.h"

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: netdrop server|client <ip> <port>\n"); return 2; }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)atoi(argv[3]));
    if (inet_pton(AF_INET, argv[2], &a.sin_addr) != 1) return 2;
    int s;
    if (!strcmp(argv[1], "server")) {
        int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { perror("listen"); return 2; }
        s = accept(ls, NULL, NULL);
        close(ls);
    } else {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 2; }
    }
    if (s < 0) { perror("accept"); return 2; }
    tune_device_socket(s);
    char buf[4096]; memset(buf, 'x', 64);
    double last_ok = now_s();          // last time the other end answered (data received)
    printf("%s: connected\n", argv[1]); fflush(stdout);
    for (;;) {
        struct pollfd pf = { s, POLLIN | POLLOUT, 0 };
        if (poll(&pf, 1, 10) < 0) { perror("poll"); return 2; }
        if (pf.revents & (POLLERR | POLLHUP)) break;
        if (pf.revents & POLLIN) {
            ssize_t n = recv(s, buf, sizeof buf, MSG_DONTWAIT);
            if (n == 0) break;
            if (n < 0 && errno != EAGAIN) break;
            if (n > 0) last_ok = now_s();
        }
        if (pf.revents & POLLOUT) {
            if (send(s, buf, 64, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN) break;
        }
        usleep(10000);                 // ~100 messages a second
    }
    int err = 0; socklen_t el = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    printf("%s: connection ended %.1f s after the other end last answered (%s)\n", argv[1], now_s() - last_ok, strerror(err));
    printf("RESULT %s %.1f\n", argv[1], now_s() - last_ok);
    return 0;
}
