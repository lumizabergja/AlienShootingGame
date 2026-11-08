// Genuine NVIDIA Streamline 2.14.16 integration diagnostics.
// A successful probe is NOT evidence that frames are being generated.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

using Microsoft::WRL::ComPtr;

static FILE* gLog = nullptr;
static void say(const char* label, long long value) {
    std::printf("%-36s %lld\n", label, value);
    if (gLog) {
        std::fprintf(gLog, "%-36s %lld\n", label, value);
        std::fflush(gLog);
    }
}
static void info(const char* line) {
    std::puts(line);
    if (gLog) {
        std::fprintf(gLog, "%s\n", line);
        std::fflush(gLog);
    }
}

int main() {
    if (fopen_s(&gLog, "FrameBridgeNvidiaProbe.log", "w") != 0) {
        std::puts("Cannot open log in current directory; console results will still be shown.");
    }
    info("FrameBridge NVIDIA SDK probe - genuine Streamline APIs, not a frame generator");
    info("The probe does not capture a game or modify any game's process.");
    info("Reflex 2 Frame Warp is NOT integrated. No custom warp substitutes are used.");

    // SL must initialize BEFORE any graphics device or factory is created.
    sl::Feature requested[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex};
    sl::Preferences preferences{};
    preferences.featuresToLoad = requested;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(_countof(requested));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    // Our own project identifier; not an NVIDIA-issued application ID.
    preferences.projectId = "defa3629-69df-40b4-9777-83586e04d16f";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.logLevel = sl::LogLevel::eDefault;

    const sl::Result init = slInit(preferences);
    say("slInit result", static_cast<int>(init));
    if (init != sl::Result::eOk) {
        info("NVIDIA Streamline initialization FAILED. No fallback engine is used.");
        if (gLog) std::fclose(gLog);
        return 2;
    }

    // HAGS, driver, and API requirements are checked by NVIDIA's own SDK.
    sl::FeatureRequirements requirements{};
    const auto reqResult = slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements);
    say("DLSS-G requirements query", static_cast<int>(reqResult));
    if (reqResult == sl::Result::eOk) {
        say("DLSS-G requirement flags", static_cast<int64_t>(requirements.flags));
        say("DLSS-G max viewports", requirements.maxNumViewports);
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        say("CreateDXGIFactory1 HRESULT", static_cast<long long>(hr));
        slShutdown();
        if (gLog) std::fclose(gLog);
        return 3;
    }

    ComPtr<IDXGIAdapter1> selected;
    bool foundSupport = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        sl::AdapterInfo adapterInfo{};
        adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        adapterInfo.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
        const sl::Result supported = slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo);
        char adapterName[128]{};
        std::snprintf(adapterName, sizeof(adapterName), "Adapter %u, vendor 0x%04x", i, desc.VendorId);
        info(adapterName);
        say("NVIDIA DLSS-G adapter support", static_cast<int>(supported));
        if (supported == sl::Result::eOk) {
            selected = candidate;
            foundSupport = true;
            break;
        }
    }

    if (!foundSupport) {
        info("No adapter passed NVIDIA's DLSS-G support query. The real feature is unavailable.");
        selected.Reset();
        factory.Reset();
        slShutdown();
        if (gLog) std::fclose(gLog);
        return 4;
    }

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        say("D3D12CreateDevice HRESULT", static_cast<long long>(hr));
        selected.Reset();
        factory.Reset();
        slShutdown();
        if (gLog) std::fclose(gLog);
        return 5;
    }
    const sl::Result deviceResult = slSetD3DDevice(device.Get());
    say("slSetD3DDevice result", static_cast<int>(deviceResult));
    if (deviceResult == sl::Result::eOk) {
        sl::ViewportHandle viewport{0};
        sl::DLSSGState state{};
        const auto stateResult = slDLSSGGetState(viewport, state, nullptr);
        say("slDLSSGGetState result", static_cast<int>(stateResult));
        if (stateResult == sl::Result::eOk) {
            say("DLSS-G state status bitfield", static_cast<int>(state.status));
            say("Maximum generated frames/real", state.numFramesToGenerateMax);
            say("Maximum frame multiplier", 1 + state.numFramesToGenerateMax);
            say("Minimum dimension", state.minWidthOrHeight);
            say("Frames actually presented", state.numFramesActuallyPresented);
            // In this probe zero generated frames is expected: no frame producer/swap chain exists.
        }
    }

    info("SDK diagnostic finished. Generated frames = 0 (this is expected in a probe).");
    info("Native Frame Warp = unavailable without NVIDIA FrameWarp SDK and real camera data.");
    device.Reset();
    selected.Reset();
    factory.Reset();
    say("slShutdown result", static_cast<int>(slShutdown()));
    if (gLog) std::fclose(gLog);
    return deviceResult == sl::Result::eOk ? 0 : 6;
}
