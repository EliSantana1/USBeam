// SPDX-License-Identifier: GPL-3.0-or-later
// Windows smooth-input test, receiving side: reads the test device from tests/fakehid.c through
// Windows' own HID driver, like any input device, and measures reports per second, how even their
// timing is, and that every report arrives once, in order.
//   winhid_measure <count> <label>     (waits up to 30 s for the device to appear)
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static double now_ms(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static HANDLE open_dev(void) {
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    HANDLE h = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf; det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof buf, NULL, NULL)) continue;
        if (!wcsstr(det->DevicePath, L"vid_1209&pid_0001")) continue;
        h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    const char *label = argc > 2 ? argv[2] : "";
    timeBeginPeriod(1);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 300 && (h = open_dev()) == INVALID_HANDLE_VALUE; i++) Sleep(100);
    if (h == INVALID_HANDLE_VALUE) { printf("%s: the test device didn't appear\n", label); return 1; }
    HidD_SetNumInputBuffers(h, 512);
    double *t = calloc((size_t)n, sizeof *t), *g = calloc((size_t)n, sizeof *g);
    uint32_t expect = 0; int in_order = 1, k = 0, first = 1;
    double start = now_ms();
    while (k < n && now_ms() - start < 60000) {
        uint8_t r[9]; DWORD got = 0;
        if (!ReadFile(h, r, sizeof r, &got, NULL) || got < 5) { printf("%s: read failed (%lu)\n", label, GetLastError()); break; }
        uint32_t c; memcpy(&c, r + 1, 4);
        if (first) { expect = c; first = 0; }            // reports made before we started reading are skipped
        if (c != expect) in_order = 0;
        expect = c + 1;
        t[k++] = now_ms();
    }
    CloseHandle(h);
    if (k < 10) { printf("%s: only %d reports arrived\n", label, k); return 1; }
    for (int i = 1; i < k; i++) g[i - 1] = t[i] - t[i - 1];
    qsort(g, (size_t)(k - 1), sizeof *g, cmpd);
    double rate = (k - 1) / ((t[k - 1] - t[0]) / 1000.0);
    #define Q(f) g[(int)((f) * (k - 2))]
    printf("%s: %.0f reports/s, gap ms median %.2f p90 %.2f p99 %.2f max %.2f; arrived %d/%d; in order, none missing: %s\n",
           label, rate, Q(0.5), Q(0.9), Q(0.99), g[k - 2], k, n, in_order ? "yes" : "NO");
    printf("NUMBERS %.0f %.2f %.2f %d\n", rate, Q(0.9), Q(0.99), in_order && k == n);
    return 0;
}
