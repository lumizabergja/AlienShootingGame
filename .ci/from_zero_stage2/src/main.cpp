#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

#include <sl.h>
#include <sl_helpers.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
constexpr UINT kWidth = 960;
constexpr UINT kHeight = 540;
constexpr UINT kBackBufferCount = 2;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kMvecFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kUiAlphaFormat = DXGI_FORMAT_R8_UNORM;
constexpr uint32_t kRequestedMultiplier = 2;

std::mutex g_logMutex;
std::ofstream g_log;
std::atomic<long> g_lastPresentError{S_OK};

std::wstring ExeDir() {
    std::array<wchar_t, 32768> path{};
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), n)).parent_path().wstring();
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
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

void OnPresentApiError(const sl::APIError& error) {
    g_lastPresentError.store(error.hres, std::memory_order_relaxed);
}

bool SlOk(sl::Result result, const char* where) {
    if (result == sl::Result::eOk) return true;
    Log(std::string(where) + " -> " + sl::getResultAsStr(result));
    return false;
}

void StoreMatrix(sl::float4x4& dst, FXMMATRIX matrix) {
    XMFLOAT4X4 m{};
    XMStoreFloat4x4(&m, matrix);
    dst.setRow(0, {m._11, m._12, m._13, m._14});
    dst.setRow(1, {m._21, m._22, m._23, m._24});
    dst.setRow(2, {m._31, m._32, m._33, m._34});
    dst.setRow(3, {m._41, m._42, m._43, m._44});
}

D3D12_CPU_DESCRIPTOR_HANDLE OffsetHandle(D3D12_CPU_DESCRIPTOR_HANDLE base, UINT index, UINT stride) {
    base.ptr += static_cast<SIZE_T>(index) * stride;
    return base;
}

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fenceValue{};
};

class App {
public:
    ~App() { Shutdown(); }

    bool Init(HINSTANCE instance) {
        instance_ = instance;
        exeDir_ = ExeDir();
        logDir_ = (std::filesystem::path(exeDir_) / L"logs").wstring();
        std::filesystem::create_directories(logDir_);
        g_log.open(std::filesystem::path(logDir_) / L"ExternalDLSSG-Zero-Stage2.log", std::ios::out | std::ios::trunc);
        Log("=== Stage 2: synthetic DLSS-G + Reflex validation ===");

        if (!InitStreamline()) return false;
        if (!CreateAppWindow()) return false;
        if (!CreateDeviceAndQueue()) return false;
        if (!ConfigureReflex()) return false;
        if (!QueryDLSSG()) return false;
        if (!CreateSwapchain()) return false;
        if (!CreateDescriptorsAndBackbuffers()) return false;
        if (!CreateSyntheticInputs()) return false;
        if (!CreateFrameContexts()) return false;
        if (!ConfigureDLSSG()) return false;

        Log("STAGE2_READY: DLSS-G options, Reflex, synthetic depth/MV/HUDless/UI-alpha and frame-based tagging path initialized");
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
            if (!RenderOneFrame()) {
                SetWindowTextW(hwnd_, L"External DLSS-G - Stage 2 failed; see log");
                Sleep(100);
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
        sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        const wchar_t* pluginPaths[] = {exeDir_.c_str()};

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
        Log("ORDER_PROOF: slInit completed before any DXGI or D3D12 API call");
        return true;
    }

    bool CreateAppWindow() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ExternalDLSSGZeroStage2";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log("RegisterClassExW failed");
            return false;
        }
        RECT rect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"External DLSS-G - clean Stage 2 synthetic validation",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance_, this);
        if (!hwnd_) {
            Log("CreateWindowExW failed");
            return false;
        }
        return true;
    }

    bool CreateDeviceAndQueue() {
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            Log("CreateDXGIFactory2 failed: " + Hr(hr));
            return false;
        }

        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumHr = factory_->EnumAdapters1(index, &candidate);
            if (enumHr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumHr)) continue;
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

            sl::AdapterInfo info{};
            info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
            info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
            const sl::Result support = slIsFeatureSupported(sl::kFeatureDLSS_G, info);
            Log("Adapter " + std::to_string(index) + ": " + Narrow(desc.Description) + " DLSS-G=" + sl::getResultAsStr(support));
            if (support == sl::Result::eOk) {
                adapter_ = candidate;
                break;
            }
        }
        if (!adapter_) {
            Log("No adapter with DLSS-G support was found");
            return false;
        }

        sl::FeatureRequirements requirements{};
        const sl::Result reqResult = slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements);
        if (reqResult == sl::Result::eOk) {
            Log("DLSS-G requirements: maxViewports=" + std::to_string(requirements.maxNumViewports));
        } else {
            Log(std::string("slGetFeatureRequirements(DLSS-G) -> ") + sl::getResultAsStr(reqResult));
        }

        hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) {
            Log("D3D12CreateDevice failed: " + Hr(hr));
            return false;
        }
        if (!SlOk(slSetD3DDevice(device_.Get()), "slSetD3DDevice")) return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) {
            Log("CreateCommandQueue failed: " + Hr(hr));
            return false;
        }
        return true;
    }

    bool ConfigureReflex() {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        reflex.frameLimitUs = 0;
        if (!SlOk(slReflexSetOptions(reflex), "slReflexSetOptions")) return false;
        Log("Reflex configured to Low Latency; no external frame-latency waitable object is used");
        return true;
    }

    bool QueryDLSSG() {
        sl::DLSSGState state{};
        const sl::Result stateResult = slDLSSGGetState(viewport_, state, nullptr);
        if (!SlOk(stateResult, "slDLSSGGetState(initial)")) return false;
        if (state.numFramesToGenerateMax < 1) {
            Log("DLSS-G reports numFramesToGenerateMax < 1");
            return false;
        }
        const uint32_t desiredGenerated = kRequestedMultiplier - 1;
        generatedFrames_ = std::clamp(desiredGenerated, 1u, state.numFramesToGenerateMax);
        Log("DLSS-G max generated frames=" + std::to_string(state.numFramesToGenerateMax) +
            "; Stage2 using multiplier=" + std::to_string(generatedFrames_ + 1));
        return true;
    }

    bool CreateSwapchain() {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Format = kColorFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBackBufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> base;
        const HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr, nullptr, &base);
        if (FAILED(hr)) {
            Log("CreateSwapChainForHwnd failed: " + Hr(hr));
            return false;
        }
        if (FAILED(base.As(&swapchain_))) {
            Log("Query IDXGISwapChain3 failed");
            return false;
        }
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        Log("Flip-discard swapchain created with DLSS-G already loaded by slInit");
        return true;
    }

    bool CreateDescriptorsAndBackbuffers() {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = kBackBufferCount + 3;
        if (FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)))) {
            Log("Create RTV heap failed");
            return false;
        }
        rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.NumDescriptors = 1;
        if (FAILED(device_->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap_)))) {
            Log("Create DSV heap failed");
            return false;
        }

        const auto base = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBufferCount; ++i) {
            if (FAILED(swapchain_->GetBuffer(i, IID_PPV_ARGS(&backbuffers_[i])))) {
                Log("GetBuffer failed for swapchain index " + std::to_string(i));
                return false;
            }
            backbufferRtvs_[i] = OffsetHandle(base, i, rtvStride_);
            device_->CreateRenderTargetView(backbuffers_[i].Get(), nullptr, backbufferRtvs_[i]);
        }
        hudlessRtv_ = OffsetHandle(base, kBackBufferCount, rtvStride_);
        mvecRtv_ = OffsetHandle(base, kBackBufferCount + 1, rtvStride_);
        uiAlphaRtv_ = OffsetHandle(base, kBackBufferCount + 2, rtvStride_);
        dsv_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        return true;
    }

    bool CreateTexture(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState,
                       const D3D12_CLEAR_VALUE* clearValue, ComPtr<ID3D12Resource>& resource) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;

        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
                                                             clearValue, IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            Log("CreateCommittedResource failed for format " + std::to_string(static_cast<uint32_t>(format)) + ": " + Hr(hr));
            return false;
        }
        return true;
    }

    bool CreateSyntheticInputs() {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT uiSupport{kUiAlphaFormat};
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &uiSupport, sizeof(uiSupport))) ||
            (uiSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
            Log("R8_UNORM render-target support is required for Stage2 UI-alpha validation");
            return false;
        }

        D3D12_CLEAR_VALUE colorClear{};
        colorClear.Format = kColorFormat;
        colorClear.Color[0] = 0.06f;
        colorClear.Color[1] = 0.10f;
        colorClear.Color[2] = 0.18f;
        colorClear.Color[3] = 1.0f;
        if (!CreateTexture(kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &colorClear, hudless_)) return false;
        device_->CreateRenderTargetView(hudless_.Get(), nullptr, hudlessRtv_);

        D3D12_CLEAR_VALUE mvecClear{};
        mvecClear.Format = kMvecFormat;
        if (!CreateTexture(kMvecFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &mvecClear, mvec_)) return false;
        device_->CreateRenderTargetView(mvec_.Get(), nullptr, mvecRtv_);

        D3D12_CLEAR_VALUE uiClear{};
        uiClear.Format = kUiAlphaFormat;
        if (!CreateTexture(kUiAlphaFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &uiClear, uiAlpha_)) return false;
        device_->CreateRenderTargetView(uiAlpha_.Get(), nullptr, uiAlphaRtv_);

        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = kDepthFormat;
        depthClear.DepthStencil.Depth = 0.5f;
        depthClear.DepthStencil.Stencil = 0;
        if (!CreateTexture(kDepthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &depthClear, depth_)) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
        depthView.Format = kDepthFormat;
        depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(depth_.Get(), &depthView, dsv_);

        Log("Synthetic inputs created: D32 depth, RG16F dense zero-MV, BGRA8 HUDless, R8 UI alpha");
        return true;
    }

    bool CreateFrameContexts() {
        for (auto& frame : frames_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) {
                Log("CreateCommandAllocator failed");
                return false;
            }
            if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&frame.list)))) {
                Log("CreateCommandList failed");
                return false;
            }
            if (FAILED(frame.list->Close())) {
                Log("Initial command list close failed");
                return false;
            }
        }
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            Log("CreateFence failed");
            return false;
        }
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) {
            Log("CreateEvent for GPU fence failed");
            return false;
        }
        return true;
    }

    bool ConfigureDLSSG() {
        options_ = {};
        options_.mode = sl::DLSSGMode::eOn;
        options_.numFramesToGenerate = generatedFrames_;
        options_.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        options_.numBackBuffers = kBackBufferCount;
        options_.mvecDepthWidth = kWidth;
        options_.mvecDepthHeight = kHeight;
        options_.colorWidth = kWidth;
        options_.colorHeight = kHeight;
        options_.colorBufferFormat = static_cast<uint32_t>(kColorFormat);
        options_.mvecBufferFormat = static_cast<uint32_t>(kMvecFormat);
        options_.depthBufferFormat = static_cast<uint32_t>(kDepthFormat);
        options_.hudLessBufferFormat = static_cast<uint32_t>(kColorFormat);
        options_.uiBufferFormat = static_cast<uint32_t>(kUiAlphaFormat);
        options_.onErrorCallback = &OnPresentApiError;
        options_.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
        if (!SlOk(slDLSSGSetOptions(viewport_, options_), "slDLSSGSetOptions")) return false;
        dlssgConfigured_ = true;
        Log("DLSS-G enabled with UI recomposition and multiplier " + std::to_string(generatedFrames_ + 1));
        return true;
    }

    bool WaitForFrame(FrameContext& frame) {
        if (frame.fenceValue == 0 || fence_->GetCompletedValue() >= frame.fenceValue) return true;
        if (FAILED(fence_->SetEventOnCompletion(frame.fenceValue, fenceEvent_))) {
            Log("SetEventOnCompletion failed");
            return false;
        }
        WaitForSingleObject(fenceEvent_, INFINITE);
        return true;
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    void BuildConstants(sl::Constants& constants) const {
        const float nearZ = 0.1f;
        const float farZ = 1000.0f;
        const float fov = XMConvertToRadians(60.0f);
        const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
        const XMMATRIX viewToClip = XMMatrixPerspectiveFovLH(fov, aspect, nearZ, farZ);
        XMVECTOR determinant{};
        const XMMATRIX clipToView = XMMatrixInverse(&determinant, viewToClip);
        const XMMATRIX identity = XMMatrixIdentity();
        StoreMatrix(constants.cameraViewToClip, viewToClip);
        StoreMatrix(constants.clipToCameraView, clipToView);
        StoreMatrix(constants.clipToLensClip, identity);
        StoreMatrix(constants.clipToPrevClip, identity);
        StoreMatrix(constants.prevClipToClip, identity);
        constants.jitterOffset = {0.0f, 0.0f};
        constants.mvecScale = {1.0f / static_cast<float>(kWidth), 1.0f / static_cast<float>(kHeight)};
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {0.0f, 0.0f, 0.0f};
        constants.cameraUp = {0.0f, 1.0f, 0.0f};
        constants.cameraRight = {1.0f, 0.0f, 0.0f};
        constants.cameraFwd = {0.0f, 0.0f, 1.0f};
        constants.cameraNear = nearZ;
        constants.cameraFar = farZ;
        constants.cameraFOV = fov;
        constants.cameraAspectRatio = aspect;
        constants.depthInverted = sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = frameIndex_ == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        constants.motionVectorsDilated = sl::Boolean::eFalse;
        constants.motionVectorsJittered = sl::Boolean::eFalse;
    }

    bool TagInputs(const sl::FrameToken& token) {
        const uint32_t state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource depthRes(sl::ResourceType::eTex2d, depth_.Get(), state);
        sl::Resource mvecRes(sl::ResourceType::eTex2d, mvec_.Get(), state);
        sl::Resource hudlessRes(sl::ResourceType::eTex2d, hudless_.Get(), state);
        sl::Resource uiAlphaRes(sl::ResourceType::eTex2d, uiAlpha_.Get(), state);
        for (auto* resource : {&depthRes, &mvecRes, &hudlessRes, &uiAlphaRes}) {
            resource->width = kWidth;
            resource->height = kHeight;
            resource->mipLevels = 1;
            resource->arrayLayers = 1;
        }
        depthRes.nativeFormat = static_cast<uint32_t>(kDepthFormat);
        mvecRes.nativeFormat = static_cast<uint32_t>(kMvecFormat);
        hudlessRes.nativeFormat = static_cast<uint32_t>(kColorFormat);
        uiAlphaRes.nativeFormat = static_cast<uint32_t>(kUiAlphaFormat);

        const sl::Extent extent{0, 0, kWidth, kHeight};
        sl::ResourceTag tags[] = {
            {&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
            {&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent},
            {&hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent},
            {&uiAlphaRes, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent}
        };
        return SlOk(slSetTagForFrame(token, viewport_, tags, static_cast<uint32_t>(std::size(tags)), nullptr), "slSetTagForFrame");
    }

    bool RecordSyntheticFrame(FrameContext& frame, UINT backbufferIndex) {
        if (FAILED(frame.allocator->Reset())) {
            Log("Command allocator reset failed");
            return false;
        }
        if (FAILED(frame.list->Reset(frame.allocator.Get(), nullptr))) {
            Log("Command list reset failed");
            return false;
        }

        std::array<D3D12_RESOURCE_BARRIER, 5> toWrite{
            Transition(backbuffers_[backbufferIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(hudless_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(mvec_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(uiAlpha_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(depth_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
        };
        frame.list->ResourceBarrier(static_cast<UINT>(toWrite.size()), toWrite.data());

        const float sceneColor[4]{0.06f, 0.10f, 0.18f, 1.0f};
        const float zero[4]{0.0f, 0.0f, 0.0f, 0.0f};
        frame.list->ClearRenderTargetView(backbufferRtvs_[backbufferIndex], sceneColor, 0, nullptr);
        frame.list->ClearRenderTargetView(hudlessRtv_, sceneColor, 0, nullptr);
        frame.list->ClearRenderTargetView(mvecRtv_, zero, 0, nullptr);
        frame.list->ClearRenderTargetView(uiAlphaRtv_, zero, 0, nullptr);
        frame.list->ClearDepthStencilView(dsv_, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);

        std::array<D3D12_RESOURCE_BARRIER, 5> toRead{
            Transition(backbuffers_[backbufferIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT),
            Transition(hudless_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(mvec_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(uiAlpha_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(depth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        };
        frame.list->ResourceBarrier(static_cast<UINT>(toRead.size()), toRead.data());

        if (FAILED(frame.list->Close())) {
            Log("Command list close failed");
            return false;
        }
        return true;
    }

    bool RenderOneFrame() {
        sl::FrameToken* token{};
        const uint32_t currentFrameIndex = frameIndex_;
        if (!SlOk(slGetNewFrameToken(token, &currentFrameIndex), "slGetNewFrameToken") || !token) return false;
        if (!SlOk(slReflexSleep(*token), "slReflexSleep")) return false;
        if (!SlOk(slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token), "PCL SimulationStart")) return false;

        sl::Constants constants{};
        BuildConstants(constants);
        if (!SlOk(slSetConstants(constants, *token, viewport_), "slSetConstants")) return false;
        if (!SlOk(slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token), "PCL SimulationEnd")) return false;

        const UINT backbufferIndex = swapchain_->GetCurrentBackBufferIndex();
        FrameContext& frame = frames_[backbufferIndex];
        if (!WaitForFrame(frame)) return false;
        if (!RecordSyntheticFrame(frame, backbufferIndex)) return false;

        if (!SlOk(slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token), "PCL RenderSubmitStart")) return false;
        ID3D12CommandList* lists[]{frame.list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        if (!SlOk(slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token), "PCL RenderSubmitEnd")) return false;

        if (!TagInputs(*token)) return false;
        if (!SlOk(slDLSSGSetOptions(viewport_, options_), "slDLSSGSetOptions(frame)")) return false;

        if (!SlOk(slPCLSetMarker(sl::PCLMarker::ePresentStart, *token), "PCL PresentStart")) return false;
        g_lastPresentError.store(S_OK, std::memory_order_relaxed);
        const HRESULT presentHr = swapchain_->Present(0, 0);
        if (!SlOk(slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token), "PCL PresentEnd")) return false;
        if (FAILED(presentHr)) {
            Log("Present returned failure: " + Hr(presentHr));
            return false;
        }
        const HRESULT asyncError = g_lastPresentError.load(std::memory_order_relaxed);
        if (FAILED(asyncError)) {
            Log("DLSS-G Present API callback reported: " + Hr(asyncError));
            return false;
        }

        const uint64_t signalValue = nextFenceValue_++;
        if (FAILED(queue_->Signal(fence_.Get(), signalValue))) {
            Log("Queue Signal failed");
            return false;
        }
        frame.fenceValue = signalValue;

        if ((frameIndex_ % 120u) == 0u) {
            sl::DLSSGState state{};
            const sl::Result r = slDLSSGGetState(viewport_, state, &options_);
            if (r == sl::Result::eOk) {
                Log("DLSS-G state: status=" + std::to_string(static_cast<uint32_t>(state.status)) +
                    " presented=" + std::to_string(state.numFramesActuallyPresented) +
                    " maxGenerated=" + std::to_string(state.numFramesToGenerateMax));
                if (state.status != sl::DLSSGStatus::eOk) {
                    Log("DLSS-G status is not eOk; Stage2 integration needs investigation before capture is added");
                }
            } else {
                Log(std::string("slDLSSGGetState(periodic) -> ") + sl::getResultAsStr(r));
            }
        }

        ++frameIndex_;
        return true;
    }

    void WaitForGpuIdle() {
        if (!queue_ || !fence_) return;
        const uint64_t value = nextFenceValue_++;
        if (SUCCEEDED(queue_->Signal(fence_.Get(), value)) && fence_->GetCompletedValue() < value && fenceEvent_) {
            if (SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_))) {
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }
    }

    void Shutdown() {
        WaitForGpuIdle();
        if (dlssgConfigured_ && slInitialized_) {
            options_.mode = sl::DLSSGMode::eOff;
            slDLSSGSetOptions(viewport_, options_);
            dlssgConfigured_ = false;
        }
        if (fenceEvent_) {
            CloseHandle(fenceEvent_);
            fenceEvent_ = nullptr;
        }
        for (auto& frame : frames_) {
            frame.list.Reset();
            frame.allocator.Reset();
        }
        fence_.Reset();
        depth_.Reset();
        uiAlpha_.Reset();
        mvec_.Reset();
        hudless_.Reset();
        for (auto& b : backbuffers_) b.Reset();
        dsvHeap_.Reset();
        rtvHeap_.Reset();
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
            Log("Stage 2 shutdown complete");
            g_log.close();
        }
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    bool running_{true};
    bool slInitialized_{};
    bool dlssgConfigured_{};
    std::wstring exeDir_;
    std::wstring logDir_;

    sl::ViewportHandle viewport_{0};
    sl::DLSSGOptions options_{};
    uint32_t generatedFrames_{1};
    uint32_t frameIndex_{};

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    std::array<ComPtr<ID3D12Resource>, kBackBufferCount> backbuffers_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBufferCount> backbufferRtvs_{};
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    UINT rtvStride_{};
    D3D12_CPU_DESCRIPTOR_HANDLE hudlessRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE mvecRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE uiAlphaRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_{};

    ComPtr<ID3D12Resource> hudless_;
    ComPtr<ID3D12Resource> mvec_;
    ComPtr<ID3D12Resource> uiAlpha_;
    ComPtr<ID3D12Resource> depth_;

    std::array<FrameContext, kBackBufferCount> frames_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_{};
    uint64_t nextFenceValue_{1};
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    App app;
    if (!app.Init(instance)) {
        MessageBoxW(nullptr,
            L"Clean Stage 2 initialization failed. See logs\\ExternalDLSSG-Zero-Stage2.log for the exact failing stage.",
            L"External DLSS-G - Stage 2", MB_OK | MB_ICONERROR);
        return 2;
    }
    return app.Run();
}
