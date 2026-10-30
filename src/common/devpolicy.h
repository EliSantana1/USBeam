// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// devpolicy.h - which USB devices are offered to other PCs by default (Windows and Linux).
//
// Some USB devices are part of the PC itself rather than something plugged into it: lighting
// controllers on the motherboard, the Bluetooth part of a Wi-Fi card, fingerprint readers and
// so on. Those are hidden unless the user chooses to share them. Each platform decides
// "built into this PC" (internal port) and "wireless adapter" itself; this file has the
// shared rules for lighting controllers and for keyboards / mice.
//
// Interfaces are given as (class, subclass, protocol) triplets.

#include <ctype.h>
#include <string.h>

// Lighting controllers known by their ids (in case the name or port gives no hint).
static const struct { unsigned short vid, pid; } DEVP_LIGHTING_IDS[] = {
    { 0x0b05, 0x1867 }, { 0x0b05, 0x1872 }, { 0x0b05, 0x18f3 }, { 0x0b05, 0x19af },   // ASUS Aura
    { 0x6940, 0x6973 },                                                                   // Corsair Gaming RGB controller
};

static int devp_contains(const char *lower, const char *word) { return strstr(lower, word) != NULL; }

// A keyboard or mouse (it stops working on its own PC while another PC uses it).
static int devp_is_input(const unsigned char (*ifc)[3], int n) {
    for (int i = 0; i < n; i++) if (ifc[i][0] == 0x03 && (ifc[i][2] == 1 || ifc[i][2] == 2)) return 1;
    return 0;
}

// Has a real function besides lighting: keyboard, mouse, sound, storage, camera, network, ...
// (Lighting controllers only have the generic "vendor data" connection.)
static int devp_has_function(const unsigned char (*ifc)[3], int n) {
    for (int i = 0; i < n; i++) {
        unsigned c = ifc[i][0];
        if (c == 0x03 && ifc[i][2] == 0) continue;     // HID without keyboard/mouse function
        if (c == 0xff || c == 0xfe || c == 0x00) continue;   // vendor-specific, firmware update, unset
        return 1;
    }
    return 0;
}

// A lighting (RGB/LED) controller. have_ifc = interfaces are known.
static int devp_is_lighting(const char *name, unsigned vid, unsigned pid,
                            const unsigned char (*ifc)[3], int n, int have_ifc) {
    for (size_t i = 0; i < sizeof DEVP_LIGHTING_IDS / sizeof DEVP_LIGHTING_IDS[0]; i++)
        if (DEVP_LIGHTING_IDS[i].vid == vid && DEVP_LIGHTING_IDS[i].pid == pid) return 1;
    char lower[256]; size_t k = 0;
    for (const char *c = name ? name : ""; *c && k < sizeof lower - 1; c++) lower[k++] = (char)tolower((unsigned char)*c);
    lower[k] = 0;
    static const char *kw[] = { "rgb", "led controller", "lighting", "aura", "mystic light", "polychrome",
                                "icue", "commander", "argb", "light controller", "led strip" };
    static const char *not_kw[] = { "keyboard", "mouse", "headset", "headphone", "speaker", "microphone",
                                    "webcam", "camera", "gamepad", "joystick", "receiver", "dongle", "drive", "hub" };
    int hit = 0;
    for (size_t i = 0; i < sizeof kw / sizeof kw[0] && !hit; i++) if (devp_contains(lower, kw[i])) hit = 1;
    if (!hit) return 0;
    for (size_t i = 0; i < sizeof not_kw / sizeof not_kw[0]; i++) if (devp_contains(lower, not_kw[i])) return 0;
    if (have_ifc && devp_has_function(ifc, n)) return 0;
    return 1;
}
