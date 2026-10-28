#if !defined(_WIN32)
#error This controller targets Win32.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

#include "bridge_shared.hpp"

namespace {
constexpr int kFpsEdit = 101;
constexpr int kFgCombo = 102;
constexpr int kApply = 103;
constexpr int kStatus = 104;

HWND g_window = nullptr;
HWND g_fps = nullptr;
HWND g_fg = nullptr;
HWND g_status = nullptr;
HANDLE g_mapping = nullptr;
gpu_frame_bridge::SharedSettings* g_settings = nullptr;
HFONT g_font = nullptr;

void logLine(const std::string& message) {
    wchar_t localAppData[MAX_PATH]{};
    const auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring path = n && n < MAX_PATH
        ? std::wstring(localAppData) + L"\\VkFrameBridgeController.log"
        : L"VkFrameBridgeController.log";
    std::ofstream file(std::filesystem::path(path), std::ios::app);
    if (file) file << message << '\n';
}

HWND addControl(const wchar_t* type, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id) {
    auto control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, g_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (control && g_font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

bool applySettings() {
    wchar_t input[96]{};
    GetWindowTextW(g_fps, input, static_cast<int>(sizeof(input) / sizeof(input[0])));
    errno = 0;
    wchar_t* end = nullptr;
    const double fps = std::wcstod(input, &end);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (input == end || (end && *end != L'\0') ||
        errno == ERANGE || !std::isfinite(fps) || fps < 0.0 || fps > 1000.0) {
        SetWindowTextW(g_status, L"Enter an FPS between 0 and 1000. Decimal values are accepted.");
        logLine("[WARN] Capture FPS rejected: invalid numeric input");
        return false;
    }

    const int selection = static_cast<int>(SendMessageW(g_fg, CB_GETCURSEL, 0, 0));
    if (selection < 0 || selection > 5) {
        SetWindowTextW(g_status, L"Select FG Off or 2x through 6x.");
        return false;
    }
    const LONG fpsMilli = static_cast<LONG>(std::llround(fps * 1000.0));
    const LONG multiplier = selection == 0 ? 0 : selection + 1;

    if (!g_settings) {
        SetWindowTextW(g_status, L"Shared settings unavailable. See controller log.");
        logLine("[ERROR] Shared memory was not initialized");
        return false;
    }
    InterlockedIncrement(&g_settings->revision);
    InterlockedExchange(&g_settings->capture_fps_milli, fpsMilli);
    InterlockedExchange(&g_settings->fg_multiplier, multiplier);
    InterlockedIncrement(&g_settings->revision);
    wchar_t status[220]{};
    swprintf_s(status, L"Capture target %.3f FPS applied; FG %s requested.\nFrame export / FG not implemented in layer yet.",
               fps, multiplier == 0 ? L"Off" :
               multiplier == 2 ? L"2x" :
               multiplier == 3 ? L"3x" :
               multiplier == 4 ? L"4x" :
               multiplier == 5 ? L"5x" : L"6x");
    SetWindowTextW(g_status, status);
    logLine("[CONFIG] capture_milli_fps=" + std::to_string(fpsMilli) +
            " fg_request=" + std::to_string(multiplier) +
            " ofa_grid=4 ofa_perf=FAST ofa_scale_percent=10 (requested, not executed)");
    return true;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_window = hwnd;
        g_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        addControl(L"STATIC", L"Capture FPS", SS_LEFT, 16, 20, 126, 25, 0);
        g_fps = addControl(L"EDIT", L"120", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                           154, 17, 112, 29, kFpsEdit);
        SendMessageW(g_fps, EM_SETLIMITTEXT, 20, 0);
        addControl(L"STATIC", L"0 = uncapped; 60, 80, 112.5, 120, etc.", SS_LEFT,
                   16, 55, 334, 24, 0);
        addControl(L"STATIC", L"FG multiplier", SS_LEFT, 16, 91, 128, 25, 0);
        g_fg = addControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                          154, 87, 112, 175, kFgCombo);
        for (const wchar_t* option : {L"Off", L"2x", L"3x", L"4x", L"5x", L"6x"})
            SendMessageW(g_fg, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
        SendMessageW(g_fg, CB_SETCURSEL, 1, 0);
        addControl(L"STATIC", L"NvOFA requested: FAST / 4x4 / 10% fixed", SS_LEFT,
                   16, 132, 355, 25, 0);
        addControl(L"BUTTON", L"Apply settings", BS_DEFPUSHBUTTON | WS_TABSTOP,
                   16, 169, 340, 38, kApply);
        g_status = addControl(L"STATIC", L"Diagnostic build: real frame selection only.\nNo frame generation or texture copy yet.",
                              SS_LEFT, 16, 225, 345, 60, kStatus);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kApply && HIWORD(wparam) == BN_CLICKED) {
            applySettings();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (g_settings) { UnmapViewOfFile(g_settings); g_settings = nullptr; }
        if (g_mapping) { CloseHandle(g_mapping); g_mapping = nullptr; }
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    logLine("[INFO] Controller starting");
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(gpu_frame_bridge::SharedSettings),
                                   gpu_frame_bridge::kSettingsName);
    if (!g_mapping) {
        logLine("[ERROR] CreateFileMappingW failed: " + std::to_string(GetLastError()));
        MessageBoxW(nullptr, L"Cannot initialize shared settings. See VkFrameBridgeController.log.",
                    L"Frame Bridge", MB_ICONERROR);
        return 1;
    }
    const bool newMapping = GetLastError() != ERROR_ALREADY_EXISTS;
    g_settings = static_cast<gpu_frame_bridge::SharedSettings*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(gpu_frame_bridge::SharedSettings)));
    if (!g_settings) {
        logLine("[ERROR] MapViewOfFile failed: " + std::to_string(GetLastError()));
        CloseHandle(g_mapping);
        return 1;
    }
    if (newMapping || gpu_frame_bridge::atomicRead(&g_settings->magic) !=
                          gpu_frame_bridge::kSettingsMagic) {
        InterlockedExchange(&g_settings->capture_fps_milli, 120000);
        InterlockedExchange(&g_settings->fg_multiplier, 2);
        InterlockedExchange(&g_settings->revision, 2);
        InterlockedExchange(&g_settings->magic, gpu_frame_bridge::kSettingsMagic);
    }

    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = windowProc;
    cls.lpszClassName = L"VkFrameBridge.SettingsController";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassW(&cls)) return 1;
    const auto hwnd = CreateWindowExW(0, cls.lpszClassName, L"Vulkan Frame Bridge Settings",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 395, 333,
                                     nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
