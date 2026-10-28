#include "runtime.h"
#include "diagnostics.h"
#include <softpub.h>
#include <wintrust.h>

void FrameGeneration::load(bool diagnostics) {
    wchar_t path[32768];
    GetModuleFileNameW(nullptr, path, 32768);
    auto dir = std::filesystem::path(path).parent_path();
    auto file = (dir / L"sl.interposer.dll").wstring();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    auto trust = WinVerifyTrust(nullptr, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    if (trust != ERROR_SUCCESS)
        throw std::runtime_error("Streamline signature verification failed. Use the original NVIDIA DLLs and check Windows certificate trust.");

    dll = LoadLibraryExW(file.c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll)
        throw std::runtime_error("Extract all runtime DLLs beside the EXE");
    load(initFn, "slInit");
    load(shutdownFn, "slShutdown");
    load(deviceFn, "slSetD3DDevice");
    load(featureFn, "slGetFeatureFunction");
    load(supportedFn, "slIsFeatureSupported");
    load(requirementsFn, "slGetFeatureRequirements");
    load(tokenFn, "slGetNewFrameToken");
    load(constantsFn, "slSetConstants");
    load(tagsFn, "slSetTagForFrame");
    load(createFactory, "CreateDXGIFactory1");
    load(createDevice, "D3D12CreateDevice");

    auto dirString = dir.wstring();
    const wchar_t *dirs[] = {dirString.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences p{};
    p.pathsToPlugins = dirs;
    p.numPathsToPlugins = 1;
    p.pathToLogsAndData = dirString.c_str();
    p.featuresToLoad = features;
    p.numFeaturesToLoad = 3;
    p.logLevel = diagnostics ? sl::LogLevel::eDefault : sl::LogLevel::eOff;
    p.flags = sl::PreferenceFlags::eDisableCLStateTracking |
              sl::PreferenceFlags::eUseFrameBasedResourceTagging |
              sl::PreferenceFlags::eUseDXGIFactoryProxy;
    p.engine = sl::EngineType::eCustom;
    p.engineVersion = "FrameGeneration";
    p.projectId = "e86d5d1d-b886-47ac-a90d-48708688358b";
    p.renderAPI = sl::RenderAPI::eD3D12;
    ck(initFn(p, sl::kSDKVersion), "Streamline initialization");
    initialized = true;
    diagnostics::write("Streamline initialized: D3D12 + DLSS-G + Reflex + PCL plugins requested");
}

void FrameGeneration::configure(IDXGIAdapter1 *a, UINT w, UINT h, UINT multiplier, bool vsync,
                                UINT centerBypassPx) {
    width = w;
    height = h;
    enabled = multiplier > 1;
    viewportCount = 1;
    inputExtents.fill(sl::Extent{});
    inputExtents[0] = {0, 0, w, h};
    if (!enabled)
        return;

    DXGI_ADAPTER_DESC1 ad{};
    check(a->GetDesc1(&ad), "Adapter descriptor");
    sl::AdapterInfo ai{};
    ai.deviceLUID = (uint8_t *)&ad.AdapterLuid;
    ai.deviceLUIDSizeInBytes = sizeof(LUID);
    ck(supportedFn(sl::kFeatureDLSS_G, ai), "DLSS-G support");

    feature(optionsFn, sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    feature(stateFn, sl::kFeatureDLSS_G, "slDLSSGGetState");
    feature(reflexFn, sl::kFeatureReflex, "slReflexSetOptions");
    feature(sleepFn, sl::kFeatureReflex, "slReflexSleep");
    feature(markerFn, sl::kFeaturePCL, "slPCLSetMarker");

    sl::DLSSGOptions opt{};
    opt.mode = sl::DLSSGMode::eOff;
    opt.numBackBuffers = 2;
    opt.numFramesToGenerate = multiplier - 1;
    opt.colorWidth = w;
    opt.colorHeight = h;
    opt.colorBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    opt.mvecDepthWidth = w;
    opt.mvecDepthHeight = h;
    opt.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    opt.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    // The centered protection area is clipped out of the presenter window, not processed
    // as DLSS-G UI. Keeping UI recomposition disabled prevents the crosshair from being
    // independently interpolated and removes the source of the observed flicker.
    (void)centerBypassPx;
    opt.enableUserInterfaceRecomposition = sl::Boolean::eFalse;

    sl::DLSSGState state{};
    ck(stateFn(viewports[0], state, &opt), "DLSS-G capabilities");
    diagnostics::write("DLSS-G capabilities: max_generated_between_real=" +
                       std::to_string(state.numFramesToGenerateMax) +
                       " status=" + std::to_string(unsigned(state.status)));
    if (multiplier - 1 > state.numFramesToGenerateMax)
        throw std::runtime_error("Selected FG multiplier exceeds GPU/runtime support");
    if (vsync && state.bIsVsyncSupportAvailable != sl::Boolean::eTrue)
        throw std::runtime_error("This runtime does not support VSync with FG; disable VSync");

    sl::ReflexOptions ro{};
    ro.mode = sl::ReflexMode::eLowLatencyWithBoost;
    ck(reflexFn(ro), "Reflex On + Boost");
    opt.mode = sl::DLSSGMode::eOn;
    activeOptions = opt;

    sl::float4x4 identity{};
    identity[0] = {1, 0, 0, 0};
    identity[1] = {0, 1, 0, 0};
    identity[2] = {0, 0, 1, 0};
    identity[3] = {0, 0, 0, 1};
    baseConstants.cameraViewToClip = baseConstants.clipToCameraView = baseConstants.clipToLensClip =
        baseConstants.clipToPrevClip = baseConstants.prevClipToClip = identity;
    baseConstants.jitterOffset = {0, 0};
    baseConstants.mvecScale = {1.f / width, 1.f / height};
    baseConstants.cameraPinholeOffset = {0, 0};
    baseConstants.cameraPos = {0, 0, 0};
    baseConstants.cameraUp = {0, 1, 0};
    baseConstants.cameraRight = {1, 0, 0};
    baseConstants.cameraFwd = {0, 0, 1};
    baseConstants.cameraNear = .1f;
    baseConstants.cameraFar = 1000;
    baseConstants.cameraFOV = 1.04719755f;
    baseConstants.cameraAspectRatio = float(width) / height;
    baseConstants.motionVectorsInvalidValue = 65504;
    baseConstants.depthInverted = sl::eFalse;
    baseConstants.cameraMotionIncluded = sl::eTrue;
    baseConstants.motionVectors3D = sl::eFalse;
    baseConstants.orthographicProjection = sl::eTrue;

    for (UINT i = 0; i < viewportCount; ++i)
        ck(optionsFn(viewports[i], opt), "Enable DLSS-G viewport");
    diagnostics::write("DLSS-G enabled: requested_multiplier=" + std::to_string(multiplier) +
                       " generated_between_real=" + std::to_string(multiplier - 1) +
                       " zero_motion=on flat_depth=on UI_recomposition=off");
}

void FrameGeneration::begin() {
    if (!enabled)
        return;
    uint32_t frame = index++;
    ck(tokenFn(token, &frame), "Frame token");
    ck(sleepFn(*token), "Reflex pacing");
}

void FrameGeneration::constants(bool reset) {
    if (!enabled)
        return;
    auto c = baseConstants;
    c.reset = reset ? sl::eTrue : sl::eFalse;
    for (UINT i = 0; i < viewportCount; ++i)
        ck(constantsFn(c, *token, viewports[i]), "Flat-plane constants");
}

void FrameGeneration::tag(ID3D12GraphicsCommandList *l, ID3D12Resource *motion,
                          ID3D12Resource *depth, ID3D12Resource *back,
                          ID3D12Resource *uiColorAndAlpha) {
    if (!enabled)
        return;

    sl::Resource m(sl::ResourceType::eTex2d, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        d(sl::ResourceType::eTex2d, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        b(sl::ResourceType::eTex2d, back, D3D12_RESOURCE_STATE_PRESENT);
    m.width = d.width = b.width = width;
    m.height = d.height = b.height = height;
    m.nativeFormat = DXGI_FORMAT_R16G16_FLOAT;
    d.nativeFormat = DXGI_FORMAT_R32_FLOAT;
    b.nativeFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    const auto &e = inputExtents[0];
    sl::ResourceTag tags[5] = {
        {&m, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &e},
        {&d, sl::kBufferTypeDepth, sl::eValidUntilPresent, &e},
        {&b, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent, &e},
        {&b, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &e},
        {nullptr, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &e},
    };

    UINT tagCount = 3;
    sl::Resource ui{};
    if (uiColorAndAlpha) {
        // The backbuffer is also a valid hudless source outside the protected square.
        // The UI texture is transparent everywhere except the center square, where it is
        // opaque and contains the exact native capture pixels (premultiplied alpha = 1).
        ui = sl::Resource(sl::ResourceType::eTex2d, uiColorAndAlpha,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ui.width = width;
        ui.height = height;
        ui.nativeFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        tags[4].resource = &ui;
        tagCount = 5;
    }

    ck(tagsFn(*token, viewports[0], tags, tagCount, l),
       uiColorAndAlpha ? "DLSS-G center recomposition tags" : "DLSS-G input tags");
}

FrameGenerationState FrameGeneration::afterPresent() {
    FrameGenerationState out{};
    if (!enabled)
        return out;

    uint32_t statusBits = 0;
    uint32_t presented = 0;
    for (UINT i = 0; i < viewportCount; ++i) {
        sl::DLSSGState s{};
        ck(stateFn(viewports[i], s, nullptr), "DLSS-G viewport state");
        if (i == 0)
            out.state = s;
        statusBits |= uint32_t(s.status);
        presented = std::max(presented, s.numFramesActuallyPresented);

        auto *f = (ID3D12Fence *)s.inputsProcessingCompletionFence;
        const uint64_t v = s.lastPresentInputsProcessingCompletionFenceValue;
        if (!f || !v)
            continue;
        UINT found = out.fenceCount;
        for (UINT j = 0; j < out.fenceCount; ++j) {
            if (out.fences[j] == f) {
                found = j;
                break;
            }
        }
        if (found < out.fenceCount) {
            out.values[found] = std::max(out.values[found], v);
        } else if (out.fenceCount < out.fences.size()) {
            out.fences[out.fenceCount] = f;
            out.values[out.fenceCount] = v;
            ++out.fenceCount;
        } else {
            throw std::runtime_error("Too many DLSS-G input fences");
        }
    }
    out.state.status = static_cast<sl::DLSSGStatus>(statusBits);
    out.state.numFramesActuallyPresented = presented;
    out.state.inputsProcessingCompletionFence = nullptr;
    out.state.lastPresentInputsProcessingCompletionFenceValue = 0;
    return out;
}

void FrameGeneration::pause(bool p) {
    if (!enabled)
        return;
    auto o = activeOptions;
    if (p)
        o.mode = sl::DLSSGMode::eOff;
    for (UINT i = 0; i < viewportCount; ++i)
        ck(optionsFn(viewports[i], o), "Pause/resume DLSS-G viewport");
}

void FrameGeneration::shutdown() {
    if (initialized) {
        shutdownFn();
        initialized = false;
    }
}
