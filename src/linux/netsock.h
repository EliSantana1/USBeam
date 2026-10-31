// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// netsock.h - settings for connections that carry a device (Linux). Own file so the
// network-drop test (tests/netdrop.c) checks exactly what USBeam uses.
#ifndef USBEAM_NETSOCK_H
#define USBEAM_NETSOCK_H
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#ifndef TCP_USER_TIMEOUT
#define TCP_USER_TIMEOUT 18
#endif
// Settings for a connection that may carry a device. A dead network (Wi-Fi dropped, PC asleep)
// is noticed within ~20 s: keepalive covers a quiet connection, and TCP_USER_TIMEOUT covers one
// with data waiting to be delivered (a game controller always has some). Without it, Linux keeps
// retrying for up to ~15 minutes and the device stays "in use" by a PC that is gone.
static void tune_device_socket(int s) {
    int one = 1, idle = 10, intvl = 2, cnt = 5;
    unsigned int user_ms = 20000;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_ms, sizeof user_ms);
}

#endif
