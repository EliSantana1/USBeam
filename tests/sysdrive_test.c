// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// sysdrive_test.c - prints whether USBeam treats block devices or USB devices as system drives
// (src/linux/sysdrive.h). Used by tests/sysdrive.sh.
//
//   sysdrive_test block <name>...     e.g. sda sda2 loop0
//   sysdrive_test usb <busid>...      e.g. 2-3
#include "../src/linux/sysdrive.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: sysdrive_test block|usb <name>...\n"); return 2; }
    int usb = !strcmp(argv[1], "usb");
    for (int i = 2; i < argc; i++)
        printf("%s %s\n", argv[i], (usb ? sysdrv_usb_is_system(argv[i]) : sysdrv_block_is_system(argv[i])) ? "system" : "not-system");
    return 0;
}
