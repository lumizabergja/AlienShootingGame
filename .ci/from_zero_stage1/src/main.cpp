#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <sl.h>
#include <sl_helpers.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
std::mutex g_logMutex;
std::ofstream g_log;

std::wstring ExeDir() {
    std::array<wchar_t, 32768> path{};
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), n)).parent_path().wstring();
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), n, nullptr, nullptr);
    return result;
}

void Log(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log.is_open()) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        g_log << '[' << st.wHour << ':' << st.wMinute << ':' << st.wSecond << '.' << st.wMilliseconds << "] " << text << '\n';
        g_log.flush();
    }
    OutputDebugStringA((text + "\n").c_str());
}

std::string Hr(HRESULT hr) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return s.str();
}

void SlLog(sl::LogType type, const char* msg) {
    const char* prefix = type == sl::LogType::eError ? "SL ERROR: " : (type == sl::LogType::eWarn ? "SL WARN: " : "SL: ");
    Log(std::string(prefix) + (msg ? msg : ""));
}

bool SlOk(sl::Result result, const char* where) {
    if (result == sl::Result::eOk) return true;
    Log(std::string(where) + " -> " + sl::getResultAsStr(result));
    return false;
}

class App {
public:
    ~App() { Shutdown(); }

    bool Init(HINSTANCE instance) {
        instance_ = instance;
        exeDir_ = ExeDir();
        logDir_ = (std::filesystem::path(exeDir_) / L"logs").wstring();
        std::filesystem::create_directories(logDir_);
        g_log.open(std::filesystem::path(logDir_) / L"ExternalDLSSG-Zero-Stage1.log", std::ios::out | std::ios::trunc);
        Log("=== Stage 1: clean Streamline/D3D12 foundation ===");

        if (!InitStreamline()) return false;
        if (!CreateWindow()) return false;
        if (!CreateDevice()) return false;
        if (!CreateSwapchain()) return false;

        Log("STAGE1_READY: Streamline initialized first, D3D12 device assigned, swapchain created through interposed DXGI path");
        return true;
    }

    int Run() {
        MSG msg{};
        while (running_) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running_ = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running_) break;
            if (swapchain_) {
                const HRESULT hr = swapchain_->Present(1, 0);
                if (FAILED(hr)) {
                    Log("Present failed: " + Hr(hr));
                    SetWindowTextW(hwnd_, L"External DLSS-G - Stage 1 Present failed; see log");
                    Sleep(50);
                }
            } else {
                Sleep(16);
            }
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        switch (message) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (self) self->running_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    bool InitStreamline() {
        sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        const wchar_t* pluginPaths[] = { exeDir_.c_str() };

        sl::Preferences prefs{};
        prefs.showConsole = false;
        prefs.logLevel = sl::LogLevel::eVerbose;
        prefs.logMessageCallback = &SlLog;
        prefs.pathsToPlugins = pluginPaths;
        prefs.numPathsToPlugins = 1;
        prefs.pathToLogsAndData = logDir_.c_str();
        prefs.featuresToLoad = features;
        prefs.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
        prefs.applicationId = 0;
        prefs.engine = sl::EngineType::eCustom;
        prefs.engineVersion = "1.0.0";
        prefs.projectId = "566b8148-8d0a-4f42-a46e-2dbf2a3d8fd1";
        prefs.renderAPI = sl::RenderAPI::eD3D12;
        prefs.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

        const sl::Result result = slInit(prefs, sl::kSDKVersion);
        if (!SlOk(result, "slInit")) return false;
        slInitialized_ = true;
        Log("ORDER_PROOF: slInit completed before CreateDXGIFactory2 or D3D12CreateDevice");
        return true;
    }

    bool CreateWindow() {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ExternalDLSSGZeroStage1";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log("RegisterClassExW failed");
            return false;
        }

        RECT rect{ 0, 0, 960, 540 };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"External DLSS-G - clean Stage 1",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance_, this);
        if (!hwnd_) {
            Log("CreateWindowExW failed");
            return false;
        }
        return true;
    }

    bool CreateDevice() {
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            Log("CreateDXGIFactory2 failed: " + Hr(hr));
            return false;
        }

        ComPtr<IDXGIAdapter1> fallback;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumHr = factory_->EnumAdapters1(index, &candidate);
            if (enumHr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumHr)) continue;

            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && !fallback) fallback = candidate;

            sl::AdapterInfo info{};
            info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
            info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
            const sl::Result support = slIsFeatureSupported(sl::kFeatureDLSS_G, info);
            Log("Adapter " + std::to_string(index) + ": " + Narrow(desc.Description) + " DLSS-G=" + sl::getResultAsStr(support));
            if (support == sl::Result::eOk) {
                adapter_ = candidate;
                dlssgSupported_ = true;
                break;
            }
        }
        if (!adapter_) adapter_ = fallback;
        if (!adapter_) {
            Log("No hardware DXGI adapter available");
            return false;
        }

        hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) {
            Log("D3D12CreateDevice failed: " + Hr(hr));
            return false;
        }
        if (!SlOk(slSetD3DDevice(device_.Get()), "slSetD3DDevice")) return false;
        Log("D3D12 device created and assigned to Streamline");

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) {
            Log("CreateCommandQueue failed: " + Hr(hr));
            return false;
        }
        return true;
    }

    bool CreateSwapchain() {
        if (dlssgSupported_) {
            const sl::Result loaded = slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
            Log(std::string("slSetFeatureLoaded(DLSS-G,true) -> ") + sl::getResultAsStr(loaded));
            if (loaded != sl::Result::eOk) return false;
        } else {
            Log("DLSS-G unsupported on this adapter; Stage 1 continues only to validate the interposed D3D12/DXGI foundation");
        }

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 960;
        desc.Height = 540;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> baseSwapchain;
        const HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr, nullptr, &baseSwapchain);
        if (FAILED(hr)) {
            Log("CreateSwapChainForHwnd failed: " + Hr(hr));
            return false;
        }
        const HRESULT qi = baseSwapchain.As(&swapchain_);
        if (FAILED(qi)) {
            Log("IDXGISwapChain3 query failed: " + Hr(qi));
            return false;
        }
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        return true;
    }

    void Shutdown() {
        swapchain_.Reset();
        queue_.Reset();
        device_.Reset();
        adapter_.Reset();
        factory_.Reset();
        if (slInitialized_) {
            slShutdown();
            slInitialized_ = false;
        }
        if (g_log.is_open()) {
            Log("Stage 1 shutdown complete");
            g_log.close();
        }
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    bool running_{ true };
    bool slInitialized_{};
    bool dlssgSupported_{};
    std::wstring exeDir_;
    std::wstring logDir_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    App app;
    if (!app.Init(instance)) {
        MessageBoxW(nullptr,
            L"Clean Stage 1 initialization failed. See logs\\ExternalDLSSG-Zero-Stage1.log for the exact failing stage.",
            L"External DLSS-G - Stage 1", MB_OK | MB_ICONERROR);
        return 2;
    }
    return app.Run();
}
