// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
// client_types.h - types the platform hooks of client.h need.
#ifndef USBEAM_CLIENT_TYPES_H
#define USBEAM_CLIENT_TYPES_H
#include <stdint.h>
enum { NET_AUTO = 0, NET_HOME = 1, NET_ZT = 2, NET_TS = 3 };
typedef struct { uint32_t net, mask; int kind; } LocalNet;
#endif
