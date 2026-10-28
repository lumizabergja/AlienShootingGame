#include "engine.h"
#include "preset_c.h"
#include "diagnostics.h"
#include <commctrl.h>
#include <memory>
#include <cerrno>
#include <cmath>
#include <cwchar>

namespace {
constexpr int Target = 101, Refresh = 102, Multiplier = 103, CenterRadius = 105,
              CaptureFps = 112, Start = 107, Stop = 108, Status = 109;

HWND window{}, output{}, targetCombo{}, status{};
HFONT uiFont{};
std::vector<HWND> targets;
std::thread worker;
std::unique_ptr<SessionControl> control;
HWND game{};
bool closing = false;
bool resolutionRestartPending = false;
PresetCResult presetC{};
uint64_t lastPerfVersion = 0;
ULONGLONG nextMonitorCheck = 0;
HWINEVENTHOOK foregroundHook{}, minimizeHook{};

void updateOutputState() {
    if (!control || !output)
        return;
    DWORD gp = 0, fp = 0;
    GetWindowThreadProcessId(game, &gp);
    GetWindowThreadProcessId(GetForegroundWindow(), &fp);
    bool focused = gp && gp == fp && !IsIconic(game);
    control->focused = focused;
    if (!focused)
        control->hasFrame = false;
    bool visible = focused && control->hasFrame && !control->stop;
    if (visible && !IsWindowVisible(output))
        SetWindowPos(output, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!visible && IsWindowVisible(output))
        ShowWindow(output, SW_HIDE);
}

void CALLBACK focusChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    updateOutputState();
}

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style,
         int x, int y, int w, int h, int id) {
    auto v = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(v, WM_SETFONT, (WPARAM)uiFont, TRUE);
    return v;
}

void refresh() {
    SendMessageW(targetCombo, CB_RESETCONTENT, 0, 0);
    targets.clear();
    EnumWindows(
        [](HWND w, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER))
                return TRUE;
            wchar_t title[512];
            if (GetWindowTextW(w, title, 512) < 1)
                return TRUE;
            targets.push_back(w);
            SendMessageW(targetCombo, CB_ADDSTRING, 0, (LPARAM)title);
            return TRUE;
        },
        0);
    if (!targets.empty())
        SendMessageW(targetCombo, CB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK outputProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    if (m == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (m == WM_SETCURSOR)
        return TRUE;
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(w, m, a, b);
}

void setRunning(bool running) {
    for (int id : {Target, Refresh, Multiplier, CenterRadius, CaptureFps})
        EnableWindow(GetDlgItem(window, id), !running);
    EnableWindow(GetDlgItem(window, Start), !running);
    EnableWindow(GetDlgItem(window, Stop), running);
}

void start() {
    if (worker.joinable())
        return;
    int selected = int(SendMessageW(targetCombo, CB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= int(targets.size()))
        return;
    game = targets[selected];
    if (!IsWindow(game))
        return;

    Options options;
    options.multiplier = UINT(SendDlgItemMessageW(window, Multiplier, CB_GETCURSEL, 0, 0)) + 1;
    BOOL valid = FALSE;
    UINT bypass = GetDlgItemInt(window, CenterRadius, &valid, FALSE);
    if (!valid) {
        SetWindowTextW(status, L"Center bypass: enter 0 or a positive pixel value.");
        return;
    }
    options.centerRadius = bypass;
    wchar_t fpsText[32]{};
    GetDlgItemTextW(window, CaptureFps, fpsText, 32);
    errno = 0;
    wchar_t *end = nullptr;
    const double fps = std::wcstod(fpsText, &end);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (end == fpsText || (end && *end) || errno == ERANGE ||
        !std::isfinite(fps) || fps < 0.0 || fps > 1000.0 ||
        (fps > 0.0 && fps < 0.001)) {
        SetWindowTextW(status, L"Capture FPS: enter 0 (unlimited), or 0.001 to 1000. Example: 75");
        diagnostics::write("Capture FPS rejected: invalid field");
        return;
    }
    options.captureFps = fps;
    options.vsync = false;
    options.gpuProfile = false;
    options.opticalFlow = true;
    options.opticalFlowPercent = 10u;
    diagnostics::write("Capture FPS target=" + std::to_string(fps) +
                       " NVOFA=4x4 FAST scale=10% FG multiplier=" +
                       std::to_string(options.multiplier));

    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof monitorInfo;
    if (GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            diagnostics::write("Selected monitor desktop mode: " + std::to_string(dm.dmPelsWidth) +
                               "x" + std::to_string(dm.dmPelsHeight) +
                               " refresh_hz=" + std::to_string(dm.dmDisplayFrequency));
    }
    // The presenter is fully opaque; the native center bypass is still a real Win32
    // window region.  For a top-level overlay over another process, WS_EX_TRANSPARENT
    // alone is not reliable input pass-through.  Pair it with WS_EX_LAYERED at full
    // opacity so mouse input is passed to the game underneath while visuals stay opaque.
    output = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"FrameGeneration.Output", L"FrameGeneration", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!output) {
        SetWindowTextW(status, L"Cannot create output window.");
        return;
    }
    // Keep the layered window visually fully opaque.  WS_EX_LAYERED +
    // WS_EX_TRANSPARENT provides true cross-process mouse pass-through.
    if (!SetLayeredWindowAttributes(output, 0, 255, LWA_ALPHA)) {
        SetWindowTextW(status, L"Cannot enable click-through output window.");
        DestroyWindow(output);
        output = nullptr;
        return;
    }

    // Literal native center bypass: clip the FG output window itself so the requested
    // square is not covered by the Streamline/DLSS-G presenter at all. The selected
    // game remains directly visible through this hole, keeping a centered crosshair
    // native on base and generated presents alike. This avoids DLSS-G UI interpolation
    // and avoids the full-width/full-height seams required by rectangular subrects.
    if (options.centerRadius) {
        const int ow = mi.rcMonitor.right - mi.rcMonitor.left;
        const int oh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        const int r = int(options.centerRadius);
        const int cx = ow / 2, cy = oh / 2;
        if (r >= cx || r >= cy) {
            SetWindowTextW(status, L"Center bypass is too large for this display.");
            DestroyWindow(output);
            output = nullptr;
            return;
        }
        HRGN visible = CreateRectRgn(0, 0, ow, oh);
        HRGN hole = CreateRectRgn(cx - r, cy - r, cx + r, cy + r);
        if (!visible || !hole || CombineRgn(visible, visible, hole, RGN_DIFF) == ERROR ||
            SetWindowRgn(output, visible, FALSE) == 0) {
            if (visible) DeleteObject(visible);
            if (hole) DeleteObject(hole);
            SetWindowTextW(status, L"Cannot create native center bypass.");
            DestroyWindow(output);
            output = nullptr;
            return;
        }
        // SetWindowRgn owns 'visible' on success.
        DeleteObject(hole);
    }
    SetWindowDisplayAffinity(output, 0x11);

    control = std::make_unique<SessionControl>();
    control->expectedMonitor = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    control->expectedMonitorRect = mi.rcMonitor;
    lastPerfVersion = 0;
    nextMonitorCheck = GetTickCount64() + 1000;
    control->report(L"Starting...");
    setRunning(true);

    SetForegroundWindow(game);
    DWORD pid = 0, fgPid = 0;
    GetWindowThreadProcessId(game, &pid);
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    control->focused = pid == fgPid;
    auto *c = control.get();
    auto t = game, o = output;
    worker = std::thread([t, o, options, c] {
        runPresenter(t, o, options, *c);
        PostMessageW(window, WM_APP + 1, 0, 0);
    });
}

LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_CREATE: {
        window = w;
        uiFont = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!uiFont)
            uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        add(L"STATIC", L"Game", SS_LEFT | SS_NOPREFIX, 12, 15, 92, 20, 0);
        targetCombo = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                          108, 10, 245, 260, Target);
        add(L"BUTTON", L"Refresh", 0, 361, 9, 70, 24, Refresh);

        add(L"STATIC", L"FG", SS_LEFT | SS_NOPREFIX, 12, 50, 92, 20, 0);
        auto fg = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 108, 45, 110, 180, Multiplier);
        for (auto text : {L"Off", L"2x", L"3x", L"4x", L"5x", L"6x"})
            SendMessageW(fg, CB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(fg, CB_SETCURSEL, 1, 0);

        add(L"STATIC", L"Capture FPS", SS_LEFT | SS_NOPREFIX, 12, 85, 145, 20, 0);
        auto fpsEdit = add(L"EDIT", L"120", WS_BORDER | ES_AUTOHSCROLL,
                           170, 80, 100, 24, CaptureFps);
        SendMessageW(fpsEdit, EM_SETLIMITTEXT, 12, 0);

        add(L"STATIC", L"Center bypass (px)", SS_LEFT | SS_NOPREFIX, 12, 119, 150, 20, 0);
        auto bypass = add(L"EDIT", L"10", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                          170, 114, 70, 24, CenterRadius);
        SendMessageW(bypass, EM_SETLIMITTEXT, 4, 0);

        add(L"STATIC", L"NVOFA: 4x4 FAST, 10% fixed (zero-motion fallback)",
            SS_LEFT | SS_NOPREFIX, 12, 148, 400, 20, 0);

        add(L"BUTTON", L"Start", BS_DEFPUSHBUTTON, 12, 183, 205, 30, Start);
        add(L"BUTTON", L"Stop", 0, 226, 183, 205, 30, Stop);
        EnableWindow(GetDlgItem(w, Stop), FALSE);

        add(L"STATIC", L"Status", SS_LEFT | SS_NOPREFIX, 12, 230, 92, 20, 0);
        status = add(L"STATIC", presetC.configured ? L"Ready - Preset C profile saved" :
                     L"Ready - Preset C unavailable (see PRESET-C.txt)",
                     SS_LEFT | SS_NOPREFIX, 108, 227, 323, 52, Status);

        refresh();
        SetTimer(w, 1, 50, nullptr);
        foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                        nullptr, focusChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
        minimizeHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, focusChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
        RegisterHotKey(w, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        auto dc = (HDC)a;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        switch (LOWORD(a)) {
        case Refresh:
            refresh();
            break;
        case Start:
            start();
            break;
        case Stop:
            resolutionRestartPending = false;
            if (control) {
                control->stop = true;
                control->hasFrame = false;
                ShowWindow(output, SW_HIDE);
            }
            break;
        }
        return 0;

    case WM_TIMER:
        if (a == 2) {
            KillTimer(w, 2);
            if (resolutionRestartPending && !worker.joinable() && IsWindow(game)) {
                resolutionRestartPending = false;
                start();
                if (!worker.joinable())
                    setRunning(false);
            }
            return 0;
        }
        if (control) {
            updateOutputState();
            auto now = GetTickCount64();
            if (!control->stop && now >= nextMonitorCheck) {
                nextMonitorCheck = now + 1000;
                MONITORINFO current{};
                current.cbSize = sizeof current;
                HMONITOR monitor = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
                if (monitor != control->expectedMonitor || !GetMonitorInfoW(monitor, &current) ||
                    memcmp(&current.rcMonitor, &control->expectedMonitorRect, sizeof(RECT))) {
                    resolutionRestartPending = true;
                    control->resolutionChanged.store(true, std::memory_order_release);
                    control->stop = true;
                    control->hasFrame = false;
                    ShowWindow(output, SW_HIDE);
                    control->report(L"Resolution changed. Rebuilding capture + DLSS-G...");
                }
            }
            if (control->perfActive.load(std::memory_order_acquire)) {
                uint64_t v1 = control->perf.version.load(std::memory_order_acquire);
                if (v1 && v1 != lastPerfVersion) {
                    uint32_t base = control->perf.baseFpsMilli.load(std::memory_order_relaxed);
                    uint32_t presents = control->perf.presentFpsMilli.load(std::memory_order_relaxed);
                    uint32_t age = control->perf.ageUs.load(std::memory_order_relaxed);
                    uint64_t dropped = control->perf.dropped.load(std::memory_order_relaxed);
                    uint32_t sdk = control->perf.dlssStatus.load(std::memory_order_relaxed);
                    uint32_t radius = control->perf.centerRadius.load(std::memory_order_relaxed);
                    uint64_t v2 = control->perf.version.load(std::memory_order_acquire);
                    if (v1 == v2) {
                        wchar_t text[360];
                        if (radius)
                            swprintf(text, 360, L"%.1f base | %.1f present | %.2f ms age | drop %llu\nDLSS 0x%X | bypass %ux%u",
                                     base / 1000.0, presents / 1000.0, age / 1000.0,
                                     (unsigned long long)dropped, sdk, radius * 2, radius * 2);
                        else
                            swprintf(text, 360, L"%.1f base | %.1f present | %.2f ms age | drop %llu\nDLSS 0x%X | bypass Off",
                                     base / 1000.0, presents / 1000.0, age / 1000.0,
                                     (unsigned long long)dropped, sdk);
                        SetWindowTextW(status, text);
                        lastPerfVersion = v2;
                    }
                }
            } else {
                std::lock_guard<std::mutex> l(control->statusMutex);
                SetWindowTextW(status, control->status.c_str());
            }
        }
        return 0;

    case WM_HOTKEY:
        resolutionRestartPending = false;
        if (control) {
            control->stop = true;
            ShowWindow(output, SW_HIDE);
        }
        return 0;

    case WM_APP + 1:
        if (worker.joinable())
            worker.join();
        if (output) {
            DestroyWindow(output);
            output = nullptr;
        }
        if (control) {
            if (control->resolutionChanged.load(std::memory_order_acquire))
                resolutionRestartPending = true;
            std::lock_guard<std::mutex> l(control->statusMutex);
            SetWindowTextW(status, control->status.c_str());
        }
        control.reset();
        if (closing) {
            resolutionRestartPending = false;
            setRunning(false);
            DestroyWindow(w);
        } else if (resolutionRestartPending && IsWindow(game)) {
            // Debounce the mode transition.  The new session's first NvFBC frame
            // supplies the authoritative dimensions, so this restart converges once.
            SetWindowTextW(status, L"Resolution changed. Rebuilding capture + DLSS-G...");
            SetTimer(w, 2, 300, nullptr);
        } else {
            setRunning(false);
        }
        return 0;

    case WM_CLOSE:
        resolutionRestartPending = false;
        if (worker.joinable()) {
            closing = true;
            if (control) {
                control->stop = true;
                control->hasFrame = false;
            }
            if (output)
                ShowWindow(output, SW_HIDE);
            SetWindowTextW(status, L"Stopping...");
        } else {
            DestroyWindow(w);
        }
        return 0;

    case WM_DESTROY:
        if (foregroundHook)
            UnhookWinEvent(foregroundHook);
        if (minimizeHook)
            UnhookWinEvent(minimizeHook);
        UnregisterHotKey(w, 1);
        if (uiFont && uiFont != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(uiFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, a, b);
}
} // namespace

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    diagnostics::start();
    // Save the per-executable model choice before loading Streamline or D3D12.
    // If unsupported, leave the already-working FG pipeline intact.
    presetC = applyPresetCProfile();
    if (presetC.configured)
        diagnostics::write("NVAPI DRS profile: Preset C (value=3) saved and read back successfully; model activation is UNVERIFIED");
    else
        diagnostics::write(std::string("NVAPI DRS profile: Preset C NOT confirmed; stage=") +
                           std::string(presetC.stage, presetC.stage + wcslen(presetC.stage)) +
                           " error=" + std::to_string(presetC.error));
    diagnostics::fileVersion(L"nvngx_dlssg.dll");
    diagnostics::fileVersion(L"sl.dlss_g.dll");
    diagnostics::fileVersion(L"sl.interposer.dll");

    WNDCLASSW out{};
    out.hInstance = h;
    out.lpfnWndProc = outputProc;
    out.lpszClassName = L"FrameGeneration.Output";
    RegisterClassW(&out);

    WNDCLASSW wc{};
    wc.hInstance = h;
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"FrameGeneration.Control";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&wc);

    HWND w = CreateWindowExW(0, wc.lpszClassName, L"FrameGeneration - Preset C / NVOFA FAST",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 455, 324,
                             nullptr, nullptr, h, nullptr);
    if (!w)
        return 1;
    ShowWindow(w, show);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
