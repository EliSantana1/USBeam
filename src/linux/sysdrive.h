// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// sysdrive.h - finds the drives the running Linux system lives on (Linux).
//
// A USB drive that holds the running system must never be shared, reset or even listed by
// USBeam: taking it away would crash this PC. A disk or partition counts as a system one when:
//   - it, or anything built on it (encryption, LVM, RAID, loop devices, a multi-disk btrfs or
//     bcachefs), is mounted at /, /boot, /efi, /usr, /var, /home, /opt, /srv, /root, /etc, /nix,
//     /gnu, /snap or a live system's medium (/run/archiso, /run/initramfs, /cdrom, ...), or below;
//   - it holds swap (a swap partition, or a swap file on it);
//   - or the kernel says something has claimed it exclusively (mounted, swap, ZFS pool, RAID...)
//     and that isn't explained by an ordinary mount elsewhere (under /mnt, /media, /run/media...).
//     This catches what the first rules can't see, such as a ZFS root or a /dev/root source.
// Devices are compared by device number, not by name. Own file so tests/sysdrive_test.c checks
// exactly what USBeam uses. The exclusive-claim check needs root (the USBeam service has it).
#ifndef USBEAM_SYSDRIVE_H
#define USBEAM_SYSDRIVE_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define SYSDRV_MAX 512

static int sysdrv_is_system_mount(const char *mp) {
    static const char *roots[] = { "/boot", "/efi", "/usr", "/var", "/home", "/opt", "/srv", "/root", "/etc", "/nix", "/gnu",
                                   "/snap", "/run/archiso", "/run/initramfs", "/run/live", "/run/miso", "/run/rootfsbase",
                                   "/cdrom", "/lib/live", "/run/install", "/sysroot", NULL };
    if (!strcmp(mp, "/")) return 1;
    for (int i = 0; roots[i]; i++) {
        size_t n = strlen(roots[i]);
        if (!strncmp(mp, roots[i], n) && (mp[n] == 0 || mp[n] == '/')) return 1;
    }
    return 0;
}

static void sysdrv_add(dev_t *set, int *n, dev_t d) {
    if (!major(d) || *n >= SYSDRV_MAX) return;
    for (int i = 0; i < *n; i++) if (set[i] == d) return;
    set[(*n)++] = d;
}
static int sysdrv_has(const dev_t *set, int n, dev_t d) {
    for (int i = 0; i < n; i++) if (set[i] == d) return 1;
    return 0;
}

static int sysdrv_read_devnum(const char *sysdir, dev_t *out) {      // ".../dev" holds "maj:min"
    char p[PATH_MAX]; unsigned ma, mi;
    snprintf(p, sizeof p, "%s/dev", sysdir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int ok = fscanf(f, "%u:%u", &ma, &mi) == 2;
    fclose(f);
    if (ok) *out = makedev(ma, mi);
    return ok;
}

// "\040" etc. in /proc files stand for spaces and other characters.
static void sysdrv_unescape(char *s) {
    char *o = s;
    for (char *i = s; *i; i++) {
        if (i[0] == '\\' && i[1] >= '0' && i[1] <= '3' && i[2] >= '0' && i[2] <= '7' && i[3] >= '0' && i[3] <= '7') {
            *o++ = (char)((i[1] - '0') * 64 + (i[2] - '0') * 8 + (i[3] - '0')); i += 3;
        } else *o++ = *i;
    }
    *o = 0;
}

// The mounts, read once per check.
typedef struct { dev_t fsdev; int system; char src[256]; } SysdrvMount;
static int sysdrv_mounts(SysdrvMount *m, int max) {
    int n = 0;
    char line[4096];
    FILE *f = fopen("/proc/self/mountinfo", "r");
    while (f && n < max && fgets(line, sizeof line, f)) {
        unsigned ma, mi; char mp[2048] = "", fstype[64] = "", src[2048] = "";
        // id parent maj:min root mountpoint options ... - fstype source superoptions
        if (sscanf(line, "%*s %*s %u:%u %*s %2047s", &ma, &mi, mp) != 3) continue;
        sysdrv_unescape(mp);
        char *dash = strstr(line, " - ");
        if (dash && sscanf(dash + 3, "%63s %2047s", fstype, src) == 2) sysdrv_unescape(src);
        m[n].fsdev = makedev(ma, mi);
        m[n].system = sysdrv_is_system_mount(mp);
        snprintf(m[n].src, sizeof m[n].src, "%s", src);
        n++;
    }
    if (f) fclose(f);
    return n;
}

// Adds the block devices named in a mount source ("/dev/sda2", or "/dev/sda1:/dev/sdb1" for a
// bcachefs over several disks).
static void sysdrv_add_source(dev_t *set, int *n, const char *src) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", src);
    for (char *t = strtok(buf, ":"); t; t = strtok(NULL, ":")) {
        struct stat st;
        if (!strncmp(t, "/dev/", 5) && stat(t, &st) == 0 && S_ISBLK(st.st_mode)) sysdrv_add(set, n, st.st_rdev);
    }
}
// Adds the device(s) of the filesystem a file lives on.
static void sysdrv_add_fs_of(dev_t *set, int *n, dev_t fsdev, const SysdrvMount *m, int nm) {
    sysdrv_add(set, n, fsdev);                               // a real disk number (ext4, xfs...)
    for (int i = 0; i < nm; i++) if (m[i].fsdev == fsdev) sysdrv_add_source(set, n, m[i].src);   // btrfs & co.
}

// Every block device the system depends on (want_system=1), or every block device behind an
// ordinary (non-system) mount (want_system=0), as device numbers. Returns how many.
static int sysdrv_collect_kind(dev_t *set, int want_system) {
    static SysdrvMount m[512];
    int nm = sysdrv_mounts(m, 512), n = 0;
    for (int i = 0; i < nm; i++) if (m[i].system == want_system) sysdrv_add_fs_of(set, &n, m[i].fsdev, m, nm);
    if (want_system) {
        // Swap partitions, and swap files (their filesystem's disk).
        char line[4096];
        FILE *f = fopen("/proc/swaps", "r");
        while (f && fgets(line, sizeof line, f)) {
            char p[2048]; struct stat st;
            if (sscanf(line, "%2047s", p) != 1 || p[0] != '/') continue;
            sysdrv_unescape(p);
            if (stat(p, &st) != 0) continue;
            if (S_ISBLK(st.st_mode)) sysdrv_add(set, &n, st.st_rdev);
            else sysdrv_add_fs_of(set, &n, st.st_dev, m, nm);
        }
        if (f) fclose(f);
    }
    // A btrfs spread over several disks lists only one of them as its source: add all of them.
    DIR *fs = opendir("/sys/fs/btrfs");
    struct dirent *e;
    while (fs && (e = readdir(fs))) {
        if (e->d_name[0] == '.') continue;
        char dp[PATH_MAX]; snprintf(dp, sizeof dp, "/sys/fs/btrfs/%s/devices", e->d_name);
        dev_t mem[64]; int k = 0, hit = 0;
        DIR *dd = opendir(dp); struct dirent *b;
        while (dd && (b = readdir(dd)) && k < 64) {
            if (b->d_name[0] == '.') continue;
            char bp[PATH_MAX]; snprintf(bp, sizeof bp, "/sys/class/block/%s", b->d_name);
            if (!sysdrv_read_devnum(bp, &mem[k])) continue;
            if (sysdrv_has(set, n, mem[k])) hit = 1;
            k++;
        }
        if (dd) closedir(dd);
        if (hit) for (int i = 0; i < k; i++) sysdrv_add(set, &n, mem[i]);
    }
    if (fs) closedir(fs);
    // Down the stack (the list grows while it is walked): encryption, LVM and RAID devices sit on
    // other devices ("slaves"); a loop device sits on a file; a partition belongs to its disk.
    for (int i = 0; i < n; i++) {
        unsigned ma = major(set[i]), mi = minor(set[i]);
        char sp[PATH_MAX]; snprintf(sp, sizeof sp, "/sys/dev/block/%u:%u/slaves", ma, mi);
        DIR *sd = opendir(sp); struct dirent *s;
        while (sd && (s = readdir(sd))) {
            if (s->d_name[0] == '.') continue;
            char bp[PATH_MAX]; dev_t d;
            snprintf(bp, sizeof bp, "/sys/class/block/%s", s->d_name);
            if (sysdrv_read_devnum(bp, &d)) sysdrv_add(set, &n, d);
        }
        if (sd) closedir(sd);
        char lp[PATH_MAX], bf[PATH_MAX]; snprintf(lp, sizeof lp, "/sys/dev/block/%u:%u/loop/backing_file", ma, mi);
        FILE *lf = fopen(lp, "r");
        if (lf) {
            struct stat st;
            if (fgets(bf, sizeof bf, lf)) { bf[strcspn(bf, "\n")] = 0; if (stat(bf, &st) == 0) sysdrv_add_fs_of(set, &n, st.st_dev, m, nm); }
            fclose(lf);
        }
        char pp[PATH_MAX], rp[PATH_MAX]; struct stat st;
        snprintf(pp, sizeof pp, "/sys/dev/block/%u:%u/partition", ma, mi);
        if (stat(pp, &st) == 0) {
            snprintf(pp, sizeof pp, "/sys/dev/block/%u:%u/..", ma, mi);
            dev_t d;
            if (realpath(pp, rp) && sysdrv_read_devnum(rp, &d)) sysdrv_add(set, &n, d);
        }
    }
    return n;
}

// The two lists, refreshed at most once a second (they're asked for very often).
static dev_t g_sysdrv_sys[SYSDRV_MAX], g_sysdrv_ord[SYSDRV_MAX];
static int g_sysdrv_nsys, g_sysdrv_nord;
static long long g_sysdrv_at = -1;
static void sysdrv_refresh(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    long long now = (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    if (g_sysdrv_at >= 0 && now - g_sysdrv_at < 1000) return;
    g_sysdrv_at = now;
    g_sysdrv_nsys = sysdrv_collect_kind(g_sysdrv_sys, 1);
    g_sysdrv_nord = sysdrv_collect_kind(g_sysdrv_ord, 0);
}

// 1 if the block device with this kernel name (sda, sda2, dm-0, loop0...) is a system one.
static int sysdrv_block_is_system(const char *name) {
    char bp[PATH_MAX]; dev_t d;
    snprintf(bp, sizeof bp, "/sys/class/block/%s", name);
    if (!sysdrv_read_devnum(bp, &d)) return 0;
    sysdrv_refresh();
    if (sysdrv_has(g_sysdrv_sys, g_sysdrv_nsys, d)) return 1;
    // Claimed by something, but not by an ordinary mount we know of: treat it as the system's.
    char dev[PATH_MAX]; snprintf(dev, sizeof dev, "/dev/%s", name);
    int fd = open(dev, O_RDONLY | O_EXCL | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) { close(fd); return 0; }
    return errno == EBUSY && !sysdrv_has(g_sysdrv_ord, g_sysdrv_nord, d);
}

// 1 if any disk or partition behind the USB device "busid" (e.g. 2-3) is a system one.
static int sysdrv_usb_is_system(const char *busid) {
    char needle[48]; snprintf(needle, sizeof needle, "/%s/", busid);
    int hit = 0;
    DIR *d = opendir("/sys/class/block");
    struct dirent *e;
    while (!hit && d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof link, "/sys/class/block/%s", e->d_name);
        if (!realpath(link, real) || !strstr(real, needle)) continue;
        if (sysdrv_block_is_system(e->d_name)) hit = 1;
    }
    if (d) closedir(d);
    return hit;
}

#endif
