#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "dlss_bridge.h"
#include "focus_compat.h"
#include "wgc_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kAppName[] = L"DX12 Presenter V13 - DXGI / WGC - DLSS FG";
constexpr wchar_t kControlClass[] = L"LoLDX12Presenter.Control";
constexpr wchar_t kPresenterClass[] = L"LoLDX12Presenter.Output";
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_STOPPED = WM_APP + 2;
constexpr UINT WM_APP_OUTPUT_VISIBILITY = WM_APP + 3;
constexpr DWORD WDA_EXCLUDEFROMCAPTURE_VALUE = 0x00000011;
constexpr UINT kFrameCount = 2;
constexpr UINT kCaptureSlots = 2;
constexpr double kMailboxStaleFloorMs = 3.0;
constexpr double kMailboxStaleCeilMs = 12.0;

// V9 scheduling helpers.  Discover actual physical cores and give capture and
// render different *ideal* processors.  This avoids assuming SMT numbering while
// still letting Windows rebalance the threads if the game needs those CPUs.
PROCESSOR_NUMBER ChoosePhysicalCoreHint(bool captureThread) {
    PROCESSOR_NUMBER result{};
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (!bytes) return result;
    std::vector<BYTE> storage(bytes);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data()), &bytes))
        return result;

    std::vector<PROCESSOR_NUMBER> cores;
    BYTE* cursor = storage.data();
    BYTE* end = storage.data() + bytes;
    while (cursor < end) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cursor);
        if (info->Relationship == RelationProcessorCore) {
            for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                KAFFINITY mask = info->Processor.GroupMask[g].Mask;
                for (BYTE bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                    if (mask & (KAFFINITY(1) << bit)) {
                        PROCESSOR_NUMBER pn{};
                        pn.Group = info->Processor.GroupMask[g].Group;
                        pn.Number = bit;
                        cores.push_back(pn);
                        break;
                    }
                }
            }
        }
        if (!info->Size) break;
        cursor += info->Size;
    }
    if (cores.empty()) return result;
    if (cores.size() == 1) return cores[0];
    // Prefer the last two physical cores so the two helper threads do not share
    // a core.  These are hints, not affinity masks.
    return captureThread ? cores.back() : cores[cores.size() - 2];
}

void ConfigureLowLatencyThread(bool captureThread) {
    PROCESSOR_NUMBER preferred = ChoosePhysicalCoreHint(captureThread);
    SetThreadIdealProcessorEx(GetCurrentThread(), &preferred, nullptr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

// V9 adaptive polling: estimate the source frame interval and spend CPU only
// when the next frame is close.  Far from the predicted arrival we yield; inside
// ~0.35 ms we briefly PAUSE-spin to catch the freshest frame with less jitter.
void AdaptiveCaptureWait(unsigned& misses, LARGE_INTEGER nowQpc, LARGE_INTEGER lastFrameQpc,
                         double expectedIntervalMs, LARGE_INTEGER qpcFrequency) {
    ++misses;
    if (qpcFrequency.QuadPart <= 0 || lastFrameQpc.QuadPart <= 0) {
        if (misses < 24) { for (unsigned i=0;i<48;++i) YieldProcessor(); }
        else SwitchToThread();
        return;
    }
    const double elapsedMs = 1000.0 * double(nowQpc.QuadPart - lastFrameQpc.QuadPart) /
                             double(qpcFrequency.QuadPart);
    const double remainingMs = expectedIntervalMs - elapsedMs;
    if (remainingMs <= 0.35) {
        const unsigned loops = remainingMs <= 0.12 ? 192u : 96u;
        for (unsigned i=0;i<loops;++i) YieldProcessor();
    } else if (remainingMs <= 1.25) {
        SwitchToThread();
    } else {
        if ((misses & 7u) == 0) Sleep(0);
        else SwitchToThread();
    }
}

enum ControlId : int {
    IDC_TARGET = 100, IDC_REFRESH, IDC_START, IDC_STOP,
    IDC_FULLSCREEN, IDC_LOW_LATENCY, IDC_STATUS, IDC_MULTIPLIER, IDC_CAPTURE_BACKEND
};

std::wstring WinError(HRESULT hr) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Unknown error";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    wchar_t code[24]{};
    swprintf_s(code, L" (0x%08X)", static_cast<unsigned>(hr));
    return result + code;
}

class Logger {
public:
    Logger() {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::filesystem::path p(path);
        p.replace_filename(L"DLSS-Presenter.log");
        file_.open(p, std::ios::app);
    }
    void Write(const std::wstring& text) {
        std::lock_guard lock(mutex_);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        if (file_) {
            file_ << L'[' << t.wHour << L':' << t.wMinute << L':' << t.wSecond << L"] "
                  << text << L'\n';
            file_.flush();
        }
    }
private:
    std::wofstream file_;
    std::mutex mutex_;
};

Logger gLog;
DlssExperiment gDLSS;

struct CaptureSlot {
    ComPtr<ID3D11Texture2D> texture;
    HANDLE textureHandle{};
    std::atomic<int> state{0}; // 0=FREE, 1=READY, 2=IN_USE, 3=CAPTURE_WRITING
    UINT64 readyValue{};
    UINT64 releaseValue{};
    std::atomic<UINT64> sequence{0};
    LARGE_INTEGER presentTimestamp{};
    LARGE_INTEGER signalQpc{};
};

struct CaptureState {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    bool duplicateOutput1{false};
    std::mutex producerStatsMutex;
    double producerSubmitSumMs{}, producerSubmitMaxMs{};
    unsigned producerSubmitSamples{};
    std::array<CaptureSlot,kCaptureSlots> slots;
    ComPtr<ID3D11Fence> sharedFence; // capture-ready fence, signaled only by D3D11
    ComPtr<ID3D11Fence> renderFence; // slot-release fence, signaled only by D3D12
    HANDLE fenceHandle{};
    HANDLE renderFenceHandle{};
    RECT sourceRect{};
    DXGI_OUTPUT_DESC outputDesc{};
    UINT64 nextFenceValue{1};
};

struct Dx12State {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Resource> backBuffers[kFrameCount];
    ComPtr<ID3D12CommandAllocator> allocators[kFrameCount];
    ComPtr<ID3D12CommandAllocator> preAllocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12GraphicsCommandList> preList;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<ComPtr<ID3D12Resource>,kCaptureSlots> sharedTextures;
    ComPtr<ID3D12Fence> sharedFence;
    ComPtr<ID3D12Fence> renderFence;
    UINT64 nextRenderFenceValue{1};
    ComPtr<ID3D12Fence> frameFence;
    ComPtr<ID3D12QueryHeap> timestampHeap;
    ComPtr<ID3D12Resource> timestampReadback;
    UINT64 gpuTimestampFrequency{};
    bool gpuTimestampPending[kFrameCount]{};
    double gpuPreSumMs{}, gpuWaitGapSumMs{}, gpuPostSumMs{};
    double gpuPreMaxMs{}, gpuWaitGapMaxMs{}, gpuPostMaxMs{};
    unsigned gpuSamples{};
    HANDLE frameEvent{};
    HANDLE latencyWaitableObject{};
    UINT64 frameFenceValues[kFrameCount]{};
    UINT64 nextFrameFenceValue{1};
    UINT rtvStride{};
    UINT width{};
    UINT height{};
    UINT sourceWidth{};
    UINT sourceHeight{};
    bool allowTearing{};
    LARGE_INTEGER captureTimestamp{}, qpcFrequency{};
    double ageSumMs{}, ageMaxMs{};
    double captureToSubmitSumMs{}, captureToSubmitMaxMs{};
    double preToPostSubmitSumMs{}, preToPostSubmitMaxMs{};
    double mailboxToClaimSumMs{}, mailboxToClaimMaxMs{};
    double claimToPresentSumMs{}, claimToPresentMaxMs{};
    unsigned ageSamples{}, stageSamples{}, mailboxSamples{};
    LARGE_INTEGER captureSignalQpc{}, claimQpc{};
};

class App {
public:
    int Run(HINSTANCE instance);
    LRESULT OnControlMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnPresenterMessage(HWND, UINT, WPARAM, LPARAM);

private:
    bool RegisterClasses();
    void CreateControls();
    void RefreshTargets();
    void Start();
    void Stop();
    void ToggleFullscreen();
    void Worker(HWND target, HWND outputWindow);
    void PostStatus(const std::wstring& text);
    bool CreatePresenter(HWND target, bool fullscreen);
    bool InitCapture(HWND target, CaptureState& c, uint32_t& width, uint32_t& height, bool useWgc);
    bool InitDx12(HWND presenter, CaptureState& c, uint32_t sourceWidth,
                  uint32_t sourceHeight, bool lowLatency, Dx12State& d);
    bool CreateSwapchain(HWND presenter, bool lowLatency, Dx12State& d, bool resize);
    bool CreatePipeline(Dx12State& d);
    bool RenderFrame(CaptureState& c, Dx12State& d, UINT captureSlot, UINT64 copyReady,
                     UINT64& renderDone, float syntheticOffsetPixels = 0.0f);
    void DestroyDx12(Dx12State& d);
    static BOOL CALLBACK EnumWindowsProc(HWND, LPARAM);
    static LRESULT CALLBACK ControlProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PresenterProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE instance_{};
    HWND control_{};
    HWND presenter_{};
    HWND targetCombo_{};
    HWND status_{};
    HWND target_{};
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> recreateSwapchain_{false};
    bool fullscreen_{true};
    UINT multiplier_{2};
    bool lowLatency_{true};
    bool useWgc_{false};
    RECT windowedRect_{100, 100, 1380, 820};
};

App* gApp = nullptr;

bool GetClientScreenRect(HWND hwnd, RECT& rect) {
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    POINT tl{client.left, client.top};
    POINT br{client.right, client.bottom};
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return false;
    rect = {tl.x, tl.y, br.x, br.y};
    return rect.right > rect.left && rect.bottom > rect.top;
}

int App::Run(HINSTANCE instance) {
    instance_ = instance;
    gApp = this;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    gDLSS.log=[](const std::wstring& text){gLog.Write(text);};
    try { gDLSS.Load(); } catch(const std::exception& e) {
        std::string msg=e.what();std::wstring wide(msg.begin(),msg.end());gLog.Write(wide);
        MessageBoxW(nullptr,wide.c_str(),L"DLSS initialization failed",MB_ICONERROR);gDLSS.Shutdown();return 1;
    }
    if (!RegisterClasses()) {gDLSS.Shutdown();return 1;}
    control_ = CreateWindowExW(WS_EX_APPWINDOW, kControlClass, kAppName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 590, 335, nullptr, nullptr, instance_, nullptr);
    if (!control_) {gDLSS.Shutdown();return 1;}
    CreateControls();
    RefreshTargets();
    ShowWindow(control_, SW_SHOW);
    UpdateWindow(control_);
    RegisterHotKey(control_, 1, MOD_NOREPEAT, VK_F11);
    RegisterHotKey(control_, 2, MOD_NOREPEAT, VK_F8);
    RegisterHotKey(control_, 3, MOD_NOREPEAT, VK_F10);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Stop();
    gDLSS.Shutdown();
    return static_cast<int>(msg.wParam);
}

bool App::RegisterClasses() {
    WNDCLASSEXW control{sizeof(control)};
    control.style = CS_HREDRAW | CS_VREDRAW;
    control.lpfnWndProc = ControlProc;
    control.hInstance = instance_;
    control.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    control.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    control.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    control.lpszClassName = kControlClass;
    if (!RegisterClassExW(&control)) return false;
    WNDCLASSEXW presenter{sizeof(presenter)};
    presenter.style = CS_OWNDC;
    presenter.lpfnWndProc = PresenterProc;
    presenter.hInstance = instance_;
    presenter.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    presenter.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    presenter.lpszClassName = kPresenterClass;
    return RegisterClassExW(&presenter) != 0;
}

void App::CreateControls() {
    auto label = [&](const wchar_t* text, int x, int y, int w, int h) {
        return CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE,x,y,w,h,control_,nullptr,instance_,nullptr);
    };
    label(L"Source window",18,20,120,22);
    targetCombo_=CreateWindowW(WC_COMBOBOXW,nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        18,45,430,240,control_,reinterpret_cast<HMENU>(IDC_TARGET),instance_,nullptr);
    CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE,460,44,100,27,control_,reinterpret_cast<HMENU>(IDC_REFRESH),instance_,nullptr);
    CreateWindowW(L"BUTTON",L"Fullscreen output",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,18,91,170,24,control_,reinterpret_cast<HMENU>(IDC_FULLSCREEN),instance_,nullptr);
    SendDlgItemMessageW(control_,IDC_FULLSCREEN,BM_SETCHECK,BST_CHECKED,0);
    CreateWindowW(L"BUTTON",L"Low latency / tearing",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,205,91,190,24,control_,reinterpret_cast<HMENU>(IDC_LOW_LATENCY),instance_,nullptr);
    SendDlgItemMessageW(control_,IDC_LOW_LATENCY,BM_SETCHECK,BST_CHECKED,0);
    label(L"FG",410,94,30,22);
    HWND multiplier=CreateWindowW(WC_COMBOBOXW,nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        447,89,110,160,control_,reinterpret_cast<HMENU>(IDC_MULTIPLIER),instance_,nullptr);
    for(const wchar_t* option:{L"2x",L"3x",L"4x"}) SendMessageW(multiplier,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));
    SendMessageW(multiplier,CB_SETCURSEL,0,0);
    label(L"Capture",18,132,58,22);
    HWND backend=CreateWindowW(WC_COMBOBOXW,nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        78,127,210,120,control_,reinterpret_cast<HMENU>(IDC_CAPTURE_BACKEND),instance_,nullptr);
    SendMessageW(backend,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"DXGI DuplicateOutput1"));
    SendMessageW(backend,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Windows Graphics Capture"));
    SendMessageW(backend,CB_SETCURSEL,0,0);
    CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,310,126,120,36,control_,reinterpret_cast<HMENU>(IDC_START),instance_,nullptr);
    CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|WS_DISABLED,442,126,110,36,control_,reinterpret_cast<HMENU>(IDC_STOP),instance_,nullptr);
    status_=label(L"Choose capture backend and FG multiplier, then Start.",18,181,540,68);
    label(L"F11 fullscreen | F8 pause | F10 stop",18,263,540,22);
}

BOOL CALLBACK App::EnumWindowsProc(HWND hwnd, LPARAM param) {
    auto* self = reinterpret_cast<App*>(param);
    if (!IsWindowVisible(hwnd) || hwnd == self->control_ || hwnd == self->presenter_) return TRUE;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!title[0]) return TRUE;
    std::wstring text(title);
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t entry[600]{};
    swprintf_s(entry, L"%s  [PID %lu]", text.c_str(), pid);
    LRESULT index = SendMessageW(self->targetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
    SendMessageW(self->targetCombo_, CB_SETITEMDATA, index, reinterpret_cast<LPARAM>(hwnd));
    if (text.find(L"League of Legends") != std::wstring::npos ||
        text.find(L"League") != std::wstring::npos) {
        SendMessageW(self->targetCombo_, CB_SETCURSEL, index, 0);
    }
    return TRUE;
}

void App::RefreshTargets() {
    SendMessageW(targetCombo_, CB_RESETCONTENT, 0, 0);
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(this));
    if (SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0) == CB_ERR &&
        SendMessageW(targetCombo_, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(targetCombo_, CB_SETCURSEL, 0, 0);
}

void App::PostStatus(const std::wstring& text) {
    auto* message=new std::wstring(text);
    if(!PostMessageW(control_,WM_APP_STATUS,0,reinterpret_cast<LPARAM>(message))) delete message;
    gLog.Write(text);
}

void App::Start() {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    if (!gDLSS.initialized) {
        try {gDLSS.Load();} catch(const std::exception& e) {
            std::string m=e.what();PostStatus(std::wstring(m.begin(),m.end()));return;
        }
    }
    LRESULT selected = SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) { SetWindowTextW(status_, L"Select the game window first."); return; }
    HWND target = reinterpret_cast<HWND>(SendMessageW(targetCombo_, CB_GETITEMDATA, selected, 0));
    if (!IsWindow(target)) { RefreshTargets(); SetWindowTextW(status_, L"That window closed. Select it again."); return; }
    fullscreen_ = SendDlgItemMessageW(control_, IDC_FULLSCREEN, BM_GETCHECK, 0, 0) == BST_CHECKED;
    LRESULT selectedMultiplier = SendDlgItemMessageW(control_, IDC_MULTIPLIER, CB_GETCURSEL, 0, 0);
    if (selectedMultiplier < 0 || selectedMultiplier > 2) {
        SetWindowTextW(status_, L"Choose 2x, 3x or 4x before starting."); return;
    }
    multiplier_ = static_cast<UINT>(selectedMultiplier) + 2;
    lowLatency_ = SendDlgItemMessageW(control_,IDC_LOW_LATENCY,BM_GETCHECK,0,0)==BST_CHECKED;
    LRESULT selectedBackend = SendDlgItemMessageW(control_, IDC_CAPTURE_BACKEND, CB_GETCURSEL, 0, 0);
    useWgc_ = selectedBackend == 1;

    stop_ = false;
    paused_ = false;
    recreateSwapchain_ = false;
    if (!CreatePresenter(target, fullscreen_)) {
        SetWindowTextW(status_, L"Could not create the DX12 output window. Restore the game, press Refresh, and select it again.");
        return;
    }
    running_ = true;
    EnableWindow(GetDlgItem(control_, IDC_START), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_MULTIPLIER), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_CAPTURE_BACKEND), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_STOP), TRUE);
    worker_ = std::thread(&App::Worker, this, target, presenter_);
}

void App::Stop() {
    focus_compat::Enable(false);
    stop_ = true;
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    MSG pending{};
    while(PeekMessageW(&pending,control_,WM_APP_STOPPED,WM_APP_STOPPED,PM_REMOVE)) {}
    if (presenter_ && IsWindow(presenter_)) { DestroyWindow(presenter_); presenter_ = nullptr; }
    target_ = nullptr;
    running_ = false;
}

bool App::CreatePresenter(HWND target, bool fullscreen) {
    target_ = target;
    RECT source{};
    if (!GetClientScreenRect(target, source)) return false;
    HMONITOR monitor = MonitorFromRect(&source, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    RECT r = fullscreen ? mi.rcMonitor : windowedRect_;
    DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    presenter_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                      WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kPresenterClass, L"LoL DX12 Output", style,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        target, nullptr, instance_, nullptr);
    if (!presenter_) return false;
    if (!SetLayeredWindowAttributes(presenter_, 0, 255, LWA_ALPHA)) {
        DestroyWindow(presenter_); presenter_ = nullptr; return false;
    }
    // Keep the output owned by the game for window ordering. Ownership alone
    // does not satisfy Streamline's process-based foreground check.
    SetWindowLongPtrW(presenter_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(target));
    LONG_PTR exStyle = GetWindowLongPtrW(presenter_, GWL_EXSTYLE);
    SetWindowLongPtrW(presenter_, GWL_EXSTYLE, exStyle & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW));
    SetWindowPos(presenter_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowDisplayAffinity(presenter_, WDA_EXCLUDEFROMCAPTURE_VALUE);
    ShowWindow(presenter_, SW_SHOWNOACTIVATE);
    UpdateWindow(presenter_);
    ShowWindow(target, SW_SHOW);
    // Preserve game keyboard/raw-input focus; do not attach input queues.
    SetForegroundWindow(target);
    gLog.Write(L"Game retains focus; presenter is non-activating and click-through.");
    return true;
}

void App::ToggleFullscreen() {
    if (!presenter_) return;
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        GetWindowRect(presenter_, &windowedRect_);
        SetWindowLongPtrW(presenter_, GWL_STYLE, WS_POPUP);
        HMONITOR m = MonitorFromWindow(presenter_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(m, &mi);
        SetWindowPos(presenter_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(presenter_, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        SetWindowPos(presenter_, HWND_TOP, windowedRect_.left, windowedRect_.top,
            windowedRect_.right - windowedRect_.left, windowedRect_.bottom - windowedRect_.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    recreateSwapchain_ = true;
}

bool App::InitCapture(HWND target, CaptureState& c, uint32_t& width, uint32_t& height, bool useWgc) {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&c.factory));
    if (FAILED(hr)) { PostStatus(L"CreateDXGIFactory2 failed: " + WinError(hr)); return false; }
    if (!GetClientScreenRect(target, c.sourceRect)) { PostStatus(L"Could not read the source window area."); return false; }
    HMONITOR sourceMonitor = MonitorFromRect(&c.sourceRect, MONITOR_DEFAULTTONEAREST);
    for (UINT ai = 0; ; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (c.factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{}; output->GetDesc(&desc);
            if (desc.Monitor == sourceMonitor) {
                c.adapter = adapter; output.As(&c.output); c.outputDesc = desc; break;
            }
        }
        if (c.output) break;
    }
    if (!c.output) { PostStatus(L"Could not find the monitor containing League."); return false; }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};
    // DXGI has one capture owner; WGC also uses internal worker threads and
    // requires a thread-safe device and protected immediate context.
    const UINT captureDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        (useWgc ? 0u : D3D11_CREATE_DEVICE_SINGLETHREADED);
    hr = D3D11CreateDevice(c.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        captureDeviceFlags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &c.device, &obtained, &c.context);
    if (FAILED(hr)) { PostStatus(L"D3D11 capture device failed: " + WinError(hr)); return false; }
    if (useWgc) {
        ComPtr<ID3D11Multithread> multithread;
        hr = c.context.As(&multithread);
        if (FAILED(hr)) { PostStatus(L"WGC D3D11 multithread interface failed: " + WinError(hr)); return false; }
        multithread->SetMultithreadProtected(TRUE);
    }
    hr = c.device.As(&c.device5);
    if (FAILED(hr)) { PostStatus(L"Windows/D3D11 shared-fence support is unavailable: " + WinError(hr)); return false; }
    hr = c.context.As(&c.context4);
    if (FAILED(hr)) { PostStatus(L"D3D11 context shared-fence support is unavailable: " + WinError(hr)); return false; }

    if (!useWgc) {
        // Prefer DuplicateOutput1 on Windows 10+; it lets DXGI avoid an unnecessary
        // fullscreen format conversion when the returned surface can stay BGRA8.
        ComPtr<IDXGIOutput5> output5;
        if (SUCCEEDED(c.output.As(&output5)) && output5) {
            const DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = output5->DuplicateOutput1(c.device.Get(), 0, ARRAYSIZE(formats), formats, &c.duplication);
            if (SUCCEEDED(hr)) c.duplicateOutput1 = true;
        }
        if (!c.duplication)
            hr = c.output->DuplicateOutput(c.device.Get(), &c.duplication);
        if (FAILED(hr) || !c.duplication) { PostStatus(L"Desktop capture failed. Close other capture apps and retry: " + WinError(hr)); return false; }
        gLog.Write(c.duplicateOutput1 ? L"V12 DXGI backend: DuplicateOutput1."
                                     : L"V12 DXGI backend: DuplicateOutput fallback.");

        RECT& desktop = c.outputDesc.DesktopCoordinates;
        RECT clipped{std::max(c.sourceRect.left, desktop.left), std::max(c.sourceRect.top, desktop.top),
                     std::min(c.sourceRect.right, desktop.right), std::min(c.sourceRect.bottom, desktop.bottom)};
        if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
            PostStatus(L"The source window is outside the selected monitor."); return false;
        }
        c.sourceRect = clipped;
        width = static_cast<uint32_t>(clipped.right - clipped.left);
        height = static_cast<uint32_t>(clipped.bottom - clipped.top);
    } else {
        width = static_cast<uint32_t>(c.sourceRect.right - c.sourceRect.left);
        height = static_cast<uint32_t>(c.sourceRect.bottom - c.sourceRect.top);
        gLog.Write(L"V12 WGC backend selected. WGC frame delivery feeds the same two-slot D3D11/D3D12 mailbox.");
    }

    // D3D11 must create the NT-handle resource. DX12 can import this resource directly;
    // OpenSharedResource1 in the opposite direction is invalid for a D3D12-created texture.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // SHARED_NTHANDLE chooses the NT handle type; SHARED marks the allocation
    // itself as shareable. Keep KEYEDMUTEX off because D3D12 synchronizes this
    // resource with the shared D3D11/D3D12 fence below.
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (UINT i=0;i<kCaptureSlots;++i) {
        hr = c.device->CreateTexture2D(&td, nullptr, &c.slots[i].texture);
        if (FAILED(hr)) { PostStatus(L"D3D11 mailbox texture creation failed: " + WinError(hr)); return false; }
        ComPtr<IDXGIResource1> dxgiResource;
        hr = c.slots[i].texture.As(&dxgiResource);
        if (FAILED(hr)) { PostStatus(L"Mailbox texture interface failed: " + WinError(hr)); return false; }
        hr = dxgiResource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &c.slots[i].textureHandle);
        if (FAILED(hr)) { PostStatus(L"Mailbox texture handle failed: " + WinError(hr)); return false; }
    }
    hr = c.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.sharedFence));
    if (FAILED(hr)) { PostStatus(L"D3D11 capture-ready fence creation failed: " + WinError(hr)); return false; }
    hr = c.sharedFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &c.fenceHandle);
    if (FAILED(hr)) { PostStatus(L"Capture-ready fence handle failed: " + WinError(hr)); return false; }
    hr = c.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.renderFence));
    if (FAILED(hr)) { PostStatus(L"D3D11 render-release fence creation failed: " + WinError(hr)); return false; }
    hr = c.renderFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &c.renderFenceHandle);
    if (FAILED(hr)) { PostStatus(L"Render-release fence handle failed: " + WinError(hr)); return false; }
    return true;
}

bool App::CreatePipeline(Dx12State& d) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = ARRAYSIZE(params); rs.pParameters = params;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &sampler;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(hr)) { PostStatus(L"DX12 root signature serialization failed: " + WinError(hr)); return false; }
    hr = d.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&d.rootSignature));
    if (FAILED(hr)) { PostStatus(L"DX12 root signature creation failed: " + WinError(hr)); return false; }

    static const char shader[] =
        "Texture2D src : register(t0); SamplerState samp : register(s0);"
        "cbuffer FrameAdjust : register(b0) { float jitterX; };"
        "struct V { float4 p:SV_Position; float2 uv:TEXCOORD0; };"
        "V VS(uint id:SV_VertexID) { V o;"
        "float2 p=float2((id==2)?3.0:-1.0,(id==1)?3.0:-1.0);"
        "o.p=float4(p,0,1); o.uv=float2((p.x+1)*0.5,1-(p.y+1)*0.5); return o; }"
        "float4 PS(V i):SV_Target { return src.Sample(samp,i.uv+float2(jitterX,0)); }";
    ComPtr<ID3DBlob> vs, ps;
    hr = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors);
    if (FAILED(hr)) { PostStatus(L"DX12 vertex shader compile failed: " + WinError(hr)); return false; }
    errors.Reset();
    hr = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors);
    if (FAILED(hr)) { PostStatus(L"DX12 pixel shader compile failed: " + WinError(hr)); return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = d.rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (auto& target : pso.BlendState.RenderTarget) target = rtBlend;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso.SampleDesc.Count = 1;
    hr = d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.pipeline));
    if (FAILED(hr)) { PostStatus(L"DX12 pipeline creation failed: " + WinError(hr)); return false; }
    return true;
}

bool App::CreateSwapchain(HWND presenter, bool lowLatency, Dx12State& d, bool resize) {
    RECT r{}; GetClientRect(presenter, &r);
    UINT width = std::max<LONG>(1, r.right - r.left);
    UINT height = std::max<LONG>(1, r.bottom - r.top);
    if (resize) {
        for (auto& b : d.backBuffers) b.Reset();
        UINT resizeFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (d.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        HRESULT hr = d.swapchain->ResizeBuffers(kFrameCount, width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM, resizeFlags);
        if (FAILED(hr)) { PostStatus(L"DX12 swapchain resize failed: " + WinError(hr)); return false; }
    } else {
        BOOL tearing = FALSE;
        if (lowLatency) d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        d.allowTearing = tearing == TRUE;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width; desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kFrameCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // The waitable-object flag lets the CPU pace against DXGI immediately before
        // capture instead of discovering swapchain pressure after a fresh image is grabbed.
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (d.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        ComPtr<IDXGISwapChain1> swap;
        HRESULT hr = d.factory->CreateSwapChainForHwnd(d.queue.Get(), presenter, &desc,
            nullptr, nullptr, &swap);
        if (FAILED(hr)) { PostStatus(L"DX12 swapchain creation failed: " + WinError(hr)); return false; }
        d.factory->MakeWindowAssociation(presenter, DXGI_MWA_NO_ALT_ENTER);
        hr = swap.As(&d.swapchain);
        if (FAILED(hr)) return false;
    }
    {
        ComPtr<IDXGISwapChain2> latencySwap;
        HRESULT latencyHr = d.swapchain.As(&latencySwap);
        if (SUCCEEDED(latencyHr)) {
            latencyHr = latencySwap->SetMaximumFrameLatency(1);
            if (FAILED(latencyHr)) { PostStatus(L"SetMaximumFrameLatency(1) failed: " + WinError(latencyHr)); return false; }
            if (!d.latencyWaitableObject) {
                d.latencyWaitableObject = latencySwap->GetFrameLatencyWaitableObject();
                if (!d.latencyWaitableObject) {
                    PostStatus(L"DXGI frame-latency waitable object is unavailable.");
                    return false;
                }
            }
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        HRESULT hr = d.swapchain->GetBuffer(i, IID_PPV_ARGS(&d.backBuffers[i]));
        if (FAILED(hr)) { PostStatus(L"DX12 backbuffer failed: " + WinError(hr)); return false; }
        d.device->CreateRenderTargetView(d.backBuffers[i].Get(), nullptr, handle);
        handle.ptr += d.rtvStride;
    }
    d.width = width; d.height = height;
    return true;
}

bool App::InitDx12(HWND, CaptureState& c, uint32_t sourceWidth,
                   uint32_t sourceHeight, bool, Dx12State& d) {
    d.sourceWidth = sourceWidth; d.sourceHeight = sourceHeight;
    HRESULT hr = gDLSS.factory(IID_PPV_ARGS(&d.factory));
    if (FAILED(hr)) return false;
    hr = gDLSS.createDevice(c.adapter.Get(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&d.device));
    if (FAILED(hr)) { PostStatus(L"DX12 device creation failed: " + WinError(hr)); return false; }
    gDLSS.BindDevice(d.device.Get());
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&d.queue));
    if (FAILED(hr)) { PostStatus(L"DX12 command queue failed: " + WinError(hr)); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = kFrameCount;
    hr = d.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&d.rtvHeap));
    if (FAILED(hr)) return false;
    d.rtvStride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; srv.NumDescriptors = 1;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = d.device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&d.srvHeap));
    if (FAILED(hr)) return false;
    for (UINT i = 0; i < kFrameCount; ++i) {
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.allocators[i]));
        if (FAILED(hr)) return false;
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.preAllocators[i]));
        if (FAILED(hr)) return false;
    }
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.allocators[0].Get(), nullptr, IID_PPV_ARGS(&d.list));
    if (FAILED(hr)) return false;
    d.list->Close();
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.preAllocators[0].Get(), nullptr, IID_PPV_ARGS(&d.preList));
    if (FAILED(hr)) return false;
    d.preList->Close();
    for (UINT i=0;i<kCaptureSlots;++i) {
        hr = d.device->OpenSharedHandle(c.slots[i].textureHandle, IID_PPV_ARGS(&d.sharedTextures[i]));
        if (FAILED(hr)) {
            PostStatus(L"DX12 could not open a D3D11 mailbox texture: " + WinError(hr));
            return false;
        }
    }
    hr = d.device->OpenSharedHandle(c.fenceHandle, IID_PPV_ARGS(&d.sharedFence));
    if (FAILED(hr)) { PostStatus(L"DX12 could not open the capture-ready fence: " + WinError(hr)); return false; }
    hr = d.device->OpenSharedHandle(c.renderFenceHandle, IID_PPV_ARGS(&d.renderFence));
    if (FAILED(hr)) { PostStatus(L"DX12 could not open the render-release fence: " + WinError(hr)); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    // Slot 0 is only a placeholder for the presenter SRV heap; the actual DLSS
    // compute path has one immutable descriptor heap per capture slot.
    d.device->CreateShaderResourceView(d.sharedTextures[0].Get(), &view,
        d.srvHeap->GetCPUDescriptorHandleForHeapStart());
    hr = d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.frameFence));
    if (FAILED(hr)) return false;
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kFrameCount * 4;
    hr = d.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&d.timestampHeap));
    if (FAILED(hr)) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb{}; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = UINT64(kFrameCount) * 4u * sizeof(UINT64); rb.Height = 1; rb.DepthOrArraySize = 1;
    rb.MipLevels = 1; rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = d.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.timestampReadback));
    if (FAILED(hr)) return false;
    d.queue->GetTimestampFrequency(&d.gpuTimestampFrequency);
    d.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.frameEvent) return false;
    if (!CreatePipeline(d)) return false;
    return true;
}

bool App::RenderFrame(CaptureState& c, Dx12State& d, UINT captureSlot, UINT64 copyReady,
                      UINT64& renderDone, float syntheticOffsetPixels) {
    // V4: launch NVOFA, then immediately submit a source/resample command list.
    // The graphics queue and optical-flow engine can work in parallel.  We insert
    // the NVOFA fence wait only between the pre-motion and post-motion lists.
    if (captureSlot >= kCaptureSlots) return false;
    ID3D12Resource* source = d.sharedTextures[captureSlot].Get();
    // Protect the shared optical-flow output and previous-frame texture from reuse
    // until the previous graphics submission has finished consuming/updating them.
    gDLSS.PrepareMotion(d.sharedFence.Get(),copyReady,captureSlot,d.renderFence.Get(),d.nextRenderFenceValue-1);
    UINT frame = d.swapchain->GetCurrentBackBufferIndex();

    // The worker has already waited for this backbuffer's frame fence, so a
    // timestamp resolve associated with its previous use is safe to read now.
    if (d.gpuTimestampPending[frame] && d.timestampReadback && d.gpuTimestampFrequency) {
        const UINT64 offset = UINT64(frame) * 4u * sizeof(UINT64);
        D3D12_RANGE readRange{SIZE_T(offset), SIZE_T(offset + 4u*sizeof(UINT64))};
        void* mapped = nullptr;
        if (SUCCEEDED(d.timestampReadback->Map(0, &readRange, &mapped)) && mapped) {
            const UINT64* q = reinterpret_cast<const UINT64*>(
                static_cast<const BYTE*>(mapped) + offset);
            auto ticksToMs = [&](UINT64 a, UINT64 b) -> double {
                return b >= a ? 1000.0 * double(b-a) / double(d.gpuTimestampFrequency) : 0.0;
            };
            const double preMs = ticksToMs(q[0], q[1]);
            const double waitMs = ticksToMs(q[1], q[2]);
            const double postMs = ticksToMs(q[2], q[3]);
            d.gpuPreSumMs += preMs; d.gpuPreMaxMs = std::max(d.gpuPreMaxMs, preMs);
            d.gpuWaitGapSumMs += waitMs; d.gpuWaitGapMaxMs = std::max(d.gpuWaitGapMaxMs, waitMs);
            d.gpuPostSumMs += postMs; d.gpuPostMaxMs = std::max(d.gpuPostMaxMs, postMs);
            ++d.gpuSamples;
            D3D12_RANGE written{0,0}; d.timestampReadback->Unmap(0, &written);
        }
        d.gpuTimestampPending[frame] = false;
    }

    const UINT queryBase = frame * 4u;
    HRESULT hr = d.preAllocators[frame]->Reset();
    if (FAILED(hr)) return false;
    hr = d.preList->Reset(d.preAllocators[frame].Get(), nullptr);
    if (FAILED(hr)) return false;
    if (d.timestampHeap) d.preList->EndQuery(d.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 0);
    gDLSS.RecordPreMotion(d.preList.Get(),source,captureSlot);
    if (d.timestampHeap) d.preList->EndQuery(d.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);
    hr = d.preList->Close();
    if (FAILED(hr)) return false;

    // Both NVOFA and this pre-list wait on the same capture-ready fence.  After it
    // fires they only read the live captured texture, so this overlap is read/read.
    hr = d.queue->Wait(d.sharedFence.Get(), copyReady);
    if (FAILED(hr)) return false;
    ID3D12CommandList* preLists[] = {d.preList.Get()};
    d.queue->ExecuteCommandLists(1, preLists);
    LARGE_INTEGER preSubmitted{}; QueryPerformanceCounter(&preSubmitted);

    // Delay the optical-flow dependency until the first command list that actually
    // consumes flow/cost surfaces.
    if(!gDLSS.WaitMotion(d.queue.Get())) return false;

    hr = d.allocators[frame]->Reset();
    if (FAILED(hr)) return false;
    hr = d.list->Reset(d.allocators[frame].Get(), d.pipeline.Get());
    if (FAILED(hr)) return false;
    if (d.timestampHeap) d.list->EndQuery(d.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 2);
    gDLSS.RecordPostMotion(d.list.Get(),source,captureSlot);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d.backBuffers[frame].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.list->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame) * d.rtvStride;
    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    d.list->SetGraphicsRootSignature(d.rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = {d.srvHeap.Get()};
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetGraphicsRootDescriptorTable(0, d.srvHeap->GetGPUDescriptorHandleForHeapStart());
    float jitterX = syntheticOffsetPixels / static_cast<float>(std::max(1u, d.sourceWidth));
    d.list->SetGraphicsRoot32BitConstants(1, 1, &jitterX, 0);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3, 1, 0, 0);
    gDLSS.TagBackbuffer(d.list.Get(),d.backBuffers[frame].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d.list->ResourceBarrier(1, &barrier);
    if (d.timestampHeap && d.timestampReadback) {
        d.list->EndQuery(d.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 3);
        d.list->ResolveQueryData(d.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase, 4,
            d.timestampReadback.Get(), UINT64(queryBase) * sizeof(UINT64));
        d.gpuTimestampPending[frame] = true;
    }
    hr = d.list->Close();
    if (FAILED(hr)) return false;

    ID3D12CommandList* postLists[] = {d.list.Get()};
    gDLSS.Marker(sl::PCLMarker::eRenderSubmitStart);
    d.queue->ExecuteCommandLists(1, postLists);
    gDLSS.Marker(sl::PCLMarker::eRenderSubmitEnd);
    LARGE_INTEGER postSubmitted{}; QueryPerformanceCounter(&postSubmitted);

    if (d.qpcFrequency.QuadPart > 0) {
        if (d.captureSignalQpc.QuadPart && postSubmitted.QuadPart >= d.captureSignalQpc.QuadPart) {
            double ms = 1000.0 * double(postSubmitted.QuadPart - d.captureSignalQpc.QuadPart) / double(d.qpcFrequency.QuadPart);
            d.captureToSubmitSumMs += ms; d.captureToSubmitMaxMs = std::max(d.captureToSubmitMaxMs, ms);
        }
        if (postSubmitted.QuadPart >= preSubmitted.QuadPart) {
            double ms = 1000.0 * double(postSubmitted.QuadPart - preSubmitted.QuadPart) / double(d.qpcFrequency.QuadPart);
            d.preToPostSubmitSumMs += ms; d.preToPostSubmitMaxMs = std::max(d.preToPostSubmitMaxMs, ms);
        }
        ++d.stageSamples;
    }

    renderDone = d.nextRenderFenceValue++;
    hr = d.queue->Signal(d.renderFence.Get(), renderDone);
    if (FAILED(hr)) return false;
    UINT sync = d.allowTearing ? 0 : 1;
    UINT flags = d.allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    gDLSS.Marker(sl::PCLMarker::ePresentStart);
    LARGE_INTEGER atPresent{}; QueryPerformanceCounter(&atPresent);
    if (d.qpcFrequency.QuadPart > 0 && d.claimQpc.QuadPart > 0 &&
        atPresent.QuadPart >= d.claimQpc.QuadPart) {
        const double ms = 1000.0 * double(atPresent.QuadPart - d.claimQpc.QuadPart) /
                          double(d.qpcFrequency.QuadPart);
        d.claimToPresentSumMs += ms;
        d.claimToPresentMaxMs = std::max(d.claimToPresentMaxMs, ms);
    }
    if (d.qpcFrequency.QuadPart > 0 && d.captureTimestamp.QuadPart > 0 &&
        atPresent.QuadPart >= d.captureTimestamp.QuadPart) {
        double ageMs = 1000.0 * double(atPresent.QuadPart - d.captureTimestamp.QuadPart) / double(d.qpcFrequency.QuadPart);
        d.ageSumMs += ageMs; d.ageMaxMs = std::max(d.ageMaxMs, ageMs); ++d.ageSamples;
    }
    hr = d.swapchain->Present(sync, flags);
    gDLSS.Marker(sl::PCLMarker::ePresentEnd);
    if (FAILED(hr)) return false;
    gDLSS.AfterPresent();
    // Monotonic fence allocation avoids scanning both backbuffer fence values on
    // every Present.  Each backbuffer still remembers the value protecting its
    // allocator/resource reuse.
    UINT64 own = d.nextFrameFenceValue++;
    hr = d.queue->Signal(d.frameFence.Get(), own);
    if (FAILED(hr)) return false;
    d.frameFenceValues[frame] = own;
    return true;
}

void App::DestroyDx12(Dx12State& d) {
    if (d.queue && d.frameFence) {
        UINT64 value = d.nextFrameFenceValue++;
        if (SUCCEEDED(d.queue->Signal(d.frameFence.Get(), value)) && d.frameEvent) {
            d.frameFence->SetEventOnCompletion(value, d.frameEvent);
            WaitForSingleObject(d.frameEvent, 3000);
        }
    }
    if (d.frameEvent) { CloseHandle(d.frameEvent); d.frameEvent = nullptr; }
    if (d.latencyWaitableObject) { CloseHandle(d.latencyWaitableObject); d.latencyWaitableObject = nullptr; }
}

void App::Worker(HWND target, HWND outputWindow) {
    {
    CaptureState c;
    {
    Dx12State d;
    WgcCaptureBackend wgc;
    struct FrameNotification {
        HANDLE event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        ~FrameNotification() { if(event) CloseHandle(event); }
    } notification;
    std::thread captureThread;
    std::atomic<bool> captureThreadStop{false};
    std::atomic<bool> captureEnabled{false};
    std::atomic<bool> captureFault{false};
    std::atomic<unsigned> mailboxOverwrites{0};
    std::atomic<unsigned> mailboxBusyDrops{0};
    std::atomic<unsigned> rendererStaleDrops{0};
    std::atomic<unsigned> rendererAgeDrops{0};
    std::atomic<unsigned> rendererClaimReplacements{0};
    std::atomic<int> sourceIntervalUs{8333};
    try {
        PostStatus(L"V13: initializing capture and DLSS...");
        gLog.Write(L"V13 repaired capture-backend build. DuplicateOutput1 when available / single-threaded D3D11 capture / discard-copy mailbox writes / freshest-claim scheduling / GPU stage timestamps. Selected multiplier: " + std::to_wstring(multiplier_) + L"x.");
        QueryPerformanceFrequency(&d.qpcFrequency);
        {
            std::wstring reason;
            if (!focus_compat::Install(target, outputWindow, reason)) {
                gLog.Write(reason);
                throw std::runtime_error("Focus compatibility could not initialize. See log and restart the app.");
            }
            gLog.Write(L"Focus compatibility installed only in this process's sl.common.dll. Game retains real Windows focus.");
        }
        uint32_t width=0,height=0;
        if(!InitCapture(target,c,width,height,useWgc_)) throw std::runtime_error("Capture initialization failed. See earlier log entry.");
        if (useWgc_) {
            std::wstring wgcError;
            if (!wgc.Init(target, c.device.Get(), c.sharedFence.Get(), d.qpcFrequency, wgcError))
                throw std::runtime_error(std::string("WGC initialization failed: ") + std::string(wgcError.begin(), wgcError.end()));
            gLog.Write(L"V12 active backend: Windows Graphics Capture (FreeThreaded frame pool).");
        } else {
            gLog.Write(L"V12 active backend: DXGI Desktop Duplication.");
        }
        if(!InitDx12(outputWindow,c,width,height,lowLatency_,d)) throw std::runtime_error("DX12 initialization failed. See log.");
        RECT output{};GetClientRect(outputWindow,&output);
        ID3D12Resource* captureSources[kCaptureSlots]{};
        for(UINT i=0;i<kCaptureSlots;++i) captureSources[i]=d.sharedTextures[i].Get();
        gDLSS.Init(d.device.Get(),c.adapter.Get(),captureSources,kCaptureSlots,output.right,output.bottom,false,true,multiplier_,true);
        if(!CreateSwapchain(outputWindow,lowLatency_,d,false)) throw std::runtime_error("Swapchain creation failed.");
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        d.device->CreateShaderResourceView(gDLSS.current.Get(),&view,d.srvHeap->GetCPUDescriptorHandleForHeapStart());

        // V9: capture owns Desktop Duplication/D3D11 exclusively.  It writes into
        // a 2-slot mailbox and publishes only completed copies.  READY slots may be
        // overwritten when the renderer has not claimed them, so stale capture
        // frames never form a queue.
        captureThread = std::thread([&, width, height]() {
            HRESULT apartmentHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            struct ApartmentGuard { HRESULT hr; ~ApartmentGuard() { if(SUCCEEDED(hr)) CoUninitialize(); } } apartment{apartmentHr};
            if (useWgc_ && FAILED(apartmentHr)) {
                gLog.Write(L"WGC capture-thread COM initialization failed: " + WinError(apartmentHr));
                captureFault.store(true, std::memory_order_release); return;
            }
            ConfigureLowLatencyThread(true);
            UINT64 sequence=1;
            unsigned captureMisses=0;
            LARGE_INTEGER lastCaptureQpc{};
            double expectedIntervalMs = 1000.0 / 120.0;
            while(!stop_ && !captureThreadStop.load(std::memory_order_acquire) &&
                  IsWindow(target) && IsWindow(outputWindow)) {
                if(!captureEnabled.load(std::memory_order_acquire) || paused_) {
                    Sleep(1);
                    continue;
                }

                DXGI_OUTDUPL_FRAME_INFO info{};
                ComPtr<IDXGIResource> resource;
                ComPtr<ID3D11Texture2D> sourceTexture;
                LARGE_INTEGER sourceTimestamp{};
                HRESULT hr=S_OK;
                bool wgcSizeChanged=false;
                if (useWgc_) {
                    std::wstring wgcError;
                    if (!wgc.TryGetNextFrame(sourceTexture, sourceTimestamp, wgcSizeChanged, wgcError)) {
                        gLog.Write(L"V12 WGC frame failure: " + wgcError);
                        captureFault.store(true,std::memory_order_release);
                        break;
                    }
                    if (wgcSizeChanged) {
                        gLog.Write(L"V12 WGC source size changed; restart required to rebuild fixed-size FG resources.");
                        captureFault.store(true,std::memory_order_release);
                        break;
                    }
                    if (!sourceTexture) {
                        LARGE_INTEGER nowQpc{}; QueryPerformanceCounter(&nowQpc);
                        wgc.WaitForFrame(2);
                        continue;
                    }
                } else {
                    hr=c.duplication->AcquireNextFrame(2,&info,&resource);
                    if(hr==DXGI_ERROR_WAIT_TIMEOUT) {
                        LARGE_INTEGER nowQpc{}; QueryPerformanceCounter(&nowQpc);
                        continue;
                    }
                    if (hr == DXGI_ERROR_ACCESS_LOST) {
                        c.duplication.Reset();
                        gLog.Write(L"V13: DXGI access lost; recreating duplication.");
                        for (unsigned attempt=0; attempt<20 && !stop_ && !captureThreadStop.load(); ++attempt) {
                            ComPtr<IDXGIOutput5> out5;
                            if(SUCCEEDED(c.output.As(&out5))) {
                                const DXGI_FORMAT formats[]={DXGI_FORMAT_B8G8R8A8_UNORM};
                                hr=out5->DuplicateOutput1(c.device.Get(),0,1,formats,&c.duplication);
                            }
                            if(!c.duplication) hr=c.output->DuplicateOutput(c.device.Get(),&c.duplication);
                            if(SUCCEEDED(hr) && c.duplication) break;
                            Sleep(50);
                        }
                        if(c.duplication) { gLog.Write(L"V13: DXGI duplication recovered."); continue; }
                    }
                    if(FAILED(hr)) {
                        gLog.Write(L"V12 DXGI AcquireNextFrame failed: "+WinError(hr));
                        captureFault.store(true,std::memory_order_release);
                        break;
                    }
                    if(!info.LastPresentTime.QuadPart || !info.AccumulatedFrames) { c.duplication->ReleaseFrame(); continue; }
                    hr=resource.As(&sourceTexture);
                    if(FAILED(hr)) { c.duplication->ReleaseFrame(); captureFault.store(true); break; }
                    sourceTimestamp=info.LastPresentTime;
                }
                captureMisses = 0;

                struct FrameLease {
                    IDXGIOutputDuplication* d{};
                    WgcCaptureBackend* wgc{};
                    UINT64 copyValue{};
                    ~FrameLease(){if(d)d->ReleaseFrame(); if(wgc)wgc->FinishFrame(copyValue);}
                } lease{useWgc_ ? nullptr : c.duplication.Get(), useWgc_ ? &wgc : nullptr};

                int chosen=-1;
                const UINT64 released=c.renderFence ? c.renderFence->GetCompletedValue() : UINT64_MAX;

                // Prefer a FREE slot whose previous GPU reader is already complete.
                for(UINT i=0;i<kCaptureSlots;++i) {
                    if(c.slots[i].state.load(std::memory_order_acquire)!=0) continue;
                    if(c.slots[i].releaseValue && c.slots[i].releaseValue>released) continue;
                    int expected=0;
                    if(c.slots[i].state.compare_exchange_strong(expected,3,std::memory_order_acq_rel)) {
                        chosen=(int)i; break;
                    }
                }

                // If all immediately-free slots are busy on the GPU, overwrite the
                // oldest READY slot.  No renderer owns a READY slot until its CAS to
                // IN_USE succeeds, so this is a safe stale-frame drop.
                if(chosen<0) {
                    UINT64 oldest=UINT64_MAX; int candidate=-1;
                    for(UINT i=0;i<kCaptureSlots;++i) {
                        if(c.slots[i].state.load(std::memory_order_acquire)==1 &&
                           c.slots[i].sequence.load(std::memory_order_acquire)<oldest) {
                            oldest=c.slots[i].sequence.load(std::memory_order_relaxed); candidate=(int)i;
                        }
                    }
                    if(candidate>=0) {
                        int expected=1;
                        if(c.slots[candidate].state.compare_exchange_strong(expected,3,std::memory_order_acq_rel)) {
                            chosen=candidate;
                            mailboxOverwrites.fetch_add(1,std::memory_order_relaxed);
                        }
                    }
                }

                // Last resort: claim a FREE slot and enqueue a GPU-side wait on the
                // render-release fence.  This does not block the capture CPU thread.
                if(chosen<0) {
                    for(UINT i=0;i<kCaptureSlots;++i) {
                        int expected=0;
                        if(c.slots[i].state.compare_exchange_strong(expected,3,std::memory_order_acq_rel)) {
                            chosen=(int)i; break;
                        }
                    }
                }

                if(chosen<0) {
                    mailboxBusyDrops.fetch_add(1,std::memory_order_relaxed);
                    continue;
                }

                auto& slot=c.slots[(UINT)chosen];
                if(slot.releaseValue && c.renderFence &&
                   c.renderFence->GetCompletedValue()<slot.releaseValue) {
                    hr=c.context4->Wait(c.renderFence.Get(),slot.releaseValue);
                    if(FAILED(hr)) {
                        slot.state.store(0,std::memory_order_release);
                        gLog.Write(L"V12 capture render-release wait failed: "+WinError(hr));
                        captureFault.store(true,std::memory_order_release);
                        break;
                    }
                }

                if(!sourceTexture) {
                    slot.state.store(0,std::memory_order_release);
                    captureFault.store(true,std::memory_order_release);
                    break;
                }
                D3D11_TEXTURE2D_DESC sourceDesc{}; sourceTexture->GetDesc(&sourceDesc);
                RECT currentClient{};
                if(!GetClientScreenRect(target,currentClient) ||
                   currentClient.right-currentClient.left != (LONG)width ||
                   currentClient.bottom-currentClient.top != (LONG)height) {
                    slot.state.store(0,std::memory_order_release);
                    gLog.Write(L"Source window resized/closed; stop and restart capture at the new size.");
                    captureFault.store(true); break;
                }
                c.sourceRect=currentClient;
                D3D11_BOX box{};
                if (useWgc_) {
                    RECT wr{};
                    if(FAILED(DwmGetWindowAttribute(target,DWMWA_EXTENDED_FRAME_BOUNDS,&wr,sizeof(wr)))) GetWindowRect(target,&wr);
                    box.left=(UINT)std::max<LONG>(0,c.sourceRect.left-wr.left);
                    box.top=(UINT)std::max<LONG>(0,c.sourceRect.top-wr.top);
                } else {
                    RECT out=c.outputDesc.DesktopCoordinates;
                    box.left=(UINT)(c.sourceRect.left-out.left); box.top=(UINT)(c.sourceRect.top-out.top);
                }
                if (box.left + width > sourceDesc.Width || box.top + height > sourceDesc.Height) {
                    slot.state.store(0,std::memory_order_release);
                    gLog.Write(L"V12 capture crop no longer fits source surface; restart after window resize.");
                    captureFault.store(true,std::memory_order_release);
                    break;
                }
                box.right=box.left+width;box.bottom=box.top+height;box.back=1;
                LARGE_INTEGER producerStart{}; QueryPerformanceCounter(&producerStart);
                c.context->CopySubresourceRegion(slot.texture.Get(),0,0,0,0,sourceTexture.Get(),0,&box);
                UINT64 ready=c.nextFenceValue++;
                hr=c.context4->Signal(c.sharedFence.Get(),ready);
                if(FAILED(hr)) {
                    slot.state.store(0,std::memory_order_release);
                    gLog.Write(L"V12 capture-ready signal failed: "+WinError(hr));
                    captureFault.store(true,std::memory_order_release);
                    break;
                }
                // Desktop Duplication is a D3D11 API. The copy + interop fence must
                // be submitted before D3D12/NVOFA may consume this slot. Keep one
                // asynchronous Flush here; removing it can leave the signal buffered.
                c.context->Flush();
                lease.copyValue=ready;

                LARGE_INTEGER signalQpc{}; QueryPerformanceCounter(&signalQpc);
                if (d.qpcFrequency.QuadPart > 0 && producerStart.QuadPart > 0 && signalQpc.QuadPart >= producerStart.QuadPart) {
                    const double producerMs = 1000.0 * double(signalQpc.QuadPart - producerStart.QuadPart) / double(d.qpcFrequency.QuadPart);
                    std::lock_guard statsLock(c.producerStatsMutex);
                    c.producerSubmitSumMs += producerMs;
                    c.producerSubmitMaxMs = std::max(c.producerSubmitMaxMs, producerMs);
                    ++c.producerSubmitSamples;
                }
                if (lastCaptureQpc.QuadPart > 0 && d.qpcFrequency.QuadPart > 0 && sourceTimestamp.QuadPart > lastCaptureQpc.QuadPart) {
                    const double intervalMs = 1000.0 * double(sourceTimestamp.QuadPart - lastCaptureQpc.QuadPart) /
                                              double(d.qpcFrequency.QuadPart);
                    if (intervalMs > 2.0 && intervalMs < 25.0) {
                        expectedIntervalMs = expectedIntervalMs * 0.90 + intervalMs * 0.10;
                        sourceIntervalUs.store((int)std::lround(expectedIntervalMs * 1000.0), std::memory_order_release);
                    }
                }
                lastCaptureQpc = sourceTimestamp;
                slot.readyValue=ready;
                slot.sequence.store(sequence++,std::memory_order_release);
                slot.presentTimestamp=sourceTimestamp;
                slot.signalQpc=signalQpc;
                slot.state.store(1,std::memory_order_release);
                if(notification.event) SetEvent(notification.event);
            }
        });

        ConfigureLowLatencyThread(false);
        auto lastReport=std::chrono::steady_clock::now();auto lastFrame=lastReport;
        unsigned presented=0;bool visible=true;
        bool pacingReady=false;
        bool waitingForFocus=false;
        auto lastNoFrameReport=std::chrono::steady_clock::now();
        PostStatus(L"Ready: switch to your selected game window to begin capture.");
        while(!stop_&&IsWindow(target)&&IsWindow(outputWindow)) {
            if(captureFault.load(std::memory_order_acquire))
                throw std::runtime_error("Capture thread stopped after a DXGI/D3D11 error. Check the log.");

            HWND foreground=GetForegroundWindow();
            bool active=foreground==target||foreground==outputWindow||
                        GetAncestor(foreground,GA_ROOT)==target;
            if(paused_||!active||IsIconic(target)) {
                if(!waitingForFocus) { PostStatus(paused_ ? L"Paused (F8 resumes)." : L"Waiting for game focus: click your selected game window."); waitingForFocus=true; }
                captureEnabled.store(false,std::memory_order_release);
                for(auto& slot:c.slots) {
                    int expected=1;
                    slot.state.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
                }
                focus_compat::Enable(false);
                gDLSS.Suspend(true);
                if(visible) {PostMessageW(outputWindow,WM_APP_OUTPUT_VISIBILITY,FALSE,0);visible=false;}
                gDLSS.ResetHistory();Sleep(10);continue;
            }
            if(waitingForFocus) { PostStatus(L"Game focused: waiting for first captured frame..."); waitingForFocus=false; }
            focus_compat::Enable(true);
            gDLSS.Suspend(false);
            if(!captureEnabled.exchange(true,std::memory_order_acq_rel)) {
                // Do not consume a READY image captured before the app regained focus.
                for(auto& slot:c.slots) {
                    int expected=1;
                    slot.state.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
                }
                gDLSS.ResetHistory();
            }
            if(!visible) {PostMessageW(outputWindow,WM_APP_OUTPUT_VISIBILITY,TRUE,0);visible=true;}
            if(recreateSwapchain_.exchange(false)) throw std::runtime_error("Output resized: press Start again to rebuild DLSS resources.");
            if (stop_ || paused_) continue;

            // Render/present pacing is independent from Desktop Duplication now.
            if (!pacingReady && d.latencyWaitableObject) {
                DWORD wr = WaitForSingleObjectEx(d.latencyWaitableObject, 1000, FALSE);
                if (wr != WAIT_OBJECT_0)
                    throw std::runtime_error("DXGI frame-latency wait timed out before render.");
                pacingReady=true;
            }

            UINT preCaptureFrame=d.swapchain->GetCurrentBackBufferIndex();
            if(d.frameFenceValues[preCaptureFrame] &&
               d.frameFence->GetCompletedValue()<d.frameFenceValues[preCaptureFrame]) {
                HR(d.frameFence->SetEventOnCompletion(d.frameFenceValues[preCaptureFrame],d.frameEvent),
                   "Pre-render backbuffer completion");
                if(WaitForSingleObject(d.frameEvent,3000)!=WAIT_OBJECT_0)
                    throw std::runtime_error("Backbuffer stalled before render.");
            }

            // Pay Streamline's previous-input dependency before claiming a mailbox
            // frame so the chosen capture remains as fresh as possible.
            // Keep a pending token/pacing grant across empty capture polls.
            gDLSS.PrepareFrame();

            // Claim the newest READY slot.  If a newer frame arrived while older
            // captures were waiting, discard those older READY slots immediately.
            int chosen=-1;
            for(;;) {
                UINT64 newest=0; int candidate=-1;
                for(UINT i=0;i<kCaptureSlots;++i) {
                    if(c.slots[i].state.load(std::memory_order_acquire)==1 &&
                       c.slots[i].sequence.load(std::memory_order_acquire)>newest) {
                        newest=c.slots[i].sequence.load(std::memory_order_relaxed); candidate=(int)i;
                    }
                }
                if(candidate<0) break;

                // Inspect frame metadata only after taking IN_USE ownership.
                // The sole newest frame remains usable even if the game is static.
                int expected=1;
                if(c.slots[candidate].state.compare_exchange_strong(expected,2,std::memory_order_acq_rel)) {
                    chosen=candidate; break;
                }
            }
            if(chosen<0) {
                if(notification.event) WaitForSingleObject(notification.event,2); else Sleep(1);
                if(std::chrono::steady_clock::now()-lastNoFrameReport>std::chrono::seconds(3)) {
                    PostStatus(L"No new captured frames yet. Keep the game visible; try borderless/windowed mode.");
                    lastNoFrameReport=std::chrono::steady_clock::now();
                }
                continue;
            }
            lastNoFrameReport=std::chrono::steady_clock::now();

            for(UINT i=0;i<kCaptureSlots;++i) {
                if((int)i==chosen) continue;
                int expected=1;
                if(c.slots[i].state.compare_exchange_strong(expected,0,std::memory_order_acq_rel))
                    rendererStaleDrops.fetch_add(1,std::memory_order_relaxed);
            }

            auto& slot=c.slots[(UINT)chosen];
            d.captureTimestamp=slot.presentTimestamp;
            d.captureSignalQpc=slot.signalQpc;
            QueryPerformanceCounter(&d.claimQpc);
            auto now=std::chrono::steady_clock::now();
            if(now-lastFrame>std::chrono::milliseconds(100)) gDLSS.ResetHistory();
            lastFrame=now;

            // V10 last-moment replacement: after all CPU-side pacing/history work
            // but before RenderFrame submits any GPU read of the claimed texture,
            // check whether capture published something newer.  If so, atomically
            // claim the newer slot and release the older claim without ever putting
            // stale work on the GPU.  With two slots this is bounded and cheap.
            {
                UINT64 currentSeq = slot.sequence.load(std::memory_order_acquire);
                int fresher = -1; UINT64 fresherSeq = currentSeq;
                for(UINT i=0;i<kCaptureSlots;++i) {
                    if((int)i==chosen) continue;
                    if(c.slots[i].state.load(std::memory_order_acquire)!=1) continue;
                    const UINT64 seq = c.slots[i].sequence.load(std::memory_order_acquire);
                    if(seq > fresherSeq) { fresherSeq = seq; fresher = (int)i; }
                }
                if(fresher >= 0) {
                    int ready=1;
                    if(c.slots[(UINT)fresher].state.compare_exchange_strong(ready,2,std::memory_order_acq_rel)) {
                        // The old slot has not been submitted to D3D12/NVOFA yet, so
                        // no release fence is required; return it directly to FREE.
                        c.slots[(UINT)chosen].state.store(0,std::memory_order_release);
                        chosen=fresher;
                        auto& newer=c.slots[(UINT)chosen];
                        d.captureTimestamp=newer.presentTimestamp;
                        d.captureSignalQpc=newer.signalQpc;
                        QueryPerformanceCounter(&d.claimQpc);
                        rendererClaimReplacements.fetch_add(1,std::memory_order_relaxed);
                    }
                }
            }

            auto& renderSlot=c.slots[(UINT)chosen];
            // Measure the mailbox age of the frame that will actually be submitted,
            // after any last-moment replacement.
            if (d.qpcFrequency.QuadPart > 0 && renderSlot.signalQpc.QuadPart > 0 &&
                d.claimQpc.QuadPart >= renderSlot.signalQpc.QuadPart) {
                const double ms = 1000.0 * double(d.claimQpc.QuadPart - renderSlot.signalQpc.QuadPart) /
                                  double(d.qpcFrequency.QuadPart);
                d.mailboxToClaimSumMs += ms;
                d.mailboxToClaimMaxMs = std::max(d.mailboxToClaimMaxMs, ms);
                ++d.mailboxSamples;
            }
            gDLSS.BeforeFrame(d.queue.Get());
            UINT64 renderDone=0;
            if(!RenderFrame(c,d,(UINT)chosen,renderSlot.readyValue,renderDone))
                throw std::runtime_error("Present failed. Check NVIDIA Streamline logs.");

            // Publish the D3D12 release fence value before returning ownership to
            // capture.  Capture will GPU-wait on this value before reusing the slot.
            renderSlot.releaseValue=renderDone;
            renderSlot.state.store(0,std::memory_order_release);

            pacingReady=false;
            if(presented==0) gLog.Write(L"V13 frame successfully presented.");
            ++presented;
            now=std::chrono::steady_clock::now();double elapsed=std::chrono::duration<double>(now-lastReport).count();
            if(elapsed>=1.0) {
                gLog.Write(focus_compat::Report());
                if (d.ageSamples) {
                    wchar_t age[240]{};
                    swprintf_s(age, L"Desktop-update age at Present call (CPU): avg %.2f ms, max %.2f ms. Not input-to-screen latency.",
                        d.ageSumMs / d.ageSamples, d.ageMaxMs);
                    gLog.Write(age);
                }
                if (d.stageSamples) {
                    wchar_t stage[320]{};
                    swprintf_s(stage, L"V11 submit timing (CPU): capture-signal->post-submit avg %.3f ms max %.3f ms | pre-submit->post-submit avg %.3f ms max %.3f ms.",
                        d.captureToSubmitSumMs / d.stageSamples, d.captureToSubmitMaxMs,
                        d.preToPostSubmitSumMs / d.stageSamples, d.preToPostSubmitMaxMs);
                    gLog.Write(stage);
                }
                if (d.mailboxSamples) {
                    wchar_t sched[320]{};
                    swprintf_s(sched, L"V11 scheduling timing (CPU): mailbox-ready->claim avg %.3f ms max %.3f ms | claim->Present avg %.3f ms max %.3f ms.",
                        d.mailboxToClaimSumMs / d.mailboxSamples, d.mailboxToClaimMaxMs,
                        d.claimToPresentSumMs / d.mailboxSamples, d.claimToPresentMaxMs);
                    gLog.Write(sched);
                }
                {
                    double sumMs{},maxMs{}; unsigned samples{};
                    { std::lock_guard statsLock(c.producerStatsMutex);
                      sumMs=c.producerSubmitSumMs; maxMs=c.producerSubmitMaxMs; samples=c.producerSubmitSamples;
                      c.producerSubmitSumMs=c.producerSubmitMaxMs=0; c.producerSubmitSamples=0; }
                    if(samples) {
                    wchar_t producer[256]{};
                    swprintf_s(producer, L"V12 producer CPU: copy+signal+Flush avg %.3f ms max %.3f ms (%u samples).",
                        sumMs / double(samples), maxMs, samples);
                    gLog.Write(producer);
                    }
                }
                if (d.gpuSamples) {
                    wchar_t gpu[384]{};
                    swprintf_s(gpu, L"V11 GPU timestamps: pre-motion avg %.3f ms max %.3f | NVOFA/wait gap avg %.3f ms max %.3f | post-motion+present-prep avg %.3f ms max %.3f.",
                        d.gpuPreSumMs / d.gpuSamples, d.gpuPreMaxMs,
                        d.gpuWaitGapSumMs / d.gpuSamples, d.gpuWaitGapMaxMs,
                        d.gpuPostSumMs / d.gpuSamples, d.gpuPostMaxMs);
                    gLog.Write(gpu);
                }
                wchar_t mailbox[280]{};
                const double sourceMs = sourceIntervalUs.load(std::memory_order_acquire) / 1000.0;
                const double staleMs = std::clamp(sourceMs * 0.92, kMailboxStaleFloorMs, kMailboxStaleCeilMs);
                swprintf_s(mailbox,L"V11 mailbox: capture-overwrite=%u, capture-all-slots-busy=%u, renderer-stale=%u, renderer-age-reject=%u, last-moment-replace=%u | source %.3f ms stale-limit %.3f ms.",
                    mailboxOverwrites.exchange(0),mailboxBusyDrops.exchange(0),rendererStaleDrops.exchange(0),rendererAgeDrops.exchange(0),
                    rendererClaimReplacements.exchange(0),sourceMs,staleMs);
                gLog.Write(mailbox);
                d.ageSumMs = d.ageMaxMs = 0; d.ageSamples = 0;
                d.captureToSubmitSumMs = d.captureToSubmitMaxMs = 0;
                d.preToPostSubmitSumMs = d.preToPostSubmitMaxMs = 0; d.stageSamples = 0;
                d.mailboxToClaimSumMs = d.mailboxToClaimMaxMs = 0;
                d.claimToPresentSumMs = d.claimToPresentMaxMs = 0; d.mailboxSamples = 0;
                d.gpuPreSumMs = d.gpuPreMaxMs = 0;
                d.gpuWaitGapSumMs = d.gpuWaitGapMaxMs = 0;
                d.gpuPostSumMs = d.gpuPostMaxMs = 0; d.gpuSamples = 0;

                PostStatus(L"Captured/base presents: "+std::to_wstring(unsigned(presented/elapsed))+L" FPS | "+gDLSS.Status());
                presented=0;lastReport=now;
            }
        }
    } catch(const std::exception& e) {
        std::string message=e.what();PostStatus(L"Stopped: "+std::wstring(message.begin(),message.end()));
    }

    captureEnabled.store(false,std::memory_order_release);
    captureThreadStop.store(true,std::memory_order_release);
    if(captureThread.joinable()) captureThread.join();
    // All submitted WGC copies must complete before their leased surfaces close.
    if(useWgc_ && c.sharedFence && c.nextFenceValue>1) {
        HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(done) { if(SUCCEEDED(c.sharedFence->SetEventOnCompletion(c.nextFenceValue-1,done))) WaitForSingleObject(done,3000); CloseHandle(done); }
    }
    wgc.Shutdown();

    if (d.queue) {
        try { gDLSS.BeforeFrame(d.queue.Get()); } catch (const std::exception& e) {
            std::string m=e.what(); gLog.Write(std::wstring(m.begin(),m.end()));
        }
    }
    focus_compat::Enable(false);
    DestroyDx12(d);
    gDLSS.Release();
    if (!focus_compat::Remove()) gLog.Write(L"Could not fully restore local focus import. Close this app before another test.");
    gDLSS.EndRuntime();
    }
    for(auto& slot:c.slots) if(slot.textureHandle) CloseHandle(slot.textureHandle);
    if(c.fenceHandle)CloseHandle(c.fenceHandle);
    if(c.renderFenceHandle)CloseHandle(c.renderFenceHandle);
    }
    gDLSS.Shutdown();
    PostMessageW(control_,WM_APP_STOPPED,0,0);
}

LRESULT App::OnControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH: RefreshTargets(); return 0;
        case IDC_START: Start(); return 0;
        case IDC_STOP: Stop(); EnableWindow(GetDlgItem(control_, IDC_START), TRUE);
            EnableWindow(GetDlgItem(control_, IDC_MULTIPLIER), TRUE);
            EnableWindow(GetDlgItem(control_, IDC_CAPTURE_BACKEND), TRUE);
            EnableWindow(GetDlgItem(control_, IDC_STOP), FALSE); return 0;
        }
        break;
    case WM_HOTKEY:
        if (wp == 1 && running_) ToggleFullscreen();
        if (wp == 2 && running_) paused_ = !paused_;
        if (wp == 3 && running_) stop_ = true;
        return 0;
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        SetWindowTextW(status_, text->c_str()); delete text; return 0;
    }
    case WM_APP_STOPPED:
        Stop();
        EnableWindow(GetDlgItem(control_, IDC_MULTIPLIER), TRUE);
        EnableWindow(GetDlgItem(control_, IDC_CAPTURE_BACKEND), TRUE);
        EnableWindow(GetDlgItem(control_, IDC_START), TRUE);
        EnableWindow(GetDlgItem(control_, IDC_STOP), FALSE);
        return 0;
    case WM_CLOSE: Stop(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, 1); UnregisterHotKey(hwnd, 2); UnregisterHotKey(hwnd, 3);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::OnPresenterMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_OUTPUT_VISIBILITY:
        if (wp) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SIZE: if (running_) recreateSwapchain_ = true; return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { stop_ = true; return 0; }
        if (wp == VK_F8) { paused_ = !paused_; return 0; }
        if (wp == VK_F11) { ToggleFullscreen(); return 0; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: stop_ = true; if(!running_) DestroyWindow(hwnd); return 0;
    case WM_DESTROY: if (presenter_ == hwnd) presenter_ = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::ControlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return gApp ? gApp->OnControlMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK App::PresenterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return gApp ? gApp->OnPresenterMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    App app;
    return app.Run(instance);
}
