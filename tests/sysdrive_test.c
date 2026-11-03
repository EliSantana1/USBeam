// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// sysdrive_test.c - how USBeam treats block devices or USB devices (src/linux/sysdrive.h).
// Used by tests/sysdrive.sh.
//
//   sysdrive_test block <name>...     system / in-use-unknown / not-system      e.g. sda sda2 loop0
//   sysdrive_test usb <busid>...      the same for everything behind a USB device, e.g. 2-3
//   sysdrive_test check <name>...     what USBeam does: hidden / shown (and remembers certain ones
//                                     in the test's own file, like USBeam does until a restart)
#include <stdarg.h>
#define SYSDRV_FILE "/tmp/usbeam-sysdrive-test/known"
#include "../src/linux/sysdrive.h"
static void logm(const char *fmt, ...) { va_list a; va_start(a, fmt); fprintf(stderr, "  usbeam: "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n"); va_end(a); }
static const char *NAME[] = { "not-system", "in-use-unknown", "system" };

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: sysdrive_test block|usb|check <name>...\n"); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[1], "check")) printf("%s %s\n", argv[i], sysdrv_check(argv[i], argv[i], 1, sysdrv_block_state) ? "hidden" : "shown");
        else printf("%s %s\n", argv[i], NAME[!strcmp(argv[1], "usb") ? sysdrv_usb_state(argv[i]) : sysdrv_block_state(argv[i])]);
    }
    return 0;
}
