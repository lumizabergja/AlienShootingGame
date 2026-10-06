#include <windows.h>
#include <commctrl.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "dlss_bridge.h"
#include "focus_compat.h"
#include "wgc_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kAppName[] = L"DX12 Presenter V25 - DLSS FG 2x / 3x / 4x";
constexpr wchar_t kControlClass[] = L"LoLDX12Presenter.Control";
constexpr wchar_t kPresenterClass[] = L"LoLDX12Presenter.Output";
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_STOPPED = WM_APP + 2;
constexpr UINT WM_APP_OUTPUT_VISIBILITY = WM_APP + 3;
constexpr DWORD WDA_EXCLUDEFROMCAPTURE_VALUE = 0x00000011;
constexpr UINT kFrameCount = 2;
constexpr UINT kAllocatorCount = 4;
constexpr UINT kCaptureSlots = 3;
constexpr UINT kGpuQueriesPerFrame = 5;
constexpr UINT kPreprocessQueriesPerFrame = 2;

enum ControlId : int {
    IDC_TARGET = 100, IDC_REFRESH, IDC_START, IDC_STOP,
    IDC_FULLSCREEN, IDC_LOW_LATENCY, IDC_STATUS, IDC_MULTIPLIER, IDC_CAPTURE_BACKEND, IDC_NVOF_GRID, IDC_REFLEX_TIMING, IDC_NVOF_COST
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

struct CaptureState {
    enum : LONG { kFree = 0, kReady = 1, kConsuming = 2, kWriting = 3, kHistory = 4 };
    struct Mailbox {
        std::atomic<LONG> state{kFree};
        UINT64 readyValue{};
        LARGE_INTEGER sourceTimestamp{};
        std::atomic<uint64_t> sequence{0};
    };
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutput5> output5;
    ComPtr<IDXGIOutputDuplication> duplication;
    bool duplicateOutput1{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Texture2D> sharedTextures[kCaptureSlots];
    ComPtr<ID3D11Fence> captureFence;
    ComPtr<ID3D11Fence> releaseFence;
    HANDLE textureHandles[kCaptureSlots]{};
    HANDLE captureFenceHandle{};
    HANDLE releaseFenceHandle{};
    RECT sourceRect{};
    DXGI_OUTPUT_DESC outputDesc{};
    UINT64 nextCaptureValue{1};
    UINT64 nextReleaseValue{1};
    std::atomic<UINT64> lastReleaseDone[kCaptureSlots]{};
    std::atomic<UINT64> lastHistoryDone{0};
    std::atomic<int> historySlot{-1};
    Mailbox mailboxes[kCaptureSlots];
};

struct Dx12State {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandQueue> preprocessQueue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Resource> backBuffers[kFrameCount];
    ComPtr<ID3D12CommandAllocator> allocators[kAllocatorCount];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> transferAllocators[kAllocatorCount];
    ComPtr<ID3D12GraphicsCommandList> transferCopyList;
    uint64_t parallelCopyFrames{}, rasterFrames{};
    ComPtr<ID3D12CommandAllocator> preprocessAllocators[kAllocatorCount];
    ComPtr<ID3D12GraphicsCommandList> preprocessList;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> sharedTextures[kCaptureSlots];
    ComPtr<ID3D12Fence> captureFence;
    ComPtr<ID3D12Fence> releaseFence;
    ComPtr<ID3D12Fence> frameFence;
    ComPtr<ID3D12Fence> preprocessFence;
    ComPtr<ID3D12QueryHeap> gpuQueryHeap;
    ComPtr<ID3D12QueryHeap> preprocessQueryHeap;
    ComPtr<ID3D12Resource> gpuQueryReadback;
    ComPtr<ID3D12Resource> preprocessQueryReadback;
    UINT64* gpuQueryData{};
    UINT64* preprocessQueryData{};
    UINT64 gpuTimestampFrequency{};
    UINT64 preprocessTimestampFrequency{};
    bool gpuProfileValid[kAllocatorCount]{};
    bool preprocessProfileValid[kAllocatorCount]{};
    uint64_t gpuProfileSamples{};
    uint64_t preprocessProfileSamples{};
    double gpuTagSumMs{}, gpuSetupSumMs{}, gpuDrawSumMs{}, gpuFinishSumMs{};
    double gpuTotalSumMs{}, gpuTotalMaxMs{};
    double preprocessGpuSumMs{}, preprocessGpuMaxMs{};
    uint64_t dependencySamples{}, captureFencePending{}, nvofFencePending{}, preprocessFencePending{};
    uint64_t presentSamples{};
    double presentSumMs{}, presentMaxMs{};
    HANDLE frameEvent{};
    UINT64 allocatorFenceValues[kAllocatorCount]{};
    UINT64 preprocessFenceValues[kAllocatorCount]{};
    UINT64 nextAllocatorFenceValue{1};
    UINT64 nextPreprocessFenceValue{1};
    UINT nextAllocator{};
    UINT nextPreprocessAllocator{};
    uint64_t allocatorWaitCount{}, preprocessAllocatorWaitCount{};
    double allocatorWaitSumMs{}, allocatorWaitMaxMs{};
    double preprocessAllocatorWaitSumMs{}, preprocessAllocatorWaitMaxMs{};
    UINT rtvStride{};
    UINT srvStride{};
    UINT width{};
    UINT height{};
    UINT sourceWidth{};
    UINT sourceHeight{};
    bool allowTearing{};
    LARGE_INTEGER captureTimestamp{}, qpcFrequency{};
    double ageSumMs{}, ageMaxMs{};
    unsigned ageSamples{};
};

void ConsumeGpuProfile(Dx12State& d, UINT allocatorIndex) {
    if (allocatorIndex >= kAllocatorCount || !d.gpuProfileValid[allocatorIndex] ||
        !d.gpuQueryData || !d.gpuTimestampFrequency) return;
    const UINT64* q = d.gpuQueryData + SIZE_T(allocatorIndex) * kGpuQueriesPerFrame;
    bool monotonic = true;
    for (UINT i = 1; i < kGpuQueriesPerFrame; ++i) if (q[i] < q[i - 1]) monotonic = false;
    if (monotonic && q[4] > q[0]) {
        const double scale = 1000.0 / double(d.gpuTimestampFrequency);
        d.gpuTagSumMs += double(q[1] - q[0]) * scale;
        d.gpuSetupSumMs  += double(q[2] - q[1]) * scale;
        d.gpuDrawSumMs   += double(q[3] - q[2]) * scale;
        d.gpuFinishSumMs += double(q[4] - q[3]) * scale;
        const double total = double(q[4] - q[0]) * scale;
        d.gpuTotalSumMs += total;
        d.gpuTotalMaxMs = std::max(d.gpuTotalMaxMs, total);
        ++d.gpuProfileSamples;
    }
    d.gpuProfileValid[allocatorIndex] = false;
}


void ConsumePreprocessProfile(Dx12State& d, UINT allocatorIndex) {
    if (allocatorIndex >= kAllocatorCount || !d.preprocessProfileValid[allocatorIndex] ||
        !d.preprocessQueryData || !d.preprocessTimestampFrequency) return;
    const UINT64* q = d.preprocessQueryData + SIZE_T(allocatorIndex) * kPreprocessQueriesPerFrame;
    if (q[1] >= q[0] && q[1] > q[0]) {
        const double ms = double(q[1] - q[0]) * (1000.0 / double(d.preprocessTimestampFrequency));
        d.preprocessGpuSumMs += ms;
        d.preprocessGpuMaxMs = std::max(d.preprocessGpuMaxMs, ms);
        ++d.preprocessProfileSamples;
    }
    d.preprocessProfileValid[allocatorIndex] = false;
}

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
    bool RenderFrame(CaptureState& c, Dx12State& d, UINT sourceSlot, UINT64 copyReady,
                     float syntheticOffsetPixels = 0.0f);
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
    HANDLE workerControlEvent_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> recreateSwapchain_{false};
    bool fullscreen_{true};
    UINT multiplier_{2};
    bool lowLatency_{true};
    bool useWgc_{false};
    UINT nvofGrid_{4};
    bool nvofCost_{true};
    UINT reflexTiming_{1};
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
        CW_USEDEFAULT, CW_USEDEFAULT, 590, 420, nullptr, nullptr, instance_, nullptr);
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
    SendMessageW(multiplier,CB_SETCURSEL,2,0);
    label(L"Capture",18,132,60,22);
    HWND backend=CreateWindowW(WC_COMBOBOXW,nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        85,127,250,120,control_,reinterpret_cast<HMENU>(IDC_CAPTURE_BACKEND),instance_,nullptr);
    SendMessageW(backend,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"DXGI Desktop Duplication (recommended low latency)"));
    SendMessageW(backend,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Windows Graphics Capture"));
    SendMessageW(backend,CB_SETCURSEL,0,0);
    label(L"OF Grid",350,132,55,22);
    HWND ofGrid=CreateWindowW(WC_COMBOBOXW,nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        410,127,150,100,control_,reinterpret_cast<HMENU>(IDC_NVOF_GRID),instance_,nullptr);
    SendMessageW(ofGrid,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"2x2 FAST"));
    SendMessageW(ofGrid,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"4x4 FAST"));
    SendMessageW(ofGrid,CB_SETCURSEL,1,0);
    CreateWindowW(L"BUTTON",L"OFA confidence cost (quality)",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,365,166,195,24,control_,reinterpret_cast<HMENU>(IDC_NVOF_COST),instance_,nullptr);
    SendDlgItemMessageW(control_,IDC_NVOF_COST,BM_SETCHECK,BST_CHECKED,0);
    label(L"Reflex sleep",18,170,80,22);
    HWND reflex=CreateWindowW(WC_COMBOBOXW,nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        105,165,250,120,control_,reinterpret_cast<HMENU>(IDC_REFLEX_TIMING),instance_,nullptr);
    SendMessageW(reflex,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Off (Reflex mode)"));
    SendMessageW(reflex,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Pre-capture (V14 behavior)"));
    SendMessageW(reflex,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"After frame arrival"));
    SendMessageW(reflex,CB_SETCURSEL,1,0);
    CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,18,210,180,36,control_,reinterpret_cast<HMENU>(IDC_START),instance_,nullptr);
    CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|WS_DISABLED,210,210,110,36,control_,reinterpret_cast<HMENU>(IDC_STOP),instance_,nullptr);
    status_=label(L"Choose capture backend, FG multiplier and Reflex timing, then Start.",18,265,540,65);
    label(L"F11 fullscreen | F8 pause | F10 stop",18,345,540,22);
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
    if (selected == CB_ERR) { SetWindowTextW(status_, L"Select the League game window first."); return; }
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
    if (selectedBackend < 0 || selectedBackend > 1) {
        SetWindowTextW(status_, L"Choose DXGI or Windows Graphics Capture before starting."); return;
    }
    useWgc_ = selectedBackend == 1;
    LRESULT selectedGrid = SendDlgItemMessageW(control_, IDC_NVOF_GRID, CB_GETCURSEL, 0, 0);
    if (selectedGrid < 0 || selectedGrid > 1) {
        SetWindowTextW(status_, L"Choose NVOFA 2x2 FAST or 4x4 FAST before starting."); return;
    }
    nvofGrid_ = selectedGrid == 0 ? 2u : 4u;
    nvofCost_ = SendDlgItemMessageW(control_,IDC_NVOF_COST,BM_GETCHECK,0,0)==BST_CHECKED;
    LRESULT selectedReflex = SendDlgItemMessageW(control_, IDC_REFLEX_TIMING, CB_GETCURSEL, 0, 0);
    if (selectedReflex < 0 || selectedReflex > 2) {
        SetWindowTextW(status_, L"Choose Reflex sleep timing before starting."); return;
    }
    reflexTiming_ = static_cast<UINT>(selectedReflex);

    stop_ = false;
    paused_ = false;
    recreateSwapchain_ = false;
    if (!CreatePresenter(target, fullscreen_)) {
        SetWindowTextW(status_, L"Could not create the DX12 output window. Restore League, press Refresh, and select it again.");
        return;
    }
    if (workerControlEvent_) { CloseHandle(workerControlEvent_); workerControlEvent_ = nullptr; }
    workerControlEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!workerControlEvent_) {
        SetWindowTextW(status_, L"Could not create worker control event.");
        if (presenter_ && IsWindow(presenter_)) { DestroyWindow(presenter_); presenter_ = nullptr; }
        return;
    }
    running_ = true;
    EnableWindow(GetDlgItem(control_, IDC_START), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_MULTIPLIER), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_CAPTURE_BACKEND), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_REFLEX_TIMING), FALSE);
    EnableWindow(GetDlgItem(control_, IDC_STOP), TRUE);
    worker_ = std::thread(&App::Worker, this, target, presenter_);
}

void App::Stop() {
    focus_compat::Enable(false);
    stop_ = true;
    if (workerControlEvent_) SetEvent(workerControlEvent_);
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    if (workerControlEvent_) { CloseHandle(workerControlEvent_); workerControlEvent_ = nullptr; }
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
    hr = D3D11CreateDevice(c.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &c.device, &obtained, &c.context);
    if (FAILED(hr)) { PostStatus(L"D3D11 capture device failed: " + WinError(hr)); return false; }
    hr = c.device.As(&c.device5);
    if (FAILED(hr)) { PostStatus(L"Windows/D3D11 shared-fence support is unavailable: " + WinError(hr)); return false; }
    hr = c.context.As(&c.context4);
    if (FAILED(hr)) { PostStatus(L"D3D11 context shared-fence support is unavailable: " + WinError(hr)); return false; }
    if (!useWgc) {
        // V23: prefer DuplicateOutput1 so DXGI can use the modern duplication path
        // and return the exact BGRA format consumed by the rest of the pipeline.
        // Fall back to DuplicateOutput on older systems/drivers rather than making
        // the new backend less robust than V22.
        if (SUCCEEDED(c.output.As(&c.output5)) && c.output5) {
            const DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = c.output5->DuplicateOutput1(c.device.Get(), 0, ARRAYSIZE(formats), formats, &c.duplication);
            if (SUCCEEDED(hr)) {
                c.duplicateOutput1 = true;
                gLog.Write(L"V23 DXGI capture: IDXGIOutput5::DuplicateOutput1 active (BGRA8 direct format).");
            } else {
                gLog.Write(L"V23 DuplicateOutput1 unavailable/failed; falling back to DuplicateOutput: " + WinError(hr));
                c.duplication.Reset();
            }
        }
        if (!c.duplication) {
            hr = c.output->DuplicateOutput(c.device.Get(), &c.duplication);
            if (FAILED(hr)) { PostStatus(L"Desktop capture failed. Close other capture apps and retry: " + WinError(hr)); return false; }
            gLog.Write(L"V23 DXGI capture: DuplicateOutput compatibility fallback active.");
        }

        RECT& desktop = c.outputDesc.DesktopCoordinates;
        RECT clipped{std::max(c.sourceRect.left, desktop.left), std::max(c.sourceRect.top, desktop.top),
                     std::min(c.sourceRect.right, desktop.right), std::min(c.sourceRect.bottom, desktop.bottom)};
        if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
            PostStatus(L"The League window is outside the selected monitor."); return false;
        }
        c.sourceRect = clipped;
        width = static_cast<uint32_t>(clipped.right - clipped.left);
        height = static_cast<uint32_t>(clipped.bottom - clipped.top);
    } else {
        width = static_cast<uint32_t>(c.sourceRect.right - c.sourceRect.left);
        height = static_cast<uint32_t>(c.sourceRect.bottom - c.sourceRect.top);
        if (!width || !height) { PostStatus(L"The source client area is empty."); return false; }
    }

    // Three shared capture mailboxes are required for true overlap: one can remain
    // reserved as NVOFA history, one can be consumed by DX12, and the third can
    // receive the newest desktop update. Capture-ready
    // and release-complete use different fences so values can never complete out
    // of order when D3D11 and D3D12 signal from independent queues.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (UINT i = 0; i < kCaptureSlots; ++i) {
        hr = c.device->CreateTexture2D(&td, nullptr, &c.sharedTextures[i]);
        if (FAILED(hr)) { PostStatus(L"D3D11 shared texture creation failed: " + WinError(hr)); return false; }
        ComPtr<IDXGIResource1> dxgiResource;
        hr = c.sharedTextures[i].As(&dxgiResource);
        if (FAILED(hr)) { PostStatus(L"Shared texture interface failed: " + WinError(hr)); return false; }
        hr = dxgiResource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &c.textureHandles[i]);
        if (FAILED(hr)) { PostStatus(L"Shared texture handle failed: " + WinError(hr)); return false; }
    }
    hr = c.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.captureFence));
    if (FAILED(hr)) { PostStatus(L"D3D11 capture fence creation failed: " + WinError(hr)); return false; }
    hr = c.captureFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &c.captureFenceHandle);
    if (FAILED(hr)) { PostStatus(L"Capture fence handle failed: " + WinError(hr)); return false; }
    hr = c.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.releaseFence));
    if (FAILED(hr)) { PostStatus(L"D3D11 release fence creation failed: " + WinError(hr)); return false; }
    hr = c.releaseFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &c.releaseFenceHandle);
    if (FAILED(hr)) { PostStatus(L"Release fence handle failed: " + WinError(hr)); return false; }
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
        HRESULT hr = d.swapchain->ResizeBuffers(kFrameCount, width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM, d.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
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
        desc.Flags = d.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> swap;
        HRESULT hr = d.factory->CreateSwapChainForHwnd(d.queue.Get(), presenter, &desc,
            nullptr, nullptr, &swap);
        if (FAILED(hr)) { PostStatus(L"DX12 swapchain creation failed: " + WinError(hr)); return false; }
        d.factory->MakeWindowAssociation(presenter, DXGI_MWA_NO_ALT_ENTER);
        hr = swap.As(&d.swapchain);
        if (FAILED(hr)) return false;
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
    D3D12_COMMAND_QUEUE_DESC cq{}; cq.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = d.device->CreateCommandQueue(&cq, IID_PPV_ARGS(&d.preprocessQueue));
    if (FAILED(hr)) { PostStatus(L"DX12 async preprocess queue failed: " + WinError(hr)); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = kFrameCount;
    hr = d.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&d.rtvHeap));
    if (FAILED(hr)) return false;
    d.rtvStride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; srv.NumDescriptors = kCaptureSlots;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = d.device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&d.srvHeap));
    if (FAILED(hr)) return false;
    d.srvStride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < kAllocatorCount; ++i) {
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.allocators[i]));
        if (FAILED(hr)) return false;
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
            IID_PPV_ARGS(&d.preprocessAllocators[i]));
        if (FAILED(hr)) return false;
    }
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.allocators[0].Get(), nullptr, IID_PPV_ARGS(&d.list));
    if (FAILED(hr)) return false;
    d.list->Close();
    for (UINT i=0; i<kAllocatorCount; ++i) {
        HR(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.transferAllocators[i])), "Create transfer allocator");
    }
    HR(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.transferAllocators[0].Get(), nullptr, IID_PPV_ARGS(&d.transferCopyList)), "Create transfer copy list");
    HR(d.transferCopyList->Close(), "Close transfer copy list");
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
        d.preprocessAllocators[0].Get(), nullptr, IID_PPV_ARGS(&d.preprocessList));
    if (FAILED(hr)) return false;
    d.preprocessList->Close();
    for (UINT i = 0; i < kCaptureSlots; ++i) {
        hr = d.device->OpenSharedHandle(c.textureHandles[i], IID_PPV_ARGS(&d.sharedTextures[i]));
        if (FAILED(hr)) {
            PostStatus(L"DX12 could not open a D3D11 shared capture texture: " + WinError(hr));
            return false;
        }
    }
    hr = d.device->OpenSharedHandle(c.captureFenceHandle, IID_PPV_ARGS(&d.captureFence));
    if (FAILED(hr)) { PostStatus(L"DX12 could not open the capture fence: " + WinError(hr)); return false; }
    hr = d.device->OpenSharedHandle(c.releaseFenceHandle, IID_PPV_ARGS(&d.releaseFence));
    if (FAILED(hr)) { PostStatus(L"DX12 could not open the release fence: " + WinError(hr)); return false; }
    hr = d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.frameFence));
    if (FAILED(hr)) return false;
    hr = d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.preprocessFence));
    if (FAILED(hr)) return false;
    d.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.frameEvent) return false;
    if (FAILED(d.queue->GetTimestampFrequency(&d.gpuTimestampFrequency)) || !d.gpuTimestampFrequency) return false;
    if (FAILED(d.preprocessQueue->GetTimestampFrequency(&d.preprocessTimestampFrequency)) || !d.preprocessTimestampFrequency) return false;
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = kAllocatorCount * kGpuQueriesPerFrame;
    if (FAILED(d.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&d.gpuQueryHeap)))) return false;
    D3D12_QUERY_HEAP_DESC preprocessQueryDesc{};
    preprocessQueryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    preprocessQueryDesc.Count = kAllocatorCount * kPreprocessQueriesPerFrame;
    if (FAILED(d.device->CreateQueryHeap(&preprocessQueryDesc, IID_PPV_ARGS(&d.preprocessQueryHeap)))) return false;
    D3D12_HEAP_PROPERTIES readbackHeap{}; readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = UINT64(queryDesc.Count) * sizeof(UINT64);
    readbackDesc.Height = 1; readbackDesc.DepthOrArraySize = 1; readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1; readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.gpuQueryReadback)))) return false;
    void* mapped = nullptr; D3D12_RANGE noRead{0,0};
    if (FAILED(d.gpuQueryReadback->Map(0, &noRead, &mapped)) || !mapped) return false;
    d.gpuQueryData = static_cast<UINT64*>(mapped);

    D3D12_RESOURCE_DESC preprocessReadbackDesc = readbackDesc;
    preprocessReadbackDesc.Width = UINT64(preprocessQueryDesc.Count) * sizeof(UINT64);
    if (FAILED(d.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &preprocessReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.preprocessQueryReadback)))) return false;
    mapped = nullptr;
    if (FAILED(d.preprocessQueryReadback->Map(0, &noRead, &mapped)) || !mapped) return false;
    d.preprocessQueryData = static_cast<UINT64*>(mapped);
    if (!CreatePipeline(d)) return false;
    return true;
}

bool App::RenderFrame(CaptureState& c, Dx12State& d, UINT sourceSlot, UINT64 copyReady,
                      float syntheticOffsetPixels) {
    sourceSlot %= kCaptureSlots;
    const int priorHistorySlot = c.historySlot.load(std::memory_order_acquire);
    const UINT frame = d.swapchain->GetCurrentBackBufferIndex();
    const auto sourceDesc = d.sharedTextures[sourceSlot]->GetDesc();
    const auto backDesc = d.backBuffers[frame]->GetDesc();
    const bool parallelCopy = gDLSS.HasHardwareMotion() && syntheticOffsetPixels == 0.0f &&
        (sourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0 &&
        sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        backDesc.Dimension == sourceDesc.Dimension && sourceDesc.Width == backDesc.Width &&
        sourceDesc.Height == backDesc.Height && sourceDesc.Format == backDesc.Format &&
        sourceDesc.MipLevels == 1 && backDesc.MipLevels == 1 &&
        sourceDesc.DepthOrArraySize == 1 && backDesc.DepthOrArraySize == 1 &&
        sourceDesc.SampleDesc.Count == 1 && backDesc.SampleDesc.Count == 1;
    if (parallelCopy) {
        // The early copy uses a dedicated allocator protected by the final
        // direct-queue fence, which follows copy AND finalization.
        const UINT transferIndex = d.nextAllocator % kAllocatorCount;
        const UINT64 done = d.allocatorFenceValues[transferIndex];
        if (done && d.frameFence->GetCompletedValue() < done) {
            HR(d.frameFence->SetEventOnCompletion(done, d.frameEvent), "Transfer allocator fence");
            if (WaitForSingleObject(d.frameEvent, 3000) != WAIT_OBJECT_0) return false;
        }
        HR(d.transferAllocators[transferIndex]->Reset(), "Reset transfer allocator");
        HR(d.transferCopyList->Reset(d.transferAllocators[transferIndex].Get(), nullptr), "Reset transfer copy");
        Transition(d.transferCopyList.Get(), d.backBuffers[frame].Get(),
                   D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        d.transferCopyList->CopyResource(d.backBuffers[frame].Get(), d.sharedTextures[sourceSlot].Get());
        Transition(d.transferCopyList.Get(), d.backBuffers[frame].Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        HR(d.transferCopyList->Close(), "Close transfer copy");
        HR(d.queue->Wait(d.captureFence.Get(), copyReady), "Early copy capture wait");
        // Finish optical-flow source access first. Simultaneous-access sources
        // promote to COPY_SOURCE / NON_PIXEL on each reader and decay to COMMON.
        if (!gDLSS.WaitMotion(d.queue.Get())) return false;
        ID3D12CommandList* copy[] = {d.transferCopyList.Get()};
        d.queue->ExecuteCommandLists(1, copy);
        ++d.parallelCopyFrames;
    } else {
        ++d.rasterFrames;
    }

    // V23 stage 1: record and submit motion/depth preprocessing on a dedicated
    // compute queue. This queue waits directly on capture + NVOFA fences. The
    // presenter queue sees only one final preprocess-fence dependency.
    const UINT preprocessAllocatorIndex = d.nextPreprocessAllocator++ % kAllocatorCount;
    const UINT64 preprocessAllocatorDone = d.preprocessFenceValues[preprocessAllocatorIndex];
    if (preprocessAllocatorDone && d.preprocessFence->GetCompletedValue() < preprocessAllocatorDone) {
        LARGE_INTEGER waitStart{}, waitEnd{}; QueryPerformanceCounter(&waitStart);
        HRESULT hr = d.preprocessFence->SetEventOnCompletion(preprocessAllocatorDone, d.frameEvent);
        if (FAILED(hr)) return false;
        if (WaitForSingleObject(d.frameEvent, 3000) != WAIT_OBJECT_0) return false;
        QueryPerformanceCounter(&waitEnd);
        if (d.qpcFrequency.QuadPart > 0) {
            const double waitMs = 1000.0 * double(waitEnd.QuadPart - waitStart.QuadPart) / double(d.qpcFrequency.QuadPart);
            ++d.preprocessAllocatorWaitCount;
            d.preprocessAllocatorWaitSumMs += waitMs;
            d.preprocessAllocatorWaitMaxMs = std::max(d.preprocessAllocatorWaitMaxMs, waitMs);
        }
    }
    ConsumePreprocessProfile(d, preprocessAllocatorIndex);

    HRESULT hr = d.preprocessAllocators[preprocessAllocatorIndex]->Reset();
    if (FAILED(hr)) return false;
    hr = d.preprocessList->Reset(d.preprocessAllocators[preprocessAllocatorIndex].Get(), nullptr);
    if (FAILED(hr)) return false;

    // DLSS-G input-set reuse must be synchronized on the queue that writes those
    // inputs, not merely on the final presenter queue.
    gDLSS.BeforeFrame(d.preprocessQueue.Get());

    const UINT preprocessQueryBase = preprocessAllocatorIndex * kPreprocessQueriesPerFrame;
    d.preprocessList->EndQuery(d.preprocessQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, preprocessQueryBase + 0);
    gDLSS.RecordPreprocess(d.preprocessList.Get(), d.sharedTextures[sourceSlot].Get(), sourceSlot, parallelCopy);
    d.preprocessList->EndQuery(d.preprocessQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, preprocessQueryBase + 1);
    d.preprocessList->ResolveQueryData(d.preprocessQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       preprocessQueryBase, kPreprocessQueriesPerFrame,
                                       d.preprocessQueryReadback.Get(),
                                       UINT64(preprocessQueryBase) * sizeof(UINT64));
    hr = d.preprocessList->Close();
    if (FAILED(hr)) return false;

    ++d.dependencySamples;
    if (d.captureFence->GetCompletedValue() < copyReady) ++d.captureFencePending;
    if (gDLSS.MotionWouldBlockNow()) ++d.nvofFencePending;

    hr = d.preprocessQueue->Wait(d.captureFence.Get(), copyReady);
    if (FAILED(hr)) return false;
    if (!gDLSS.WaitMotion(d.preprocessQueue.Get())) return false;

    ID3D12CommandList* preprocessLists[] = { d.preprocessList.Get() };
    d.preprocessQueue->ExecuteCommandLists(1, preprocessLists);
    const UINT64 preprocessDone = d.nextPreprocessFenceValue++;
    hr = d.preprocessQueue->Signal(d.preprocessFence.Get(), preprocessDone);
    if (FAILED(hr)) return false;
    d.preprocessFenceValues[preprocessAllocatorIndex] = preprocessDone;
    d.preprocessProfileValid[preprocessAllocatorIndex] = true;

    // NVOFA flow outputs are no longer needed once the compute resolve completes.
    // Let the triple-flow ring recycle them at preprocess completion instead of
    // holding them until the later Present path completes.
    gDLSS.MarkMotionConsumed(d.preprocessFence.Get(), preprocessDone);

    // V23 stage 2: record the final presenter list while the compute queue can
    // still be working. The direct queue will wait on exactly one preprocess fence.
    const UINT allocatorIndex = d.nextAllocator++ % kAllocatorCount;
    const UINT64 allocatorDone = d.allocatorFenceValues[allocatorIndex];
    if (allocatorDone && d.frameFence->GetCompletedValue() < allocatorDone) {
        LARGE_INTEGER waitStart{}, waitEnd{}; QueryPerformanceCounter(&waitStart);
        hr = d.frameFence->SetEventOnCompletion(allocatorDone, d.frameEvent);
        if (FAILED(hr)) return false;
        if (WaitForSingleObject(d.frameEvent, 3000) != WAIT_OBJECT_0) return false;
        QueryPerformanceCounter(&waitEnd);
        if (d.qpcFrequency.QuadPart > 0) {
            const double waitMs = 1000.0 * double(waitEnd.QuadPart - waitStart.QuadPart) / double(d.qpcFrequency.QuadPart);
            ++d.allocatorWaitCount;
            d.allocatorWaitSumMs += waitMs;
            d.allocatorWaitMaxMs = std::max(d.allocatorWaitMaxMs, waitMs);
        }
    }
    ConsumeGpuProfile(d, allocatorIndex);

    hr = d.allocators[allocatorIndex]->Reset();
    if (FAILED(hr)) return false;
    hr = d.list->Reset(d.allocators[allocatorIndex].Get(), d.pipeline.Get());
    if (FAILED(hr)) return false;

    const UINT queryBase = allocatorIndex * kGpuQueriesPerFrame;
    d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 0);
    gDLSS.TagInputs(d.list.Get());
    d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);

    D3D12_RESOURCE_BARRIER backBarrier{};
    backBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBarrier.Transition.pResource = d.backBuffers[frame].Get();
    backBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!parallelCopy) {
    // Compute leaves the live source in NON_PIXEL_SHADER_RESOURCE. The direct
    // queue owns the final state transition after its preprocess-fence wait.
    D3D12_RESOURCE_BARRIER sourceToPixel{};
    sourceToPixel.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sourceToPixel.Transition.pResource = d.sharedTextures[sourceSlot].Get();
    sourceToPixel.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sourceToPixel.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    sourceToPixel.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.list->ResourceBarrier(1, &sourceToPixel);

    d.list->SetPipelineState(d.pipeline.Get());
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    backBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.list->ResourceBarrier(1, &backBarrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame) * d.rtvStride;
    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    d.list->SetGraphicsRootSignature(d.rootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { d.srvHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    auto colorTable = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
    colorTable.ptr += SIZE_T(sourceSlot) * d.srvStride;
    d.list->SetGraphicsRootDescriptorTable(0, colorTable);
    float jitterX = syntheticOffsetPixels / static_cast<float>(std::max(1u, d.sourceWidth));
    d.list->SetGraphicsRoot32BitConstants(1, 1, &jitterX, 0);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 2);

    d.list->DrawInstanced(3, 1, 0, 0);
    d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 3);

    } else {
        // No draw on the fast path. Early copy timings are deliberately not
        // reported as raster timings in the existing final-list profile.
        d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 2);
        d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 3);
    }
    D3D12_RESOURCE_BARRIER sourceBarrier{};
    sourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sourceBarrier.Transition.pResource = d.sharedTextures[sourceSlot].Get();
    sourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    sourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    sourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!parallelCopy) d.list->ResourceBarrier(1, &sourceBarrier);

    gDLSS.TagBackbuffer(d.list.Get(), d.backBuffers[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d.list->ResourceBarrier(1, &backBarrier);
    d.list->EndQuery(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 4);
    d.list->ResolveQueryData(d.gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase,
                             kGpuQueriesPerFrame, d.gpuQueryReadback.Get(),
                             UINT64(queryBase) * sizeof(UINT64));

    hr = d.list->Close();
    if (FAILED(hr)) return false;

    if (d.preprocessFence->GetCompletedValue() < preprocessDone) ++d.preprocessFencePending;
    hr = d.queue->Wait(d.preprocessFence.Get(), preprocessDone);
    if (FAILED(hr)) return false;

    ID3D12CommandList* lists[] = { d.list.Get() };
    gDLSS.Marker(sl::PCLMarker::eRenderSubmitStart);
    d.queue->ExecuteCommandLists(1, lists);
    gDLSS.Marker(sl::PCLMarker::eRenderSubmitEnd);

    UINT64 renderDone = c.nextReleaseValue++;
    hr = d.queue->Signal(d.releaseFence.Get(), renderDone);
    if (FAILED(hr)) return false;

    const UINT64 allocatorFenceValue = d.nextAllocatorFenceValue++;
    hr = d.queue->Signal(d.frameFence.Get(), allocatorFenceValue);
    if (FAILED(hr)) return false;
    d.allocatorFenceValues[allocatorIndex] = allocatorFenceValue;
    d.gpuProfileValid[allocatorIndex] = true;

    // History remains the last actually presented capture, independent of any
    // producer-side speculative OFA work.
    const int nextHistorySlot = static_cast<int>(sourceSlot);
    if (priorHistorySlot >= 0 && priorHistorySlot < static_cast<int>(kCaptureSlots) &&
        priorHistorySlot != nextHistorySlot) {
        c.lastReleaseDone[priorHistorySlot].store(renderDone, std::memory_order_release);
        c.mailboxes[priorHistorySlot].state.store(CaptureState::kFree, std::memory_order_release);
    }
    c.mailboxes[sourceSlot].state.store(CaptureState::kHistory, std::memory_order_release);
    c.lastHistoryDone.store(renderDone, std::memory_order_release);
    c.historySlot.store(nextHistorySlot, std::memory_order_release);

    const UINT sync = d.allowTearing ? 0 : 1;
    const UINT flags = d.allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    gDLSS.Marker(sl::PCLMarker::ePresentStart);

    LARGE_INTEGER atPresent{}; QueryPerformanceCounter(&atPresent);
    if (d.qpcFrequency.QuadPart > 0 && d.captureTimestamp.QuadPart > 0 &&
        atPresent.QuadPart >= d.captureTimestamp.QuadPart) {
        const double ageMs = 1000.0 * double(atPresent.QuadPart - d.captureTimestamp.QuadPart) /
                             double(d.qpcFrequency.QuadPart);
        d.ageSumMs += ageMs;
        d.ageMaxMs = std::max(d.ageMaxMs, ageMs);
        ++d.ageSamples;
    }

    LARGE_INTEGER presentEnd{};
    hr = d.swapchain->Present(sync, flags);
    QueryPerformanceCounter(&presentEnd);
    if (d.qpcFrequency.QuadPart > 0 && presentEnd.QuadPart >= atPresent.QuadPart) {
        const double presentMs = 1000.0 * double(presentEnd.QuadPart - atPresent.QuadPart) /
                                 double(d.qpcFrequency.QuadPart);
        d.presentSumMs += presentMs;
        d.presentMaxMs = std::max(d.presentMaxMs, presentMs);
        ++d.presentSamples;
    }
    gDLSS.Marker(sl::PCLMarker::ePresentEnd);
    if (FAILED(hr)) return false;
    gDLSS.AfterPresent();
    return true;
}

void App::DestroyDx12(Dx12State& d) {
    if (d.preprocessQueue && d.preprocessFence) {
        UINT64 value = d.nextPreprocessFenceValue++;
        if (SUCCEEDED(d.preprocessQueue->Signal(d.preprocessFence.Get(), value)) && d.frameEvent) {
            d.preprocessFence->SetEventOnCompletion(value, d.frameEvent);
            WaitForSingleObject(d.frameEvent, 3000);
        }
    }
    if (d.queue && d.frameFence) {
        UINT64 value = d.nextAllocatorFenceValue++;
        if (SUCCEEDED(d.queue->Signal(d.frameFence.Get(), value)) && d.frameEvent) {
            d.frameFence->SetEventOnCompletion(value, d.frameEvent);
            WaitForSingleObject(d.frameEvent, 3000);
        }
    }
    if (d.gpuQueryReadback && d.gpuQueryData) {
        D3D12_RANGE noWrite{0,0}; d.gpuQueryReadback->Unmap(0, &noWrite); d.gpuQueryData=nullptr;
    }
    if (d.preprocessQueryReadback && d.preprocessQueryData) {
        D3D12_RANGE noWrite{0,0}; d.preprocessQueryReadback->Unmap(0, &noWrite); d.preprocessQueryData=nullptr;
    }
    if (d.frameEvent) { CloseHandle(d.frameEvent); d.frameEvent = nullptr; }
}

void App::Worker(HWND target, HWND outputWindow) {
    CaptureState c;
    Dx12State d;
    HANDLE mailboxEvent = nullptr;
    std::thread captureProducer;
    std::atomic<bool> captureFailed{false};
    std::string captureFailureMessage;
    std::atomic<uint64_t> producerCaptured{0};
    std::atomic<uint64_t> producerDropped{0};

    auto releaseLiveHistory = [&]() {
        const int held = c.historySlot.exchange(-1,std::memory_order_acq_rel);
        if (held >= 0 && held < static_cast<int>(kCaptureSlots)) {
            const UINT64 done = c.lastHistoryDone.load(std::memory_order_acquire);
            if (done) c.lastReleaseDone[held].store(done,std::memory_order_release);
            LONG expected = CaptureState::kHistory;
            c.mailboxes[held].state.compare_exchange_strong(expected,CaptureState::kFree,
                std::memory_order_acq_rel,std::memory_order_acquire);
        }
        for (UINT i=0;i<kCaptureSlots;++i) {
            LONG expected=CaptureState::kReady;
            if(c.mailboxes[i].state.compare_exchange_strong(expected,CaptureState::kFree,
                std::memory_order_acq_rel,std::memory_order_acquire)) {
                gDLSS.DiscardPreparedMotion(c.mailboxes[i].sequence.load(std::memory_order_acquire));
                producerDropped.fetch_add(1,std::memory_order_relaxed);
            }
        }
        gDLSS.ResetHistory();
    };

    try {
        gLog.Write(L"V23 DuplicateOutput1 + async-preprocess build. Backend: " + std::wstring(useWgc_ ? L"WGC" : L"DXGI DuplicateOutput1") +
                   L" / DX12. Selected multiplier: " + std::to_wstring(multiplier_) + L"x. NVOFA grid request: " +
                   std::to_wstring(nvofGrid_) + L"x" + std::to_wstring(nvofGrid_) + L" FAST.");
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
        if(!InitDx12(outputWindow,c,width,height,lowLatency_,d)) throw std::runtime_error("DX12 initialization failed. See log.");
        RECT output{};GetClientRect(outputWindow,&output);
        gDLSS.Init(d.device.Get(),c.adapter.Get(),d.sharedTextures[0].Get(),d.sharedTextures[1].Get(),d.sharedTextures[2].Get(),
                   output.right,output.bottom,false,true,multiplier_,true,nvofGrid_,reflexTiming_,nvofCost_);
        if(!CreateSwapchain(outputWindow,lowLatency_,d,false)) throw std::runtime_error("Swapchain creation failed.");
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        auto colorCpu=d.srvHeap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<kCaptureSlots;++i){
            d.device->CreateShaderResourceView(d.sharedTextures[i].Get(),&view,colorCpu); colorCpu.ptr+=d.srvStride;
        }

        mailboxEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!mailboxEvent) throw std::runtime_error("Could not create V23 capture-mailbox event.");

        // Dedicated capture producer. It owns all post-initialization D3D11/DXGI/WGC
        // capture work. The render worker only consumes already-published mailboxes.
        captureProducer=std::thread([&,target,width,height]() {
            WgcCaptureBackend wgc;
            try {
                if(useWgc_) {
                    std::wstring wgcError;
                    if(!wgc.Init(target,c.device.Get(),d.qpcFrequency,wgcError))
                        throw std::runtime_error(std::string("WGC initialization failed: ")+
                            std::string(wgcError.begin(),wgcError.end()));
                    gLog.Write(L"V23 producer active: Windows Graphics Capture, three-slot latest-frame mailbox.");
                } else {
                    gLog.Write(c.duplicateOutput1
                        ? L"V23 producer active: DXGI DuplicateOutput1, three-slot latest-frame mailbox."
                        : L"V23 producer active: DXGI DuplicateOutput compatibility fallback, three-slot latest-frame mailbox.");
                }
                gLog.Write(L"V25 capture ordering: Copy -> capture-fence Signal -> immediate D3D11 Flush -> OFA submit -> release capture frame -> publish mailbox.");
                uint64_t sequence=0;
                while(!stop_&&IsWindow(target)&&IsWindow(outputWindow)) {
                    HWND foreground=GetForegroundWindow();
                    const bool active=foreground==target||foreground==outputWindow||GetAncestor(foreground,GA_ROOT)==target;
                    if(paused_||!active||IsIconic(target)) { Sleep(2); continue; }

                    DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
                    ComPtr<ID3D11Texture2D> desktop; LARGE_INTEGER sourceTimestamp{};
                    bool wgcFrameHeld=false;
                    struct FrameLease {IDXGIOutputDuplication* d{};~FrameLease(){if(d)d->ReleaseFrame();}} lease{};
                    if(!useWgc_) {
                        HRESULT hr=c.duplication->AcquireNextFrame(16,&info,&resource);
                        if(hr==DXGI_ERROR_WAIT_TIMEOUT) continue;
                        HR(hr,"Acquire captured frame");
                        lease.d=c.duplication.Get();
                        if(!info.LastPresentTime.QuadPart||!info.AccumulatedFrames) continue;
                        sourceTimestamp=info.LastPresentTime;
                        HR(resource.As(&desktop),"Captured texture");
                    } else {
                        if(!wgc.WaitForFrame(workerControlEvent_)) {
                            if(stop_) break;
                            continue;
                        }
                        bool sizeChanged=false; std::wstring wgcError;
                        if(!wgc.TryGetNextFrame(desktop,sourceTimestamp,sizeChanged,wgcError))
                            throw std::runtime_error(std::string("WGC frame failure: ")+
                                std::string(wgcError.begin(),wgcError.end()));
                        if(sizeChanged||!desktop) continue;
                        wgcFrameHeld=true;
                    }

                    HWND currentForeground=GetForegroundWindow();
                    if(stop_||paused_||IsIconic(target)||
                       (currentForeground!=target&&currentForeground!=outputWindow&&GetAncestor(currentForeground,GA_ROOT)!=target)) {
                        if(wgcFrameHeld) wgc.ReleaseFrame();
                        continue;
                    }

                    int sourceSlot=-1;
                    // Prefer an unused slot. If render falls behind, overwrite an
                    // unclaimed READY slot rather than queueing stale captured frames.
                    for(UINT i=0;i<kCaptureSlots&&sourceSlot<0;++i) {
                        if(!gDLSS.CanReuseMotionSource(i)) continue;
                        LONG expected=CaptureState::kFree;
                        if(c.mailboxes[i].state.compare_exchange_strong(expected,CaptureState::kWriting,
                            std::memory_order_acq_rel,std::memory_order_acquire)) sourceSlot=static_cast<int>(i);
                    }
                    if(sourceSlot<0) {
                        uint64_t oldest=~uint64_t{0}; int candidate=-1;
                        for(UINT i=0;i<kCaptureSlots;++i) {
                            if(!gDLSS.CanReuseMotionSource(i)) continue;
                            if(c.mailboxes[i].state.load(std::memory_order_acquire)==CaptureState::kReady &&
                               c.mailboxes[i].sequence.load(std::memory_order_acquire)<oldest) {
                                oldest=c.mailboxes[i].sequence.load(std::memory_order_acquire); candidate=static_cast<int>(i);
                            }
                        }
                        if(candidate>=0) {
                            const uint64_t staleSequence=c.mailboxes[candidate].sequence.load(std::memory_order_acquire);
                            LONG expected=CaptureState::kReady;
                            if(c.mailboxes[candidate].state.compare_exchange_strong(expected,CaptureState::kWriting,
                                std::memory_order_acq_rel,std::memory_order_acquire)) {
                                gDLSS.DiscardPreparedMotion(staleSequence);
                                sourceSlot=candidate;
                                producerDropped.fetch_add(1,std::memory_order_relaxed);
                            }
                        }
                    }
                    if(sourceSlot<0) {
                        // History + currently consumed frame can occupy two slots;
                        // if the third was claimed in the same instant, drop this
                        // desktop update instead of waiting and increasing latency.
                        producerDropped.fetch_add(1,std::memory_order_relaxed);
                        if(wgcFrameHeld) wgc.ReleaseFrame();
                        continue;
                    }

                    const UINT slot=static_cast<UINT>(sourceSlot);
                    const UINT64 releaseValue=c.lastReleaseDone[slot].load(std::memory_order_acquire);
                    if(releaseValue)
                        HR(c.context4->Wait(c.releaseFence.Get(),releaseValue),"Capture-slot release synchronization");
                    D3D11_TEXTURE2D_DESC sourceDesc{};desktop->GetDesc(&sourceDesc);
                    D3D11_BOX box{};
                    if(!useWgc_) {
                        RECT out=c.outputDesc.DesktopCoordinates;
                        box.left=c.sourceRect.left-out.left;box.top=c.sourceRect.top-out.top;
                    } else {
                        RECT wr{},client{};GetWindowRect(target,&wr);
                        if(!GetClientScreenRect(target,client))client=c.sourceRect;
                        const UINT clientLeft=(UINT)std::max<LONG>(0,client.left-wr.left);
                        const UINT clientTop=(UINT)std::max<LONG>(0,client.top-wr.top);
                        if(sourceDesc.Width==width&&sourceDesc.Height==height){box.left=0;box.top=0;}
                        else if(clientLeft+width<=sourceDesc.Width&&clientTop+height<=sourceDesc.Height){box.left=clientLeft;box.top=clientTop;}
                        else if(width<=sourceDesc.Width&&height<=sourceDesc.Height){
                            box.left=std::min<UINT>(clientLeft,sourceDesc.Width-width);
                            box.top=std::min<UINT>(clientTop,sourceDesc.Height-height);
                        } else {
                            c.mailboxes[slot].state.store(CaptureState::kFree,std::memory_order_release);
                            wgc.ReleaseFrame();
                            throw std::runtime_error("WGC source became smaller than the fixed input; restart after changing resolution.");
                        }
                    }
                    box.right=box.left+width;box.bottom=box.top+height;box.back=1;
                    c.context->CopySubresourceRegion(c.sharedTextures[slot].Get(),0,0,0,0,desktop.Get(),0,&box);
                    const UINT64 ready=c.nextCaptureValue++;
                    HR(c.context4->Signal(c.captureFence.Get(),ready),"Capture signal");
                    // Submit copy + fence immediately, before CPU-side OFA work.
                    c.context->Flush();
                    const uint64_t publishedSequence=++sequence;
                    c.mailboxes[slot].readyValue=ready;
                    c.mailboxes[slot].sourceTimestamp=sourceTimestamp;
                    c.mailboxes[slot].sequence.store(publishedSequence,std::memory_order_release);
                    // OFA waits on the already-submitted capture fence.
                    const int producerHistory=c.historySlot.load(std::memory_order_acquire);
                    const UINT64 producerHistoryDone=c.lastHistoryDone.load(std::memory_order_acquire);
                    if(producerHistory>=0 && producerHistory<static_cast<int>(kCaptureSlots) && producerHistory!=sourceSlot)
                        gDLSS.ProducerPrepareMotion(slot,static_cast<UINT>(producerHistory),publishedSequence,
                            d.captureFence.Get(),ready,d.releaseFence.Get(),producerHistoryDone);

                    if(wgcFrameHeld){wgc.ReleaseFrame();wgcFrameHeld=false;}
                    if(lease.d){HR(lease.d->ReleaseFrame(),"Release captured frame");lease.d=nullptr;}
                    c.mailboxes[slot].state.store(CaptureState::kReady,std::memory_order_release);
                    producerCaptured.fetch_add(1,std::memory_order_relaxed);
                    SetEvent(mailboxEvent);
                }
            } catch(const std::exception& e) {
                captureFailureMessage=e.what();
                captureFailed.store(true,std::memory_order_release);
                gLog.Write(L"V23 capture producer stopped: "+std::wstring(captureFailureMessage.begin(),captureFailureMessage.end()));
            }
            wgc.Shutdown();
            if(mailboxEvent)SetEvent(mailboxEvent);
        });

        auto claimLatestMailbox=[&]()->int {
            for(int attempt=0;attempt<2;++attempt) {
                int best=-1;uint64_t bestSequence=0;
                for(UINT i=0;i<kCaptureSlots;++i) {
                    if(c.mailboxes[i].state.load(std::memory_order_acquire)==CaptureState::kReady) {
                        const uint64_t seq=c.mailboxes[i].sequence.load(std::memory_order_acquire);
                        if(best<0||seq>bestSequence){best=static_cast<int>(i);bestSequence=seq;}
                    }
                }
                if(best<0)return -1;
                LONG expected=CaptureState::kReady;
                if(!c.mailboxes[best].state.compare_exchange_strong(expected,CaptureState::kConsuming,
                    std::memory_order_acq_rel,std::memory_order_acquire)) continue;
                // One bounded freshness retry: if a newer frame arrived while we
                // claimed this slot, discard the older claim and take the newer one.
                bool newer=false;
                for(UINT i=0;i<kCaptureSlots;++i) if(static_cast<int>(i)!=best &&
                    c.mailboxes[i].state.load(std::memory_order_acquire)==CaptureState::kReady &&
                    c.mailboxes[i].sequence.load(std::memory_order_acquire)>bestSequence) { newer=true;break; }
                if(newer&&attempt==0) {
                    gDLSS.DiscardPreparedMotion(bestSequence);
                    c.mailboxes[best].state.store(CaptureState::kFree,std::memory_order_release);
                    producerDropped.fetch_add(1,std::memory_order_relaxed);
                    continue;
                }
                // All other READY frames are stale by definition once the newest
                // mailbox is claimed. Free them immediately; never build a queue.
                for(UINT i=0;i<kCaptureSlots;++i) if(static_cast<int>(i)!=best) {
                    LONG stale=CaptureState::kReady;
                    if(c.mailboxes[i].state.compare_exchange_strong(stale,CaptureState::kFree,
                        std::memory_order_acq_rel,std::memory_order_acquire)) {
                        gDLSS.DiscardPreparedMotion(c.mailboxes[i].sequence.load(std::memory_order_acquire));
                        producerDropped.fetch_add(1,std::memory_order_relaxed);
                    }
                }
                return best;
            }
            return -1;
        };

        auto lastReport=std::chrono::steady_clock::now();auto lastFrame=lastReport;
        uint64_t lastCapturedReport=0,lastDroppedReport=0;
        unsigned presented=0;bool visible=true;
        while(!stop_&&IsWindow(target)&&IsWindow(outputWindow)) {
            HWND foreground=GetForegroundWindow();
            bool active=foreground==target||foreground==outputWindow||GetAncestor(foreground,GA_ROOT)==target;
            if(paused_||!active||IsIconic(target)) {
                focus_compat::Enable(false);
                gDLSS.Suspend(true);
                if(visible){PostMessageW(outputWindow,WM_APP_OUTPUT_VISIBILITY,FALSE,0);visible=false;}
                releaseLiveHistory();Sleep(5);continue;
            }
            focus_compat::Enable(true);gDLSS.Suspend(false);
            if(!visible){PostMessageW(outputWindow,WM_APP_OUTPUT_VISIBILITY,TRUE,0);visible=true;}
            if(recreateSwapchain_.exchange(false))throw std::runtime_error("Output resized: press Start again to rebuild DLSS resources.");
            if(stop_||paused_)continue;

            // In V17 this mode sleeps before waiting for a published capture, not
            // on the capture producer thread. After-arrival mode sleeps only once
            // the freshest mailbox has been claimed.
            if(reflexTiming_==1)gDLSS.PrepareFrame(true);
            int sourceSlot=-1;
            while(sourceSlot<0&&!stop_&&!paused_) {
                if(captureFailed.load(std::memory_order_acquire))
                    throw std::runtime_error("Capture producer failed: "+captureFailureMessage);
                sourceSlot=claimLatestMailbox();
                if(sourceSlot>=0)break;
                WaitForSingleObject(mailboxEvent,16);
            }
            if(stop_||paused_)continue;
            if(sourceSlot<0)continue;

            HWND currentForeground=GetForegroundWindow();
            if(IsIconic(target)||(currentForeground!=target&&currentForeground!=outputWindow&&GetAncestor(currentForeground,GA_ROOT)!=target)) {
                gDLSS.DiscardPreparedMotion(c.mailboxes[sourceSlot].sequence.load(std::memory_order_acquire));
                c.mailboxes[sourceSlot].state.store(CaptureState::kFree,std::memory_order_release);
                continue;
            }
            const UINT64 ready=c.mailboxes[sourceSlot].readyValue;
            const uint64_t frameSequence=c.mailboxes[sourceSlot].sequence.load(std::memory_order_acquire);
            const int expectedHistory=c.historySlot.load(std::memory_order_acquire);
            const UINT64 priorHistoryDone=c.lastHistoryDone.load(std::memory_order_acquire);
            // V23: consume the producer-prepared flow only if it was generated
            // against the mailbox that is still the previous actually-presented
            // frame. If render advanced history after producer submission, discard
            // the mismatched flow and fall back to a correct render-side submit.
            bool prepared=gDLSS.SelectPreparedMotion(frameSequence,expectedHistory);
            if(!prepared && expectedHistory>=0 && expectedHistory<static_cast<int>(kCaptureSlots) && expectedHistory!=sourceSlot) {
                gDLSS.DiscardPreparedMotion(frameSequence);
                if(gDLSS.PrepareMotion(static_cast<UINT>(sourceSlot),static_cast<UINT>(expectedHistory),frameSequence,
                                       d.captureFence.Get(),ready,d.releaseFence.Get(),priorHistoryDone))
                    gDLSS.SelectPreparedMotion(frameSequence,expectedHistory);
            }
            if(reflexTiming_!=1)gDLSS.PrepareFrame(true);
            d.captureTimestamp=c.mailboxes[sourceSlot].sourceTimestamp;
            auto now=std::chrono::steady_clock::now();
            if(now-lastFrame>std::chrono::milliseconds(100))releaseLiveHistory();
            lastFrame=now;
            if(!RenderFrame(c,d,static_cast<UINT>(sourceSlot),ready))
                throw std::runtime_error("Present failed. Check NVIDIA Streamline logs.");
            ++presented;

            now=std::chrono::steady_clock::now();double elapsed=std::chrono::duration<double>(now-lastReport).count();
            if(elapsed>=1.0) {
                gLog.Write(focus_compat::Report());
                gLog.Write(L"V25 image path: parallel copy=" + std::to_wstring(d.parallelCopyFrames) +
                    L", raster fallback=" + std::to_wstring(d.rasterFrames) +
                    L". GPU final-list profile excludes the early copy.");
                d.parallelCopyFrames = d.rasterFrames = 0;
                if(d.ageSamples) {
                    wchar_t age[240]{};
                    swprintf_s(age,L"Desktop-update age at Present call (CPU): avg %.2f ms, max %.2f ms. Not input-to-screen latency.",
                        d.ageSumMs/d.ageSamples,d.ageMaxMs);gLog.Write(age);
                }
                if(d.allocatorWaitCount) {
                    wchar_t alloc[240]{};
                    swprintf_s(alloc,L"V23 allocator recycle CPU waits: %llu, avg %.3f ms, max %.3f ms.",
                        static_cast<unsigned long long>(d.allocatorWaitCount),
                        d.allocatorWaitSumMs/d.allocatorWaitCount,d.allocatorWaitMaxMs);gLog.Write(alloc);
                } else gLog.Write(L"V23 allocator recycle CPU waits: 0 in report interval.");
                if(d.preprocessAllocatorWaitCount) {
                    wchar_t alloc[260]{};
                    swprintf_s(alloc,L"V23 preprocess allocator CPU waits: %llu, avg %.3f ms, max %.3f ms.",
                        static_cast<unsigned long long>(d.preprocessAllocatorWaitCount),
                        d.preprocessAllocatorWaitSumMs/d.preprocessAllocatorWaitCount,d.preprocessAllocatorWaitMaxMs);gLog.Write(alloc);
                } else gLog.Write(L"V23 preprocess allocator CPU waits: 0 in report interval.");
                d.ageSumMs=d.ageMaxMs=0;d.ageSamples=0;
                d.allocatorWaitCount=0;d.allocatorWaitSumMs=d.allocatorWaitMaxMs=0;
                d.preprocessAllocatorWaitCount=0;d.preprocessAllocatorWaitSumMs=d.preprocessAllocatorWaitMaxMs=0;
                const uint64_t fgRecycleWaits=gDLSS.ConsumeInputRecycleWaits();
                gLog.Write(L"V23 DLSS-G recycled-input GPU waits queued: "+std::to_wstring(fgRecycleWaits)+L" in report interval.");
                if(d.preprocessProfileSamples) {
                    wchar_t pre[280]{};
                    swprintf_s(pre,L"V23 async preprocess GPU: avg %.3f ms / max %.3f ms (%llu samples).",
                        d.preprocessGpuSumMs/d.preprocessProfileSamples,d.preprocessGpuMaxMs,
                        static_cast<unsigned long long>(d.preprocessProfileSamples)); gLog.Write(pre);
                }
                if(d.gpuProfileSamples) {
                    wchar_t gpu[380]{};
                    swprintf_s(gpu,L"V23 presenter GPU list: input-tag %.3f ms, setup %.3f ms, draw %.3f ms, finish/tag %.3f ms, total %.3f ms avg / %.3f ms max (%llu samples).",
                        d.gpuTagSumMs/d.gpuProfileSamples,d.gpuSetupSumMs/d.gpuProfileSamples,
                        d.gpuDrawSumMs/d.gpuProfileSamples,d.gpuFinishSumMs/d.gpuProfileSamples,
                        d.gpuTotalSumMs/d.gpuProfileSamples,d.gpuTotalMaxMs,
                        static_cast<unsigned long long>(d.gpuProfileSamples)); gLog.Write(gpu);
                }
                if(d.dependencySamples) {
                    wchar_t deps[420]{};
                    swprintf_s(deps,L"V23 dependency readiness: preprocess submit capture pending %llu/%llu, NVOFA pending %llu/%llu; presenter submit preprocess pending %llu/%llu.",
                        static_cast<unsigned long long>(d.captureFencePending),static_cast<unsigned long long>(d.dependencySamples),
                        static_cast<unsigned long long>(d.nvofFencePending),static_cast<unsigned long long>(d.dependencySamples),
                        static_cast<unsigned long long>(d.preprocessFencePending),static_cast<unsigned long long>(d.dependencySamples)); gLog.Write(deps);
                }
                uint64_t ofaSamples=0; double ofaAvg=0.0,ofaMax=0.0;
                gDLSS.ConsumeNvofTimingStats(ofaSamples,ofaAvg,ofaMax);
                if(ofaSamples) {
                    wchar_t ofa[320]{};
                    swprintf_s(ofa,L"V23 NVOFA submit-to-completion: avg %.3f ms, max %.3f ms (%llu samples), confidence cost=%s.",
                        ofaAvg,ofaMax,static_cast<unsigned long long>(ofaSamples),nvofCost_?L"ON":L"OFF");
                    gLog.Write(ofa);
                }
                uint64_t producerOFA=0,preparedHits=0,fallbackOFA=0,discardedOFA=0,noFlowSlot=0;
                double leadAvg=0.0,leadMax=0.0;
                gDLSS.ConsumeNvofOverlapStats(producerOFA,preparedHits,fallbackOFA,discardedOFA,noFlowSlot,leadAvg,leadMax);
                {
                    wchar_t overlap[420]{};
                    swprintf_s(overlap,L"V23 OFA overlap: producer submits %llu, prepared hits %llu, render fallbacks %llu, discarded %llu, no-free-flow-slot %llu, submit-to-render lead avg %.3f ms / max %.3f ms.",
                        static_cast<unsigned long long>(producerOFA),static_cast<unsigned long long>(preparedHits),
                        static_cast<unsigned long long>(fallbackOFA),static_cast<unsigned long long>(discardedOFA),
                        static_cast<unsigned long long>(noFlowSlot),leadAvg,leadMax);
                    gLog.Write(overlap);
                }
                if(d.presentSamples) {
                    wchar_t present[240]{};
                    swprintf_s(present,L"V23 Present CPU call: avg %.3f ms, max %.3f ms (%llu calls).",
                        d.presentSumMs/d.presentSamples,d.presentMaxMs,static_cast<unsigned long long>(d.presentSamples)); gLog.Write(present);
                }
                uint64_t reflexCalls=0; double reflexAvg=0.0,reflexMax=0.0;
                gDLSS.ConsumeReflexSleepStats(reflexCalls,reflexAvg,reflexMax);
                if(reflexCalls) {
                    wchar_t reflex[240]{};
                    swprintf_s(reflex,L"V23 Reflex sleep CPU: avg %.3f ms, max %.3f ms (%llu calls).",
                        reflexAvg,reflexMax,static_cast<unsigned long long>(reflexCalls)); gLog.Write(reflex);
                }
                d.preprocessProfileSamples=0; d.preprocessGpuSumMs=d.preprocessGpuMaxMs=0;
                d.gpuProfileSamples=0; d.gpuTagSumMs=d.gpuSetupSumMs=d.gpuDrawSumMs=d.gpuFinishSumMs=0; d.gpuTotalSumMs=d.gpuTotalMaxMs=0;
                d.dependencySamples=d.captureFencePending=d.nvofFencePending=d.preprocessFencePending=0;
                d.presentSamples=0; d.presentSumMs=d.presentMaxMs=0;
                const uint64_t capturedNow=producerCaptured.load(std::memory_order_relaxed);
                const uint64_t droppedNow=producerDropped.load(std::memory_order_relaxed);
                const uint64_t capturedDelta=capturedNow-lastCapturedReport;
                const uint64_t droppedDelta=droppedNow-lastDroppedReport;
                lastCapturedReport=capturedNow;lastDroppedReport=droppedNow;
                gLog.Write(L"V23 producer: "+std::to_wstring(capturedDelta)+L" captures, "+
                           std::to_wstring(droppedDelta)+L" stale/dropped updates in report interval.");
                PostStatus(L"Captured/base presents: "+std::to_wstring(unsigned(presented/elapsed))+L" FPS | "+gDLSS.Status());
                presented=0;lastReport=now;
            }
        }
    } catch(const std::exception& e) {
        std::string message=e.what();PostStatus(L"Stopped: "+std::wstring(message.begin(),message.end()));
    }

    // Stop and join the producer before releasing any D3D11/shared resources.
    stop_=true;
    if(workerControlEvent_)SetEvent(workerControlEvent_);
    if(mailboxEvent)SetEvent(mailboxEvent);
    if(captureProducer.joinable())captureProducer.join();
    if(mailboxEvent){CloseHandle(mailboxEvent);mailboxEvent=nullptr;}

    if(d.queue) {
        try {gDLSS.DrainInputs(d.queue.Get());} catch(const std::exception& e) {
            std::string m=e.what();gLog.Write(std::wstring(m.begin(),m.end()));
        }
    }
    focus_compat::Enable(false);
    DestroyDx12(d);
    gDLSS.Release();
    if(!focus_compat::Remove())gLog.Write(L"Could not fully restore local focus import. Close this app before another test.");
    gDLSS.EndRuntime();
    for(auto& handle:c.textureHandles)if(handle)CloseHandle(handle);
    if(c.captureFenceHandle)CloseHandle(c.captureFenceHandle);
    if(c.releaseFenceHandle)CloseHandle(c.releaseFenceHandle);
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
            EnableWindow(GetDlgItem(control_, IDC_REFLEX_TIMING), TRUE);
            EnableWindow(GetDlgItem(control_, IDC_STOP), FALSE); return 0;
        }
        break;
    case WM_HOTKEY:
        if (wp == 1 && running_) ToggleFullscreen();
        if (wp == 2 && running_) { paused_ = !paused_; if(workerControlEvent_) SetEvent(workerControlEvent_); }
        if (wp == 3 && running_) { stop_ = true; if(workerControlEvent_) SetEvent(workerControlEvent_); }
        return 0;
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        SetWindowTextW(status_, text->c_str()); delete text; return 0;
    }
    case WM_APP_STOPPED:
        Stop();
        EnableWindow(GetDlgItem(control_, IDC_MULTIPLIER), TRUE);
        EnableWindow(GetDlgItem(control_, IDC_CAPTURE_BACKEND), TRUE);
            EnableWindow(GetDlgItem(control_, IDC_REFLEX_TIMING), TRUE);
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
        if (wp == VK_ESCAPE) { stop_ = true; if(workerControlEvent_) SetEvent(workerControlEvent_); return 0; }
        if (wp == VK_F8) { paused_ = !paused_; if(workerControlEvent_) SetEvent(workerControlEvent_); return 0; }
        if (wp == VK_F11) { ToggleFullscreen(); return 0; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: stop_ = true; if(workerControlEvent_) SetEvent(workerControlEvent_); if(!running_) DestroyWindow(hwnd); return 0;
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
