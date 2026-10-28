// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the USBeam contributors
//
// usbeam_app.cpp - the USBeam app for Windows: tray icon + a native window that
// shows USBeam's screen (served by the USBeam service on 127.0.0.1:3242) using
// Windows' built-in WebView2 component. No browser is opened.
//
//   USBeam.exe          open the window (or bring the running one forward)
//   USBeam.exe --tray   start hidden in the tray (used at sign-in)

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string>
#include <functional>
#include "WebView2.h"

#define UI_PORT 3242
#define WM_TRAY (WM_APP + 1)
#define WM_SHOW_APP (WM_APP + 2)
#define TIMER_CHECK 1

static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbar_created;
static ICoreWebView2Controller *g_ctl;
static ICoreWebView2 *g_web;
static std::string g_token;
static bool g_webview_failed;

// ---------------------------------------------------------------- small COM helper: callback objects
template <class I, class... Args>
class Callback final : public I {
    LONG ref = 1;
    std::function<HRESULT(Args...)> fn;
public:
    explicit Callback(std::function<HRESULT(Args...)> f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        // These objects only ever stand for the one handler interface they implement; refuse
        // the marshaling queries COM may make, accept the rest.
        static const GUID IID_IAgileObject_ = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
        static const GUID IID_INoMarshal_ = { 0xecc8691b, 0xc1db, 0x4dc0, { 0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49 } };
        static const GUID IID_IMarshal_ = { 0x00000003, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        if (IsEqualGUID(riid, IID_IMarshal_) || IsEqualGUID(riid, IID_IAgileObject_) || IsEqualGUID(riid, IID_INoMarshal_)) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<I *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE Invoke(Args... a) override { return fn(a...); }
};

// ---------------------------------------------------------------- service connection
static std::string read_token() {
    wchar_t pd[MAX_PATH], p[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) wcscpy(pd, L"C:\\ProgramData");
    _snwprintf(p, MAX_PATH, L"%ls\\USBeam\\ui-token", pd);
    FILE *f = _wfopen(p, L"r");
    if (!f) return "";
    char b[64] = "";
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    std::string t(b);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    return t;
}

// Tell the service something (used when Windows shuts down). Best effort.
static void post_action(const char *json) {
    std::string tok = read_token();
    if (tok.empty()) return;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET; a.sin_port = htons(UI_PORT); a.sin_addr.s_addr = htonl(0x7F000001);
    DWORD to = 2000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    if (connect(s, (sockaddr *)&a, sizeof a) == 0) {
        char req[512];
        int n = snprintf(req, sizeof req, "POST /api/action HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-USBeam-Token: %s\r\n"
                         "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                         UI_PORT, tok.c_str(), (int)strlen(json), json);
        send(s, req, n, 0);
        char buf[256]; recv(s, buf, sizeof buf, 0);
    }
    closesocket(s);
}

static bool system_dark() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

static void apply_title_bar_theme() {
    BOOL dark = system_dark();
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
}

static const wchar_t *WAIT_PAGE =
    L"<!doctype html><html><head><meta charset='utf-8'><style>"
    L"html,body{margin:0;height:100%;font:14px 'Segoe UI Variable Text','Segoe UI',sans-serif;background:#f4f5f7;color:#161a21}"
    L"@media (prefers-color-scheme:dark){html,body{background:#0f1217;color:#e6e9ef}}"
    L"div{display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%;gap:8px}"
    L"b{font-size:17px}span{opacity:.7}</style></head><body><div><b>USBeam is starting…</b>"
    L"<span>Waiting for the USBeam service. If this doesn't go away, reinstall USBeam.</span></div></body></html>";

static void navigate_app() {
    if (!g_web) return;
    g_token = read_token();
    if (g_token.empty()) { g_web->NavigateToString(WAIT_PAGE); return; }
    wchar_t url[160];
    _snwprintf(url, 160, L"http://127.0.0.1:%d/?t=%hs", UI_PORT, g_token.c_str());
    g_web->Navigate(url);
}

// ---------------------------------------------------------------- WebView2
typedef HRESULT (STDAPICALLTYPE *CreateEnv_t)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static void start_webview() {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) *sl = 0;
    std::wstring loader = std::wstring(dir) + L"\\WebView2Loader.dll";
    HMODULE m = LoadLibraryW(loader.c_str());
    CreateEnv_t create = m ? (CreateEnv_t)GetProcAddress(m, "CreateCoreWebView2EnvironmentWithOptions") : nullptr;
    if (!create) { g_webview_failed = true; return; }

    wchar_t data[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, data);
    std::wstring udf = std::wstring(data) + L"\\USBeam\\WebView2";
    // Match the page background so there is no white flash while loading.
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", system_dark() ? L"FF0F1217" : L"FFF4F5F7");

    HRESULT hr = create(nullptr, udf.c_str(), nullptr,
        new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment *>(
        [](HRESULT r, ICoreWebView2Environment *env) -> HRESULT {
            if (FAILED(r) || !env) { g_webview_failed = true; return S_OK; }
            env->CreateCoreWebView2Controller(g_hwnd,
                new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller *>(
                [](HRESULT r2, ICoreWebView2Controller *ctl) -> HRESULT {
                    if (FAILED(r2) || !ctl) { g_webview_failed = true; return S_OK; }
                    g_ctl = ctl; g_ctl->AddRef();
                    g_ctl->get_CoreWebView2(&g_web);
                    ICoreWebView2Settings *st = nullptr;
                    if (SUCCEEDED(g_web->get_Settings(&st)) && st) {
                        st->put_AreDefaultContextMenusEnabled(FALSE);
                        st->put_AreDevToolsEnabled(FALSE);
                        st->put_IsStatusBarEnabled(FALSE);
                        st->put_IsZoomControlEnabled(FALSE);
                        st->put_AreDefaultScriptDialogsEnabled(TRUE);
                        st->Release();
                    }
                    RECT rc; GetClientRect(g_hwnd, &rc);
                    g_ctl->put_Bounds(rc);
                    navigate_app();
                    return S_OK;
                }));
            return S_OK;
        }));
    if (FAILED(hr)) g_webview_failed = true;
}

// ---------------------------------------------------------------- tray + window
static void tray_add() {
    g_nid = {};
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"USBeam");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void show_window() {
    if (g_webview_failed) {
        int r = MessageBoxW(nullptr, L"USBeam needs the Microsoft Edge WebView2 Runtime, which is part of Windows 11.\n\nOpen the download page now?",
                            L"USBeam", MB_YESNO | MB_ICONWARNING);
        if (r == IDYES) ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    else ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (g_ctl) g_ctl->put_IsVisible(TRUE);
    if (g_web && g_token != read_token()) navigate_app();
}

static void hide_window() {
    ShowWindow(g_hwnd, SW_HIDE);
    if (g_ctl) g_ctl->put_IsVisible(FALSE);     // lets WebView2 save power while hidden
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_ctl) { RECT rc; GetClientRect(h, &rc); g_ctl->put_Bounds(rc); }
        return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(h);
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(560, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(420, dpi, 96);
        return 0;
    }
    case WM_CLOSE:
        hide_window();               // closing keeps USBeam in the tray
        return 0;
    case WM_SETTINGCHANGE:
        apply_title_bar_theme();
        break;
    case WM_TIMER:
        if (wp == TIMER_CHECK && g_web && IsWindowVisible(h) && g_token != read_token()) navigate_app();   // service restarted
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) show_window();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Open USBeam");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 2, L"Close this app (sharing keeps running)");
            SetMenuDefaultItem(m, 1, FALSE);
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            if (cmd == 1) show_window();
            else if (cmd == 2) DestroyWindow(h);
        }
        return 0;
    case WM_SHOW_APP:
        show_window();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        // Shut down or restart (including with Fast Startup): auto-reconnect choices are not kept.
        // Sleep and hibernate don't end the session, so the choices stay.
        if (wp) post_action("{\"op\":\"forget_auto\"}");
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_ctl) { g_ctl->Close(); g_ctl->Release(); g_ctl = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbar_created && g_taskbar_created) { tray_add(); return 0; }     // Explorer restarted
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    bool tray_only = cmd && wcsstr(cmd, L"--tray");
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\USBeamAppWindow");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {        // already running: bring it forward
        HWND other = FindWindowW(L"USBeamAppWindow", nullptr);
        if (other && !tray_only) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(other, WM_SHOW_APP, 0, 0); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(system_dark() ? RGB(15, 18, 23) : RGB(244, 245, 247));
    wc.lpszClassName = L"USBeamAppWindow";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1100, dpi, 96), hgt = MulDiv(760, dpi, 96);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (hgt > wa.bottom - wa.top) hgt = wa.bottom - wa.top;
    g_hwnd = CreateWindowExW(0, L"USBeamAppWindow", L"USBeam", WS_OVERLAPPEDWINDOW,
                             wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt,
                             nullptr, nullptr, inst, nullptr);
    apply_title_bar_theme();
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    tray_add();
    start_webview();
    SetTimer(g_hwnd, TIMER_CHECK, 3000, nullptr);
    if (!tray_only) show_window();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return 0;
}
