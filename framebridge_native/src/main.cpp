// FrameBridgeNative: a strictly external, GPU-resident capture/presenter probe.
// NVIDIA Frame Warp is deliberately absent until the actual SDK is integrated.
// Copyright (c) 2026. MIT-style project; see README.md.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;

constexpr int kHotkeyPause = 2;
constexpr int kHotkeyExit = 3;

static const char* kShader = R"(
Texture2D imageTex : register(t0);
SamplerState samp : register(s0);
struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Out VS(uint id : SV_VertexID) {
    Out o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0, 1);
    return o;
}
float4 PS(Out i) : SV_Target {
    return imageTex.Sample(samp, saturate(i.uv));
}
)";

struct App {
    HWND window{};
    bool debugWindow = false;
    bool paused = false;
    bool tearing = false;
    bool excluded = false;
    bool running = true;
    unsigned captureCount = 0, presentedCount = 0;
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER lastGrabTime{};
    std::chrono::steady_clock::time_point reportStart = std::chrono::steady_clock::now();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> capture;
    ComPtr<ID3D11ShaderResourceView> captureView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11SamplerState> sampler;
    UINT width{}, height{};
    RECT desktopRect{};

    void log(const char* msg) const {
        OutputDebugStringA(msg);
        std::fputs(msg, stdout);
        std::fflush(stdout);
    }

    bool initialize();
    bool startDuplication();
    bool createCaptureResource(UINT w, UINT h);
    bool grab();
    void draw();
    void report();
    void loop();
};

static App* gApp{};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_NCHITTEST:
        if (gApp && !gApp->debugWindow) return HTTRANSPARENT;
        break;
    case WM_MOUSEACTIVATE:
        if (gApp && !gApp->debugWindow) return MA_NOACTIVATE;
        break;
    case WM_HOTKEY:
        if (gApp) {
            if (wparam == kHotkeyPause) {
                gApp->paused = !gApp->paused;
                gApp->log(gApp->paused ? "[presenter] PAUSED\n" : "[presenter] RESUMED\n");
                ShowWindow(hwnd, gApp->paused ? SW_HIDE : SW_SHOWNOACTIVATE);
            } else if (wparam == kHotkeyExit) {
                PostQuitMessage(0);
            }
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (gApp) {
            gApp->duplication.Reset();
            gApp->log("[capture] display changed; will reacquire\n");
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool App::createCaptureResource(UINT w, UINT h) {
    captureView.Reset();
    capture.Reset();
    if (!w || !h) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &capture))) return false;
    if (FAILED(device->CreateShaderResourceView(capture.Get(), nullptr, &captureView))) return false;
    width = w;
    height = h;
    return true;
}

bool App::startDuplication() {
    duplication.Reset();
    output.Reset();
    ComPtr<IDXGIOutput> base;
    for (UINT i = 0; adapter->EnumOutputs(i, &base) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(base->GetDesc(&desc)) && desc.AttachedToDesktop) {
            desktopRect = desc.DesktopCoordinates;
            if (SUCCEEDED(base.As(&output))) break;
        }
        base.Reset();
    }
    if (!output) { log("[capture] no desktop output\n"); return false; }
    HRESULT hr = output->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(hr)) {
        char s[100];
        std::snprintf(s, sizeof(s), "[capture] DuplicateOutput failed 0x%08lX\n", static_cast<unsigned long>(hr));
        log(s);
        return false;
    }
    DXGI_OUTDUPL_DESC desc{};
    duplication->GetDesc(&desc);
    if (!createCaptureResource(desc.ModeDesc.Width, desc.ModeDesc.Height)) return false;
    return true;
}

bool App::initialize() {
    QueryPerformanceFrequency(&qpcFreq);
    UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
        levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (hr == E_INVALIDARG) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        deviceFlags, &levels[1], 1, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) { log("[d3d11] hardware device unavailable\n"); return false; }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(device.As(&dxgiDevice1))) dxgiDevice1->SetMaximumFrameLatency(1);
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL available = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &available, sizeof(available)))) tearing = !!available;
    }
    // Fail closed if display duplication is not permitted.
    if (!startDuplication()) return false;

    const wchar_t* name = L"FrameBridgeNativeWindow";
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = WndProc;
    cls.lpszClassName = name;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&cls)) return false;
    DWORD ex = debugWindow ? WS_EX_APPWINDOW : WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    DWORD style = debugWindow ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    int wx = debugWindow ? 160 : desktopRect.left;
    int wy = debugWindow ? 120 : desktopRect.top;
    int ww = debugWindow ? 960 : static_cast<int>(width);
    int wh = debugWindow ? 540 : static_cast<int>(height);
    window = CreateWindowExW(ex, name, L"FrameBridgeNative - Capture Probe", style,
        wx, wy, ww, wh, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return false;
    gApp = this;
    ShowWindow(window, debugWindow ? SW_SHOW : SW_SHOWNOACTIVATE);
    // Best-effort system-supported self-capture exclusion. We do not bypass it on failure.
    excluded = !!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
    if (!excluded && !debugWindow) {
        log("[safety] capture exclusion unavailable: use --windowed for diagnostics\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(ww);
    sd.Height = static_cast<UINT>(wh);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), window, &sd, nullptr, nullptr, &swap))) return false;
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) ||
        FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv))) return false;
    ComPtr<ID3DBlob> vs, ps, errors;
    hr = D3DCompile(kShader, std::strlen(kShader), "FrameBridgeShader", nullptr, nullptr,
        "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
    if (FAILED(hr)) { log(errors ? static_cast<char*>(errors->GetBufferPointer()) : "[shader] VS failed\n"); return false; }
    errors.Reset();
    hr = D3DCompile(kShader, std::strlen(kShader), "FrameBridgeShader", nullptr, nullptr,
        "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
    if (FAILED(hr)) { log(errors ? static_cast<char*>(errors->GetBufferPointer()) : "[shader] PS failed\n"); return false; }
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader))) return false;
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samp, &sampler))) return false;
    RegisterHotKey(window, kHotkeyPause, MOD_ALT | MOD_NOREPEAT, VK_F9);
    RegisterHotKey(window, kHotkeyExit, MOD_ALT | MOD_NOREPEAT, VK_F10);
    log("[ready] ALT+F9 pause | ALT+F10 exit; no custom warp is installed\n");
    log("[features] NVIDIA DLSS-G: NOT INTEGRATED; Reflex 2 Frame Warp: SDK UNAVAILABLE\n");
    return true;
}

bool App::grab() {
    if (!duplication && !startDuplication()) return false;
    DXGI_OUTDUPL_FRAME_INFO frame{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication->AcquireNextFrame(0, &frame, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        duplication.Reset();
        return false;
    }
    if (FAILED(hr)) return false;
    ComPtr<ID3D11Texture2D> texture;
    bool restart = false;
    if (SUCCEEDED(resource.As(&texture)) && capture) {
        D3D11_TEXTURE2D_DESC sd{};
        texture->GetDesc(&sd);
        if (sd.Width == width && sd.Height == height) {
            context->CopyResource(capture.Get(), texture.Get());
            context->Flush(); // Submit GPU copy before returning the duplication frame.
            lastGrabTime = frame.LastPresentTime;
            ++captureCount;
        } else {
            restart = true; // Mode switch/Alt+Tab: reacquire with new dimensions.
        }
    }
    duplication->ReleaseFrame();
    if (restart) duplication.Reset();
    return true;
}

void App::draw() {
    if (!captureView) return;
    D3D11_VIEWPORT viewport{};
    RECT rc{};
    GetClientRect(window, &rc);
    viewport.Width = static_cast<float>(rc.right - rc.left);
    viewport.Height = static_cast<float>(rc.bottom - rc.top);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    const float black[4]{};
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(rtv.Get(), black);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, captureView.GetAddressOf());
    context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    context->Draw(3, 0);
    ID3D11ShaderResourceView* nil = nullptr;
    context->PSSetShaderResources(0, 1, &nil);
    HRESULT hr = swap->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (SUCCEEDED(hr)) ++presentedCount;
    else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) running = false;
}

void App::report() {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - reportStart).count();
    if (elapsed < 2.0) return;
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    double ageMs = (qpcFreq.QuadPart && lastGrabTime.QuadPart)
        ? (qpc.QuadPart - lastGrabTime.QuadPart) * 1000.0 / qpcFreq.QuadPart : -1.0;
    char line[256];
    std::snprintf(line, sizeof(line), "[stats] acquired %.1f/s, presented %.1f/s, last frame age %.2fms\n",
        captureCount / elapsed, presentedCount / elapsed, ageMs);
    log(line);
    captureCount = presentedCount = 0;
    reportStart = now;
}

void App::loop() {
    MSG msg{};
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        if (!paused) {
            bool fresh = grab();
            draw();
            if (!fresh) Sleep(1);
        } else Sleep(10);
        report();
    }
    UnregisterHotKey(window, kHotkeyPause);
    UnregisterHotKey(window, kHotkeyExit);
    duplication.Reset();
    context->ClearState();
    context->Flush();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    AllocConsole();
    FILE* out = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    std::string cmdline;
    if (cmd) {
        while (*cmd) { cmdline.push_back(static_cast<char>(*cmd)); ++cmd; }
    }
    auto app = std::make_unique<App>();
    app->debugWindow = cmdline.find("--windowed") != std::string::npos;
    if (!app->initialize()) {
        app->log("[fatal] initialization failed; no alternate capture path was used\n");
        MessageBoxA(nullptr, "FrameBridgeNative could not initialize. Check its console log.",
            "FrameBridgeNative", MB_ICONERROR | MB_OK);
        return 1;
    }
    app->loop();
    return 0;
}
