// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// usbeam_host.c - USBeam host for Windows (test version 6).
//
// - Lists removable USB devices and lets you share / stop sharing them.
// - Sharing switches the device to Microsoft's built-in, Microsoft-signed
//   WinUSB driver (C:\Windows\INF\winusb.inf). No custom certificates.
// - Stop sharing uninstalls that and lets Windows reinstall the normal driver.
// - Serves every shared device over standard USB/IP (TCP 3240), several at once.
// - If a device is unplugged or the network drops, the session ends cleanly.
//
// Not yet supported: isochronous transfers (webcams, USB audio).
// Log: usbeam_log.txt next to the exe.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <winusb.h>
// From usbioctl.h (not included: its names clash with winusb.h)
#define IOCTL_USB_HUB_CYCLE_PORT CTL_CODE(FILE_DEVICE_UNKNOWN, 273, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct { ULONG ConnectionIndex; ULONG StatusReturned; } USB_CYCLE_PORT_PARAMS;
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <ctype.h>

#define USBIP_PORT 3240
#define USBIP_VERSION 0x0111
#define OP_REQ_DEVLIST 0x8005
#define OP_REP_DEVLIST 0x0005
#define OP_REQ_IMPORT 0x8003
#define OP_REP_IMPORT 0x0003
#define CMD_SUBMIT 1
#define CMD_UNLINK 2
#define RET_SUBMIT 3
#define RET_UNLINK 4

#define E_PIPE (-32)
#define E_NOSYS (-38)
#define E_PROTO (-71)
#define E_CONNRESET (-104)
#define E_SHUTDOWN (-108)

#define ST_OK 0
#define ST_NA 1      // not available / unknown busid

static const GUID GUID_USB_DEVICE_IF = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
static const GUID GUID_USBDEVICE_CLASS = {0x88bae032, 0x5a81, 0x49f0, {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}};
static const wchar_t *USBDEVICE_CLASS_STR = L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}";

typedef BOOL (WINAPI *DiInstallDevice_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
typedef BOOL (WINAPI *DiUninstallDevice_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);

// ================================================================ logging
static FILE *g_log;
static CRITICAL_SECTION g_log_cs;
static int g_verbose;

static void logm(const char *fmt, ...) {
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME t; GetLocalTime(&t);
    char pre[32];
    snprintf(pre, sizeof pre, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    if (g_log) { fputs(pre, g_log); va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fputc('\n', g_log); fflush(g_log); }
    LeaveCriticalSection(&g_log_cs);
}
// console + log
static void say(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    EnterCriticalSection(&g_log_cs); printf("%s\n", buf); LeaveCriticalSection(&g_log_cs);
    logm("%s", buf);
}
#define logv(...) do { if (g_verbose) logm(__VA_ARGS__); } while (0)

// ================================================================ device model
#define MAX_DEVS 64
#define MAX_IFACES 16

struct Dev;
typedef struct Urb {
    struct Urb *next;
    uint32_t seqnum, direction, ep;
    int32_t length;
    uint8_t setup[8];
    uint8_t *data;
    int unlinked;
    uint32_t unlink_seqnum;
} Urb;

typedef struct {
    struct Dev *d;
    int key;                  // 0 = control, else endpoint address
    HANDLE thread;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    Urb *head, *tail, *inflight;
    OVERLAPPED ov;
} Worker;

typedef struct Dev {
    // identity (stable while program runs)
    int used;
    wchar_t inst[256];
    char busid[32];
    // refreshed info
    int present, removable, on_winusb;
    wchar_t name[256];
    wchar_t service[64];
    uint16_t vid, pid;
    // cached descriptors (filled while on WinUSB)
    int have_desc;
    USB_DEVICE_DESCRIPTOR dd;
    uint8_t cfg[4096]; int cfg_len, num_ifaces;
    uint8_t iface_class[MAX_IFACES][3];
    uint32_t speed;
    // sharing policy
    int wanted;              // user marked it as shared (available to clients)
    int auto_rc;             // auto-reconnect after a replug
    int blocked;             // temporarily kept on this PC (refuse clients)
    int returning;           // was unplugged during a session; give back to PC when it returns
    ULONGLONG hold_until;    // reconnect countdown end (GetTickCount64), 0 = none
    int last_shown;          // last countdown second printed
    volatile LONG busy;      // driver switch in progress
    char last_peer[64];
    // session (only while a client is attached)
    volatile LONG in_use;
    char peer[64];
    SOCKET client;
    HANDLE file;
    WINUSB_INTERFACE_HANDLE ifh[MAX_IFACES];
    uint8_t ep_iface[256];
    CRITICAL_SECTION send_cs;
    Worker *workers[257];
    volatile LONG quit;
} Dev;

static Dev g_devs[MAX_DEVS];
static CRITICAL_SECTION g_devs_cs;
static int g_next_bus = 1;

// ================================================================ setupapi helpers
static BOOL get_prop(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD prop, wchar_t *buf, DWORD cb) {
    buf[0] = 0;
    return SetupDiGetDeviceRegistryPropertyW(set, dev, prop, NULL, (BYTE *)buf, cb, NULL);
}

static BOOL open_inst(const wchar_t *inst, HDEVINFO *set, SP_DEVINFO_DATA *dev, const GUID *cls) {
    *set = SetupDiCreateDeviceInfoList(cls, NULL);
    if (*set == INVALID_HANDLE_VALUE) return FALSE;
    memset(dev, 0, sizeof *dev); dev->cbSize = sizeof *dev;
    if (!SetupDiOpenDeviceInfoW(*set, inst, NULL, 0, dev)) { SetupDiDestroyDeviceInfoList(*set); return FALSE; }
    return TRUE;
}

static Dev *dev_slot(const wchar_t *inst) {
    for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].used && !_wcsicmp(g_devs[i].inst, inst)) return &g_devs[i];
    for (int i = 0; i < MAX_DEVS; i++) if (!g_devs[i].used) {
        Dev *d = &g_devs[i];
        memset(d, 0, sizeof *d);
        d->used = 1;
        wcsncpy(d->inst, inst, 255);
        snprintf(d->busid, sizeof d->busid, "1-%d", g_next_bus++);
        InitializeCriticalSection(&d->send_cs);
        d->client = INVALID_SOCKET;
        d->file = INVALID_HANDLE_VALUE;
        return d;
    }
    return NULL;
}

static BOOL read_descriptors(Dev *d);
static uint32_t hub_speed(const wchar_t *inst);
static BOOL hub_descriptors(Dev *d);

// Rescan USB devices. Only top-level, removable, non-hub devices are listed.
static void refresh_devices(void) {
    EnterCriticalSection(&g_devs_cs);
    for (int i = 0; i < MAX_DEVS; i++) { if (g_devs[i].present == 0) g_devs[i].have_desc = 0; g_devs[i].present = 0; }
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t inst[256], hw[2048], svc[64];
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, inst, 256, NULL)) continue;
        if (_wcsnicmp(inst, L"USB\\VID_", 8) != 0 || wcsstr(inst, L"&MI_")) continue;  // skip roots & interface children
        if (wcslen(inst) < 22 || inst[21] != L'\\') continue;   // only real devices: USB\\VID_xxxx&PID_xxxx\\<id>
        get_prop(set, &dev, SPDRP_HARDWAREID, hw, sizeof hw);
        get_prop(set, &dev, SPDRP_SERVICE, svc, sizeof svc);
        if (!_wcsnicmp(svc, L"usbhub", 6)) continue;                              // skip hubs
        DWORD caps = 0;
        SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_CAPABILITIES, NULL, (BYTE *)&caps, sizeof caps, NULL);
        Dev *d = dev_slot(inst);
        if (!d) continue;
        if (!d->present) logm("scan: %ls  service=%ls  caps=0x%lX  hw=%ls", inst, svc, caps, hw);
        d->present = 1;
        d->removable = (caps & CM_DEVCAP_REMOVABLE) != 0;
        wcsncpy(d->service, svc, 63);
        d->on_winusb = !_wcsicmp(svc, L"WINUSB");
        unsigned v = 0, p = 0;
        swscanf(inst, L"USB\\VID_%4x&PID_%4x", &v, &p);
        d->vid = (uint16_t)v; d->pid = (uint16_t)p;
        DEVPROPTYPE t; d->name[0] = 0;
        if (!SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_BusReportedDeviceDesc, &t, (BYTE *)d->name, sizeof d->name, NULL, 0) || !d->name[0])
            get_prop(set, &dev, SPDRP_DEVICEDESC, d->name, sizeof d->name);
        if (d->wanted && !d->have_desc && !d->in_use && !d->busy) hub_descriptors(d);
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    LeaveCriticalSection(&g_devs_cs);
}

// ================================================================ WinUSB open/close
static BOOL device_path(const wchar_t *inst, wchar_t *path, size_t cch) {
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_USB_DEVICE_IF, inst, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    BOOL ok = FALSE;
    if (SetupDiEnumDeviceInterfaces(set, NULL, &GUID_USB_DEVICE_IF, 0, &ifd)) {
        BYTE buf[2048];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof buf, NULL, NULL)) {
            wcsncpy(path, det->DevicePath, cch - 1); path[cch - 1] = 0; ok = TRUE;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

static void close_usb(Dev *d) {
    for (int i = MAX_IFACES - 1; i >= 0; i--) if (d->ifh[i]) { WinUsb_Free(d->ifh[i]); d->ifh[i] = NULL; }
    if (d->file != INVALID_HANDLE_VALUE) { CloseHandle(d->file); d->file = INVALID_HANDLE_VALUE; }
}

static BOOL open_usb(Dev *d) {
    wchar_t path[1024];
    memset(d->ep_iface, 0xFF, sizeof d->ep_iface);
    if (!device_path(d->inst, path, 1024)) { logm("[%s] no device path", d->busid); return FALSE; }
    d->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (d->file == INVALID_HANDLE_VALUE) { logm("[%s] CreateFile failed (%lu)", d->busid, GetLastError()); return FALSE; }
    if (!WinUsb_Initialize(d->file, &d->ifh[0])) { logm("[%s] WinUsb_Initialize failed (%lu)", d->busid, GetLastError()); close_usb(d); return FALSE; }

    ULONG len = 0;
    if (!WinUsb_GetDescriptor(d->ifh[0], USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, (PUCHAR)&d->dd, sizeof d->dd, &len)) goto fail;
    USB_CONFIGURATION_DESCRIPTOR ch;
    if (!WinUsb_GetDescriptor(d->ifh[0], USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, (PUCHAR)&ch, sizeof ch, &len)) goto fail;
    int total = ch.wTotalLength > sizeof d->cfg ? (int)sizeof d->cfg : ch.wTotalLength;
    if (!WinUsb_GetDescriptor(d->ifh[0], USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, d->cfg, total, &len)) goto fail;
    d->cfg_len = (int)len;
    d->num_ifaces = ch.bNumInterfaces > MAX_IFACES ? MAX_IFACES : ch.bNumInterfaces;

    int cur_if = -1;
    memset(d->iface_class, 0, sizeof d->iface_class);
    for (int off = 0; off + 2 <= d->cfg_len && d->cfg[off] >= 2; off += d->cfg[off]) {
        uint8_t *p = &d->cfg[off];
        if (p[1] == 4 && off + 9 <= d->cfg_len) {
            cur_if = p[2];
            if (p[3] == 0 && cur_if < MAX_IFACES) { d->iface_class[cur_if][0] = p[5]; d->iface_class[cur_if][1] = p[6]; d->iface_class[cur_if][2] = p[7]; }
        } else if (p[1] == 5 && off + 7 <= d->cfg_len && cur_if >= 0) {
            d->ep_iface[p[2]] = (uint8_t)cur_if;
        }
    }
    for (int i = 1; i < d->num_ifaces; i++)
        if (!WinUsb_GetAssociatedInterface(d->ifh[0], (UCHAR)(i - 1), &d->ifh[i]))
            logm("[%s] could not open interface %d (%lu)", d->busid, i, GetLastError());

    UCHAR spd = 0; len = 1;
    WinUsb_QueryDeviceInformation(d->ifh[0], DEVICE_SPEED, &len, &spd);
    // Best source: the hub reports the exact speed. Fallback: WinUSB says only
    // "high" (3) or "low/full" (1); low-speed devices always use 8-byte ep0.
    d->speed = hub_speed(d->inst);
    if (!d->speed) d->speed = spd == 3 ? 3 : (d->dd.bMaxPacketSize0 == 8 ? 1 : 2);
    // A device running at SuperSpeed includes endpoint companion descriptors (type 0x30).
    for (int off = 0; off + 2 <= d->cfg_len && d->cfg[off] >= 2; off += d->cfg[off])
        if (d->cfg[off + 1] == 0x30) { d->speed = 5; break; }
    logm("[%s] winusb speed=%u bcdUSB=%04X -> usbip speed %u", d->busid, spd, d->dd.bcdUSB, d->speed);
    for (int i = 0; i < d->num_ifaces; i++) if (d->ifh[i]) {
        UCHAR off = FALSE;
        WinUsb_SetPowerPolicy(d->ifh[i], AUTO_SUSPEND, sizeof off, &off);
    }
    d->have_desc = 1;
    return TRUE;
fail:
    logm("[%s] reading descriptors failed (%lu)", d->busid, GetLastError());
    close_usb(d);
    return FALSE;
}

static BOOL read_descriptors(Dev *d) {
    BOOL ok = open_usb(d);
    close_usb(d);
    return ok;
}

// ================================================================ share / unshare
static DiInstallDevice_t pDiInstallDevice;
static DiUninstallDevice_t pDiUninstallDevice;

static const GUID GUID_USB_HUB_IF = {0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};

// Open the hub a device is plugged into; returns its port number on that hub.
static HANDLE open_parent_hub(DEVINST di, DWORD *port) {
    DEVINST parent;
    wchar_t pinst[256], dinst[256];
    *port = 0;
    if (CM_Get_Parent(&parent, di, 0) != CR_SUCCESS) return INVALID_HANDLE_VALUE;
    if (CM_Get_Device_IDW(parent, pinst, 256, 0) != CR_SUCCESS || CM_Get_Device_IDW(di, dinst, 256, 0) != CR_SUCCESS) return INVALID_HANDLE_VALUE;
    HDEVINFO s = SetupDiCreateDeviceInfoList(NULL, NULL);
    SP_DEVINFO_DATA dd = { sizeof(dd) };
    if (SetupDiOpenDeviceInfoW(s, dinst, NULL, 0, &dd))
        SetupDiGetDeviceRegistryPropertyW(s, &dd, SPDRP_ADDRESS, NULL, (BYTE *)port, sizeof *port, NULL);
    SetupDiDestroyDeviceInfoList(s);
    if (!*port) return INVALID_HANDLE_VALUE;
    HANDLE h = INVALID_HANDLE_VALUE;
    HDEVINFO hs = SetupDiGetClassDevsW(&GUID_USB_HUB_IF, pinst, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hs == INVALID_HANDLE_VALUE) return h;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    if (SetupDiEnumDeviceInterfaces(hs, NULL, &GUID_USB_HUB_IF, 0, &ifd)) {
        BYTE buf[2048];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetailW(hs, &ifd, det, sizeof buf, NULL, NULL))
            h = CreateFileW(det->DevicePath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    SetupDiDestroyDeviceInfoList(hs);
    return h;
}

// Exact connection speed from the hub. usbip values: 1 low, 2 full, 3 high, 5 super. 0 = unknown.
#define IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX CTL_CODE(FILE_DEVICE_UNKNOWN, 274, METHOD_BUFFERED, FILE_ANY_ACCESS)
static uint32_t hub_speed(const wchar_t *inst) {
    DEVINST di; DWORD port;
    if (CM_Locate_DevNodeW(&di, (DEVINSTID_W)inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 0;
    HANDLE h = open_parent_hub(di, &port);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BYTE buf[512] = {0};
    *(ULONG *)buf = port;
    DWORD br = 0;
    uint32_t r = 0;
    // USB_NODE_CONNECTION_INFORMATION_EX (packed): Speed byte is at offset 23.
    if (DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf, sizeof buf, buf, sizeof buf, &br, NULL) && br > 23) {
        static const uint32_t map[] = { 1, 2, 3, 5 };   // UsbLowSpeed, UsbFullSpeed, UsbHighSpeed, UsbSuperSpeed
        if (buf[23] < 4) r = map[buf[23]];
    }
    CloseHandle(h);
    return r;
}

// Read device + configuration descriptors through the hub. Works whatever
// driver the device is using, so a shared device can be listed to clients
// while it keeps working normally on this PC.
#define IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION CTL_CODE(FILE_DEVICE_UNKNOWN, 260, METHOD_BUFFERED, FILE_ANY_ACCESS)
static int hub_get_desc(HANDLE h, DWORD port, uint8_t type, uint8_t *out, int len) {
    static BYTE buf[12 + 4096];
    if (len > 4096) len = 4096;
    memset(buf, 0, 12);
    *(ULONG *)buf = port;
    buf[4] = 0x80; buf[5] = 0x06;                 // GET_DESCRIPTOR
    buf[6] = 0; buf[7] = type;                    // wValue = type << 8 | index 0
    buf[10] = (BYTE)len; buf[11] = (BYTE)(len >> 8);
    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf, 12 + len, buf, 12 + len, &br, NULL) || br <= 12) return -1;
    memcpy(out, buf + 12, br - 12);
    return (int)(br - 12);
}
static CRITICAL_SECTION g_hubdesc_cs;
static BOOL hub_descriptors(Dev *d) {
    DEVINST di; DWORD port;
    if (CM_Locate_DevNodeW(&di, (DEVINSTID_W)d->inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return FALSE;
    HANDLE h = open_parent_hub(di, &port);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    EnterCriticalSection(&g_hubdesc_cs);
    BOOL ok = FALSE;
    uint8_t cfg[4096];
    if (hub_get_desc(h, port, 1, (uint8_t *)&d->dd, sizeof d->dd) == (int)sizeof d->dd) {
        int n = hub_get_desc(h, port, 2, cfg, 9);
        if (n == 9) {
            int total = cfg[2] | cfg[3] << 8;
            n = hub_get_desc(h, port, 2, cfg, total);
            if (n >= 9) {
                memcpy(d->cfg, cfg, n); d->cfg_len = n;
                d->num_ifaces = cfg[4] > MAX_IFACES ? MAX_IFACES : cfg[4];
                memset(d->iface_class, 0, sizeof d->iface_class);
                for (int off = 0; off + 2 <= n && cfg[off] >= 2; off += cfg[off]) {
                    uint8_t *q = &cfg[off];
                    if (q[1] == 4 && off + 9 <= n && q[3] == 0 && q[2] < MAX_IFACES) { d->iface_class[q[2]][0] = q[5]; d->iface_class[q[2]][1] = q[6]; d->iface_class[q[2]][2] = q[7]; }
                }
                ok = TRUE;
            }
        }
    }
    LeaveCriticalSection(&g_hubdesc_cs);
    CloseHandle(h);
    if (ok) {
        d->speed = hub_speed(d->inst);
        if (!d->speed) d->speed = d->dd.bMaxPacketSize0 == 8 ? 1 : 2;
        for (int off = 0; off + 2 <= d->cfg_len && d->cfg[off] >= 2; off += d->cfg[off])
            if (d->cfg[off + 1] == 0x30) { d->speed = 5; break; }
        d->have_desc = 1;
    } else logm("[%s] could not read descriptors through hub (%lu)", d->busid, GetLastError());
    return ok;
}

// Power-cycle the hub port the device is plugged into. To Windows this is a
// real unplug + replug, so nothing can refuse it.
static BOOL cycle_port(DEVINST di) {
    DWORD port;
    HANDLE h = open_parent_hub(di, &port);
    if (h == INVALID_HANDLE_VALUE) { logm("restart: cannot open hub (%lu)", GetLastError()); return FALSE; }
    USB_CYCLE_PORT_PARAMS cp = { port, 0 };
    DWORD br;
    BOOL ok = DeviceIoControl(h, IOCTL_USB_HUB_CYCLE_PORT, &cp, sizeof cp, &cp, sizeof cp, &br, NULL);
    logm("restart: cycle hub port %lu -> %s (err %lu, status %lu)", port, ok ? "ok" : "failed", ok ? 0 : GetLastError(), cp.StatusReturned);
    CloseHandle(h);
    return ok;
}

// Make the new driver load: ask Windows to remove + re-detect the device;
// if a program refuses (has it open), power-cycle its port; last resort
// disable/enable.
static BOOL restart_device(const wchar_t *inst) {
    DEVINST di, parent;
    if (CM_Locate_DevNodeW(&di, (DEVINSTID_W)inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return FALSE;
    if (CM_Get_Parent(&parent, di, 0) != CR_SUCCESS) return FALSE;
    PNP_VETO_TYPE vt = 0; wchar_t vname[MAX_PATH] = L"";
    CONFIGRET cr = CM_Query_And_Remove_SubTreeW(di, &vt, vname, MAX_PATH, CM_REMOVE_UI_NOT_OK);
    if (cr == CR_SUCCESS) {
        CM_Reenumerate_DevNode(parent, CM_REENUMERATE_SYNCHRONOUS);
        logm("restart: removed and re-detected");
        return TRUE;
    }
    logm("restart: remove refused (cr=%lu veto=%d by %ls), trying port power-cycle", cr, (int)vt, vname);
    if (cycle_port(di)) return TRUE;
    if (CM_Disable_DevNode(di, CM_DISABLE_UI_NOT_OK) == CR_SUCCESS) {
        CM_Enable_DevNode(di, 0);
        logm("restart: disabled and re-enabled");
        return TRUE;
    }
    return FALSE;
}

static void rescan_hardware(void) {
    DEVINST root;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
}

static BOOL share_device(Dev *d) {
    HDEVINFO set; SP_DEVINFO_DATA dev;
    wchar_t oldClass[64] = L"";
    if (!open_inst(d->inst, &set, &dev, NULL)) { say("  Could not open device (%lu).", GetLastError()); return FALSE; }
    get_prop(set, &dev, SPDRP_CLASSGUID, oldClass, sizeof oldClass);
    logm("[%s] share: old class %ls service %ls", d->busid, oldClass, d->service);
    if (!SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_CLASSGUID, (const BYTE *)USBDEVICE_CLASS_STR,
                                           (DWORD)((wcslen(USBDEVICE_CLASS_STR) + 1) * sizeof(wchar_t)))) {
        say("  Could not change device category (%lu).", GetLastError());
        SetupDiDestroyDeviceInfoList(set); return FALSE;
    }
    SetupDiDestroyDeviceInfoList(set);

    BOOL ok = FALSE, reboot = FALSE;
    if (open_inst(d->inst, &set, &dev, &GUID_USBDEVICE_CLASS)) {
        SP_DEVINSTALL_PARAMS_W ip = { sizeof(ip) };
        SetupDiGetDeviceInstallParamsW(set, &dev, &ip);
        ip.Flags |= DI_ENUMSINGLEINF;
        ip.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        GetWindowsDirectoryW(ip.DriverPath, MAX_PATH);
        wcscat(ip.DriverPath, L"\\INF\\winusb.inf");
        SP_DRVINFO_DATA_W drv = { sizeof(drv) };
        if (SetupDiSetDeviceInstallParamsW(set, &dev, &ip) && SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER)) {
            for (DWORD i = 0; SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, i, &drv); i++) {
                if (!_wcsicmp(drv.Description, L"WinUsb Device")) {
                    ok = SetupDiSetSelectedDriverW(set, &dev, &drv) && pDiInstallDevice(NULL, set, &dev, &drv, 0, &reboot);
                    if (!ok) logm("[%s] DiInstallDevice failed 0x%08lX", d->busid, GetLastError());
                    break;
                }
            }
        } else logm("[%s] driver list failed %lu", d->busid, GetLastError());
        SetupDiDestroyDeviceInfoList(set);
    }
    if (!ok) {   // roll back category
        if (open_inst(d->inst, &set, &dev, NULL)) {
            SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_CLASSGUID, (const BYTE *)oldClass, (DWORD)((wcslen(oldClass) + 1) * sizeof(wchar_t)));
            SetupDiDestroyDeviceInfoList(set);
        }
        say("  Sharing failed. The device was left on its normal driver.");
        return FALSE;
    }
    if (!restart_device(d->inst)) logm("[%s] software restart failed", d->busid);
    for (int i = 0; i < 20; i++) {          // wait up to ~10 s for the device to come back on WinUSB
        Sleep(500);
        refresh_devices();
        if (d->present && d->on_winusb && read_descriptors(d)) return TRUE;
    }
    say("  [%s] Windows did not restart the device on the sharing driver.", d->busid);
    return FALSE;
}

// Put the device back on its normal Windows driver.
static BOOL revert_driver(Dev *d) {
    HDEVINFO set; SP_DEVINFO_DATA dev;
    if (!open_inst(d->inst, &set, &dev, NULL)) { say("  Could not open device (%lu).", GetLastError()); return FALSE; }
    BOOL reboot = FALSE;
    BOOL ok = pDiUninstallDevice(NULL, set, &dev, 0, &reboot);
    DWORD err = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    if (!ok) { say("  Could not remove the WinUSB driver (0x%08lX).", err); return FALSE; }
    rescan_hardware();          // Windows re-detects the device and installs its normal driver
    for (int i = 0; i < 10; i++) { Sleep(300); refresh_devices(); if (d->present && !d->on_winusb) break; }
    logm("[%s] back on normal driver (service %ls)", d->busid, d->service);
    return TRUE;
}

static void end_session(Dev *d) {
    if (!d->in_use) return;
    InterlockedExchange(&d->quit, 1);
    if (d->client != INVALID_SOCKET) shutdown(d->client, SD_BOTH);
    for (int i = 0; i < 100 && d->in_use; i++) Sleep(100);
}

// ================================================================ network helpers
static BOOL recv_all(SOCKET s, void *buf, int n) {
    char *p = buf;
    while (n > 0) { int r = recv(s, p, n, 0); if (r <= 0) return FALSE; p += r; n -= r; }
    return TRUE;
}
static BOOL send_all(SOCKET s, const void *buf, int n) {
    const char *p = buf;
    while (n > 0) { int r = send(s, p, n, 0); if (r <= 0) return FALSE; p += r; n -= r; }
    return TRUE;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void fill_device_record(Dev *d, uint8_t *r) {
    int busnum = 1, devnum = 1;
    sscanf(d->busid, "%d-%d", &busnum, &devnum);
    memset(r, 0, 312);
    snprintf((char *)r, 256, "/sys/devices/usbeam/%s", d->busid);
    snprintf((char *)r + 256, 32, "%s", d->busid);
    put32(r + 288, busnum); put32(r + 292, devnum); put32(r + 296, d->speed);
    put16(r + 300, d->dd.idVendor); put16(r + 302, d->dd.idProduct); put16(r + 304, d->dd.bcdDevice);
    r[306] = d->dd.bDeviceClass; r[307] = d->dd.bDeviceSubClass; r[308] = d->dd.bDeviceProtocol;
    r[309] = d->cfg_len >= 6 ? d->cfg[5] : 1;
    r[310] = d->dd.bNumConfigurations;
    r[311] = (uint8_t)d->num_ifaces;
}

// ================================================================ URB handling
static WINUSB_INTERFACE_HANDLE h_iface(Dev *d, int n) {
    if (n >= 0 && n < MAX_IFACES && d->ifh[n]) return d->ifh[n];
    return d->ifh[0];
}
static WINUSB_INTERFACE_HANDLE h_ep(Dev *d, uint8_t ep) {
    uint8_t i = d->ep_iface[ep];
    return h_iface(d, i == 0xFF ? 0 : i);
}

static void send_ret_submit(Dev *d, Urb *u, int32_t status, int32_t actual, const uint8_t *data) {
    uint8_t h[48] = {0};
    put32(h, RET_SUBMIT); put32(h + 4, u->seqnum);
    put32(h + 20, (uint32_t)status); put32(h + 24, (uint32_t)actual);
    EnterCriticalSection(&d->send_cs);
    if (send_all(d->client, h, 48) && data && actual > 0) send_all(d->client, data, actual);
    LeaveCriticalSection(&d->send_cs);
}
static void send_ret_unlink(Dev *d, uint32_t seqnum, int32_t status) {
    uint8_t h[48] = {0};
    put32(h, RET_UNLINK); put32(h + 4, seqnum); put32(h + 20, (uint32_t)status);
    EnterCriticalSection(&d->send_cs);
    send_all(d->client, h, 48);
    LeaveCriticalSection(&d->send_cs);
}

static BOOL is_gone(DWORD e) {
    return e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND || e == ERROR_FILE_NOT_FOUND ||
           e == ERROR_INVALID_HANDLE || e == ERROR_NO_SUCH_DEVICE || e == ERROR_DEV_NOT_EXIST;
}
static int32_t map_err(DWORD e) {
    if (e == ERROR_GEN_FAILURE) return E_PIPE;
    if (e == ERROR_OPERATION_ABORTED || e == ERROR_SEM_TIMEOUT) return E_CONNRESET;
    if (is_gone(e)) return E_SHUTDOWN;
    return E_PROTO;
}
static void device_vanished(Dev *d, DWORD e) {
    if (InterlockedExchange(&d->quit, 1) == 0) {
        say("[%s] device unplugged or stopped responding (error %lu). Ending session with %s.", d->busid, e, d->peer);
        shutdown(d->client, SD_BOTH);
    }
}

static BOOL finish_check(Worker *w, Urb *u) {
    EnterCriticalSection(&w->cs);
    w->inflight = NULL;
    BOOL un = u->unlinked;
    LeaveCriticalSection(&w->cs);
    return un;
}

static void do_control(Worker *w, Urb *u) {
    Dev *d = w->d;
    uint8_t bm = u->setup[0], rq = u->setup[1];
    uint16_t val = u->setup[2] | u->setup[3] << 8, idx = u->setup[4] | u->setup[5] << 8;
    int recip = bm & 0x1F, type = (bm >> 5) & 3;
    logv("[%s] CTRL seq=%u bm=%02X req=%02X val=%04X idx=%04X len=%d", d->busid, u->seqnum, bm, rq, val, idx, u->length);

    if (type == 0 && recip == 0 && rq == 0x09) { send_ret_submit(d, u, 0, 0, NULL); return; }      // SET_CONFIGURATION
    if (type == 0 && recip == 1 && rq == 0x0B) {                                                    // SET_INTERFACE
        BOOL ok = WinUsb_SetCurrentAlternateSetting(h_iface(d, idx & 0xFF), (UCHAR)val);
        send_ret_submit(d, u, ok ? 0 : E_PIPE, 0, NULL); return;
    }
    if (type == 0 && recip == 2 && rq == 0x01 && val == 0) {                                        // CLEAR_FEATURE(HALT)
        BOOL ok = WinUsb_ResetPipe(h_ep(d, (uint8_t)idx), (uint8_t)idx);
        send_ret_submit(d, u, ok ? 0 : E_PIPE, 0, NULL); return;
    }
    WINUSB_INTERFACE_HANDLE h = d->ifh[0];
    if (recip == 1) h = h_iface(d, idx & 0xFF);
    else if (recip == 2) h = h_ep(d, (uint8_t)idx);
    WINUSB_SETUP_PACKET sp = { bm, rq, val, idx, (USHORT)u->length };
    ULONG got = 0;
    ResetEvent(w->ov.hEvent);
    BOOL ok = WinUsb_ControlTransfer(h, sp, u->data, (ULONG)u->length, &got, &w->ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WinUsb_GetOverlappedResult(h, &w->ov, &got, TRUE);
    DWORD err = ok ? 0 : GetLastError();
    if (finish_check(w, u)) { send_ret_unlink(d, u->unlink_seqnum, E_CONNRESET); return; }
    if (!ok) {
        logv("[%s]   ctrl failed err=%lu", d->busid, err);
        if (is_gone(err)) device_vanished(d, err);
        send_ret_submit(d, u, map_err(err), 0, NULL); return;
    }
    send_ret_submit(d, u, 0, (int32_t)got, (bm & 0x80) ? u->data : NULL);
}

static void do_pipe(Worker *w, Urb *u) {
    Dev *d = w->d;
    uint8_t ep = (uint8_t)(u->ep | (u->direction ? 0x80 : 0));
    WINUSB_INTERFACE_HANDLE h = h_ep(d, ep);
    ULONG got = 0;
    ResetEvent(w->ov.hEvent);
    BOOL ok = u->direction ? WinUsb_ReadPipe(h, ep, u->data, (ULONG)u->length, &got, &w->ov)
                           : WinUsb_WritePipe(h, ep, u->data, (ULONG)u->length, &got, &w->ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WinUsb_GetOverlappedResult(h, &w->ov, &got, TRUE);
    DWORD err = ok ? 0 : GetLastError();
    if (finish_check(w, u)) { send_ret_unlink(d, u->unlink_seqnum, E_CONNRESET); return; }
    if (!ok) {
        if (!d->quit) logv("[%s]   ep %02X failed err=%lu", d->busid, ep, err);
        if (is_gone(err)) device_vanished(d, err);
        send_ret_submit(d, u, map_err(err), 0, NULL); return;
    }
    send_ret_submit(d, u, 0, (int32_t)got, u->direction ? u->data : NULL);
}

static DWORD WINAPI worker_main(LPVOID arg) {
    Worker *w = arg;
    for (;;) {
        EnterCriticalSection(&w->cs);
        while (!w->head && !w->d->quit) SleepConditionVariableCS(&w->cv, &w->cs, INFINITE);
        if (w->d->quit) { LeaveCriticalSection(&w->cs); break; }
        Urb *u = w->head; w->head = u->next; if (!w->head) w->tail = NULL;
        w->inflight = u;
        LeaveCriticalSection(&w->cs);
        if (w->key == 0) do_control(w, u); else do_pipe(w, u);
        EnterCriticalSection(&w->cs); w->inflight = NULL; LeaveCriticalSection(&w->cs);
        free(u->data); free(u);
    }
    return 0;
}

static Worker *get_worker(Dev *d, int key) {
    if (!d->workers[key]) {
        Worker *w = calloc(1, sizeof *w);
        w->d = d; w->key = key;
        InitializeCriticalSection(&w->cs);
        InitializeConditionVariable(&w->cv);
        w->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        w->thread = CreateThread(NULL, 0, worker_main, w, 0, NULL);
        d->workers[key] = w;
    }
    return d->workers[key];
}

static void handle_unlink(Dev *d, uint32_t seq, uint32_t target) {
    for (int k = 0; k < 257; k++) {
        Worker *w = d->workers[k];
        if (!w) continue;
        EnterCriticalSection(&w->cs);
        Urb *prev = NULL;
        for (Urb *u = w->head; u; prev = u, u = u->next) {
            if (u->seqnum == target) {
                if (prev) prev->next = u->next; else w->head = u->next;
                if (w->tail == u) w->tail = prev;
                LeaveCriticalSection(&w->cs);
                free(u->data); free(u);
                send_ret_unlink(d, seq, E_CONNRESET);
                return;
            }
        }
        if (w->inflight && w->inflight->seqnum == target) {
            w->inflight->unlink_seqnum = seq;
            w->inflight->unlinked = 1;
            if (k) WinUsb_AbortPipe(h_ep(d, (uint8_t)k), (uint8_t)k);
            LeaveCriticalSection(&w->cs);
            return;
        }
        LeaveCriticalSection(&w->cs);
    }
    send_ret_unlink(d, seq, 0);
}

static void stop_workers(Dev *d) {
    InterlockedExchange(&d->quit, 1);
    for (int k = 1; k < 257; k++) if (d->workers[k]) WinUsb_AbortPipe(h_ep(d, (uint8_t)k), (uint8_t)k);
    for (int k = 0; k < 257; k++) {
        Worker *w = d->workers[k];
        if (!w) continue;
        EnterCriticalSection(&w->cs); WakeAllConditionVariable(&w->cv); LeaveCriticalSection(&w->cs);
        if (WaitForSingleObject(w->thread, 3000) == WAIT_TIMEOUT) { CancelIoEx(d->file, NULL); WaitForSingleObject(w->thread, 2000); }
        CloseHandle(w->thread); CloseHandle(w->ov.hEvent);
        for (Urb *u = w->head; u;) { Urb *n = u->next; free(u->data); free(u); u = n; }
        DeleteCriticalSection(&w->cs);
        free(w); d->workers[k] = NULL;
    }
}

static void urb_loop(Dev *d) {
    SOCKET s = d->client;
    uint8_t h[48];
    while (!d->quit && recv_all(s, h, 48)) {
        uint32_t cmd = get32(h), seq = get32(h + 4), dir = get32(h + 12), ep = get32(h + 16);
        if (cmd == CMD_SUBMIT) {
            Urb *u = calloc(1, sizeof *u);
            u->seqnum = seq; u->direction = dir; u->ep = ep;
            u->length = (int32_t)get32(h + 24);
            int32_t npk = (int32_t)get32(h + 32);
            memcpy(u->setup, h + 40, 8);
            if (u->length < 0 || u->length > 16 * 1024 * 1024) { logm("[%s] bad length %d", d->busid, u->length); free(u); return; }
            u->data = malloc(u->length ? u->length : 1);
            if (!dir && u->length && !recv_all(s, u->data, u->length)) { free(u->data); free(u); return; }
            if (npk > 0) {
                uint8_t *tmp = malloc(npk * 16);
                BOOL ok = recv_all(s, tmp, npk * 16); free(tmp);
                logm("[%s] isochronous transfer refused (not supported yet)", d->busid);
                send_ret_submit(d, u, E_NOSYS, 0, NULL); free(u->data); free(u);
                if (!ok) return;
                continue;
            }
            Worker *w = get_worker(d, ep == 0 ? 0 : (int)((ep & 0x7F) | (dir ? 0x80 : 0)));
            EnterCriticalSection(&w->cs);
            if (w->tail) w->tail->next = u; else w->head = u;
            w->tail = u;
            WakeConditionVariable(&w->cv);
            LeaveCriticalSection(&w->cs);
        } else if (cmd == CMD_UNLINK) {
            handle_unlink(d, seq, get32(h + 20));
        } else {
            logm("[%s] unknown command %u", d->busid, cmd); return;
        }
    }
}

// ================================================================ connections
typedef struct { SOCKET s; char peer[64]; } Conn;

static Dev *find_shared(const char *busid) {
    for (int i = 0; i < MAX_DEVS; i++) {
        Dev *d = &g_devs[i];
        if (d->used && d->present && d->wanted && !d->blocked && d->have_desc && !strcmp(d->busid, busid)) return d;
    }
    return NULL;
}

static DWORD WINAPI conn_main(LPVOID arg) {
    Conn *c = arg;
    SOCKET s = c->s;
    uint8_t op[8];
    if (!recv_all(s, op, 8)) goto done;
    uint16_t code = op[2] << 8 | op[3];

    if (code == OP_REQ_DEVLIST) {
        refresh_devices();
        EnterCriticalSection(&g_devs_cs);
        int n = 0, size = 12;
        for (int i = 0; i < MAX_DEVS; i++) { Dev *d = &g_devs[i]; if (d->used && d->present && d->wanted && !d->blocked && d->have_desc && !d->in_use) { n++; size += 312 + 4 * d->num_ifaces; } }
        uint8_t *r = calloc(1, size), *p = r + 12;
        put16(r, USBIP_VERSION); put16(r + 2, OP_REP_DEVLIST); put32(r + 8, n);
        for (int i = 0; i < MAX_DEVS; i++) {
            Dev *d = &g_devs[i];
            if (!(d->used && d->present && d->wanted && !d->blocked && d->have_desc && !d->in_use)) continue;
            fill_device_record(d, p); p += 312;
            for (int k = 0; k < d->num_ifaces; k++) { memcpy(p, d->iface_class[k], 3); p += 4; }
        }
        LeaveCriticalSection(&g_devs_cs);
        send_all(s, r, size); free(r);
        logm("%s listed devices (%d available)", c->peer, n);
        goto done;
    }
    if (code == OP_REQ_IMPORT) {
        char busid[33] = {0};
        if (!recv_all(s, busid, 32)) goto done;
        uint8_t r[8 + 312] = {0};
        put16(r, USBIP_VERSION); put16(r + 2, OP_REP_IMPORT);
        refresh_devices();
        EnterCriticalSection(&g_devs_cs);
        Dev *d = find_shared(busid);
        if (!d || InterlockedCompareExchange(&d->in_use, 1, 0) != 0) {
            LeaveCriticalSection(&g_devs_cs);
            put32(r + 4, ST_NA); send_all(s, r, 8);
            logm("%s asked for %s: not available", c->peer, busid);
            goto done;
        }
        LeaveCriticalSection(&g_devs_cs);
        d->quit = 0;
        if (!d->on_winusb) {
            say("[%s] %s wants %ls: switching it to the sharing driver...", d->busid, c->peer, d->name);
            InterlockedExchange(&d->busy, 1);
            BOOL ok = share_device(d);
            InterlockedExchange(&d->busy, 0);
            if (!ok) {
                put32(r + 4, ST_NA); send_all(s, r, 8);
                InterlockedExchange(&d->in_use, 0);
                goto done;
            }
        }
        if (!open_usb(d)) {
            put32(r + 4, ST_NA); send_all(s, r, 8);
            InterlockedExchange(&d->in_use, 0);
            say("[%s] could not open the device for %s", busid, c->peer);
            goto done;
        }
        fill_device_record(d, r + 8);
        send_all(s, r, sizeof r);
        strncpy(d->peer, c->peer, 63);
        strncpy(d->last_peer, c->peer, 63);
        d->client = s;
        say("[%s] %ls is now in use by %s", d->busid, d->name, c->peer);
        urb_loop(d);
        stop_workers(d);
        close_usb(d);
        d->client = INVALID_SOCKET;
        say("[%s] %ls released by %s", d->busid, d->name, c->peer);
        InterlockedExchange(&d->busy, 1);
        refresh_devices();
        if (d->present) {
            revert_driver(d);
            say("[%s] %ls is working on this PC again.", d->busid, d->name);
        } else {
            d->returning = 1; d->blocked = 1;   // unplugged: give it back to this PC when it returns
        }
        InterlockedExchange(&d->busy, 0);
        InterlockedExchange(&d->in_use, 0);
        d->quit = 0;
    } else {
        logm("unknown request 0x%04X from %s", code, c->peer);
    }
done:
    closesocket(s);
    free(c);
    return 0;
}

static DWORD WINAPI server_main(LPVOID arg) {
    SOCKET ls = (SOCKET)(uintptr_t)arg;
    for (;;) {
        struct sockaddr_in ca; int cl = sizeof ca;
        SOCKET s = accept(ls, (struct sockaddr *)&ca, &cl);
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
        struct tcp_keepalive ka = { 1, 10000, 2000 };   // detect a dead network in ~20 s
        DWORD br; WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof ka, NULL, 0, &br, NULL, NULL);
        Conn *c = calloc(1, sizeof *c);
        c->s = s;
        inet_ntop(AF_INET, &ca.sin_addr, c->peer, sizeof c->peer);
        CloseHandle(CreateThread(NULL, 0, conn_main, c, 0, NULL));
    }
    return 0;
}

// ================================================================ settings file
static char g_cfg_path[MAX_PATH];

static void save_config(void) {
    FILE *f = fopen(g_cfg_path, "w");
    if (!f) return;
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].wanted) fprintf(f, "%d|%ls\n", g_devs[i].auto_rc, g_devs[i].inst);
    fclose(f);
}

static void load_config(void) {
    FILE *f = fopen(g_cfg_path, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        int a = 0; char inst[512] = "";
        if (sscanf(line, "%d|%511[^\r\n]", &a, inst) != 2) continue;
        wchar_t w[256]; MultiByteToWideChar(CP_UTF8, 0, inst, -1, w, 256);
        Dev *d = dev_slot(w);
        if (d) { d->wanted = 1; d->auto_rc = a; }
    }
    fclose(f);
}

// ================================================================ background monitor
// Once a second: return replugged devices to this PC, run reconnect
// countdowns, and fix any device left on the sharing driver.
static int g_view[MAX_DEVS], g_view_n;
static int view_number(Dev *d) {
    for (int i = 0; i < g_view_n; i++) if (&g_devs[g_view[i]] == d) return i + 1;
    return 0;
}

static DWORD WINAPI monitor_main(LPVOID arg) {
    for (;;) {
        Sleep(1000);
        refresh_devices();
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < MAX_DEVS; i++) {
            Dev *d = &g_devs[i];
            if (!d->used || d->in_use || d->busy) continue;
            if (d->present && d->on_winusb) {            // left on sharing driver: give it back
                InterlockedExchange(&d->busy, 1);
                if (d->returning) say("[%s] %ls was plugged back in. Returning it to this PC...", d->busid, d->name);
                revert_driver(d);
                if (d->returning) {
                    d->returning = 0;
                    if (d->wanted && d->auto_rc) {
                        d->hold_until = GetTickCount64() + 10000; d->last_shown = 0;
                        say("[%s] %ls works on this PC now. It will reconnect to %s in 10 seconds. Type C %d to cancel.",
                            d->busid, d->name, d->last_peer, view_number(d));
                    } else {
                        say("[%s] %ls works on this PC now and will stay here. Type S %d to share it again.", d->busid, d->name, view_number(d));
                    }
                }
                InterlockedExchange(&d->busy, 0);
                continue;
            }
            if (d->hold_until) {
                if (now >= d->hold_until) {
                    d->hold_until = 0; d->blocked = 0;
                    say("[%s] Reconnect countdown finished: %ls is available to %s again.", d->busid, d->name, d->last_peer);
                } else {
                    int left = (int)((d->hold_until - now + 999) / 1000);
                    if (left != d->last_shown && (left <= 5 || left % 5 == 0)) {
                        d->last_shown = left;
                        say("[%s] Reconnecting %ls in %d s... (C %d to cancel)", d->busid, d->name, left, view_number(d));
                    }
                }
            }
        }
    }
    return 0;
}

// ================================================================ console UI
static void print_menu(void) {
    refresh_devices();
    EnterCriticalSection(&g_devs_cs);
    printf("\n================ USB devices on this PC ================\n");
    g_view_n = 0;
    for (int i = 0; i < MAX_DEVS; i++) {
        Dev *d = &g_devs[i];
        if (!d->used || !d->present || !d->removable) continue;
        g_view[g_view_n++] = i;
        char st[128];
        if (d->busy) snprintf(st, sizeof st, "switching driver...");
        else if (d->in_use) snprintf(st, sizeof st, "IN USE by %s", d->peer);
        else if (!d->wanted) snprintf(st, sizeof st, "not shared");
        else if (d->hold_until) snprintf(st, sizeof st, "reconnecting soon (C to cancel)");
        else if (d->blocked) snprintf(st, sizeof st, "kept on this PC (S to share again)");
        else snprintf(st, sizeof st, "shared - works here until a client connects");
        printf(" %2d) %04X:%04X  %-28.28ls %-44s auto-reconnect: %s\n", g_view_n, d->vid, d->pid, d->name, st, d->wanted ? (d->auto_rc ? "ON" : "off") : "-");
    }
    if (!g_view_n) printf(" (no removable USB devices found)\n");
    LeaveCriticalSection(&g_devs_cs);
    printf("--------------------------------------------------------\n");
    printf(" S <n> share          U <n> stop sharing (returns it to this PC)\n");
    printf(" A <n> auto-reconnect on/off          C <n> cancel a reconnect countdown\n");
    printf(" R     refresh list   Q     quit\n");
    printf("> ");
    fflush(stdout);
}

static Dev *pick(int n) {
    if (n < 1 || n > g_view_n) { printf("  No device number %d.\n", n); return NULL; }
    return &g_devs[g_view[n - 1]];
}

int main(int argc, char **argv) {
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_devs_cs);
    InitializeCriticalSection(&g_hubdesc_cs);
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) g_verbose = 1;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *sl = strrchr(path, '\\'); if (sl) strcpy(sl + 1, "usbeam_log.txt");
    g_log = fopen(path, "a");
    if (sl) strcpy(sl + 1, "usbeam_settings.txt");
    strcpy(g_cfg_path, path);
    logm("======== USBeam host started (test version 6)");

    HMODULE nd = LoadLibraryW(L"newdev.dll");
    pDiInstallDevice = nd ? (DiInstallDevice_t)GetProcAddress(nd, "DiInstallDevice") : NULL;
    pDiUninstallDevice = nd ? (DiUninstallDevice_t)GetProcAddress(nd, "DiUninstallDevice") : NULL;
    if (!pDiInstallDevice || !pDiUninstallDevice) { say("ERROR: newdev.dll functions missing."); getchar(); return 1; }

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&excl, sizeof excl);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(USBIP_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) {
        say("ERROR: cannot use network port %d (%d). Close any other copy of this program.", USBIP_PORT, WSAGetLastError());
        getchar(); return 1;
    }
    char host[256] = ""; gethostname(host, sizeof host);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    printf("USBeam host for Windows - test version 6\nThis PC's network addresses:");
    if (!getaddrinfo(host, NULL, &hints, &res)) {
        for (struct addrinfo *p = res; p; p = p->ai_next) {
            char ip[64]; inet_ntop(AF_INET, &((struct sockaddr_in *)p->ai_addr)->sin_addr, ip, sizeof ip);
            printf("  %s", ip);
        }
        freeaddrinfo(res);
    }
    printf("\nShared devices are offered on port %d.\n", USBIP_PORT);

    load_config();
    refresh_devices();
    // Safety net: anything left on the sharing driver (crash, power loss) goes back to this PC.
    for (int i = 0; i < MAX_DEVS; i++) {
        Dev *d = &g_devs[i];
        if (d->used && d->present && d->on_winusb) { say("Returning %ls to this PC (was left on the sharing driver).", d->name); revert_driver(d); }
    }
    CloseHandle(CreateThread(NULL, 0, server_main, (LPVOID)(uintptr_t)ls, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, monitor_main, NULL, 0, NULL));

    char line[128];
    for (;;) {
        print_menu();
        if (!fgets(line, sizeof line, stdin)) break;
        char c = 0; int n = 0;
        sscanf(line, " %c %d", &c, &n);
        c = (char)toupper((unsigned char)c);
        if (c == 'Q') break;
        if (c == 'R' || c == 0) continue;
        if (!strchr("SUAC", c)) { printf("  Unknown command.\n"); continue; }
        Dev *d = pick(n);
        if (!d) continue;
        if (c == 'S') {
            if (d->wanted && !d->blocked && !d->hold_until) { printf("  Already shared.\n"); continue; }
            d->wanted = 1; d->blocked = 0; d->hold_until = 0;
            if (!d->have_desc) hub_descriptors(d);
            save_config();
            say("%ls is now shared. It keeps working on this PC until a client connects to it.", d->name);
        } else if (c == 'U') {
            if (!d->wanted) { printf("  That device is not shared.\n"); continue; }
            d->wanted = 0; d->auto_rc = 0; d->blocked = 0; d->hold_until = 0; d->returning = 0;
            save_config();
            if (d->in_use) { say("Disconnecting %s from %ls...", d->peer, d->name); end_session(d); }
            say("%ls is no longer shared and stays on this PC.", d->name);
        } else if (c == 'A') {
            if (!d->wanted) { printf("  Share the device first (S %d).\n", n); continue; }
            d->auto_rc = !d->auto_rc;
            save_config();
            say("Auto-reconnect for %ls is now %s.", d->name, d->auto_rc ? "ON" : "off");
        } else if (c == 'C') {
            if (!d->hold_until) { printf("  No countdown running for that device.\n"); continue; }
            d->hold_until = 0; d->blocked = 1;
            say("Reconnect cancelled. %ls stays on this PC. Type S %d to share it again.", d->name, n);
        }
    }
    logm("======== USBeam host exiting");
    return 0;
}
