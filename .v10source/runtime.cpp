#include "runtime.h"
#include <softpub.h>
#include <wintrust.h>

namespace {
struct CommonComputeDiscovery {
    void** slot{};
    uintptr_t slotRva{};
    uintptr_t publishRva{};
};

CommonComputeDiscovery discoverCommonComputeSlot(HMODULE module) {
    if (!module) throw std::runtime_error("V10 sl.common module is unavailable");
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        throw std::runtime_error("V10 sl.common DOS header is invalid");
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error("V10 sl.common PE header is invalid");

    const auto imageSize = size_t(nt->OptionalHeader.SizeOfImage);
    auto* sections = IMAGE_FIRST_SECTION(nt);
    uint8_t* textBegin{};
    size_t textSize{};
    uint8_t* stringAddr{};
    constexpr char key[] = "sl.param.common.computeAPI";

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        auto& sec = sections[i];
        char name[9]{};
        memcpy(name, sec.Name, 8);
        auto* begin = base + sec.VirtualAddress;
        size_t size = std::min<size_t>(sec.Misc.VirtualSize ? sec.Misc.VirtualSize : sec.SizeOfRawData,
                                       imageSize - sec.VirtualAddress);
        if (!strcmp(name, ".text")) { textBegin = begin; textSize = size; }
        if (!strcmp(name, ".rdata")) {
            for (size_t off = 0; off + sizeof(key) <= size; ++off) {
                if (!memcmp(begin + off, key, sizeof(key))) {
                    if (stringAddr) throw std::runtime_error("V10 compute key is not unique in sl.common");
                    stringAddr = begin + off;
                }
            }
        }
    }
    if (!textBegin || !textSize || !stringAddr)
        throw std::runtime_error("V10 could not locate sl.common text/compute key");

    void** foundSlot{};
    uintptr_t foundPublish{};
    for (size_t i = 0; i + 16 <= textSize; ++i) {
        auto* q = textBegin + i;
        // Exact x64 sequence in the pinned NVIDIA sl.common publish path:
        //   mov r8,  [rip+disp32]   ; ctx.compute
        //   lea rdx, [rip+disp32]   ; "sl.param.common.computeAPI"
        //   call qword ptr [rax]    ; IParameters::set(const char*, void*)
        if (q[0] != 0x4c || q[1] != 0x8b || q[2] != 0x05 ||
            q[7] != 0x48 || q[8] != 0x8d || q[9] != 0x15 ||
            q[14] != 0xff || q[15] != 0x10)
            continue;
        int32_t movDisp{}, leaDisp{};
        memcpy(&movDisp, q + 3, sizeof(movDisp));
        memcpy(&leaDisp, q + 10, sizeof(leaDisp));
        auto* keyTarget = q + 14 + leaDisp;
        if (keyTarget != stringAddr) continue;
        auto* slotAddr = q + 7 + movDisp;
        if (slotAddr < base || slotAddr + sizeof(void*) > base + imageSize)
            throw std::runtime_error("V10 recovered compute slot outside sl.common image");
        if (foundSlot && foundSlot != reinterpret_cast<void**>(slotAddr))
            throw std::runtime_error("V10 found multiple compute publish slots in sl.common");
        foundSlot = reinterpret_cast<void**>(slotAddr);
        foundPublish = uintptr_t(q - base);
    }
    if (!foundSlot)
        throw std::runtime_error("V10 could not recover sl.common ctx.compute publish slot");
    return { foundSlot, uintptr_t(reinterpret_cast<uint8_t*>(foundSlot) - base), foundPublish };
}
}
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
        throw std::runtime_error("Streamline signature verification failed. Use the original NVIDIA DLLs and "
                                 "check Windows certificate trust.");
    dll = LoadLibraryExW(file.c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll)
        throw std::runtime_error("Extract all runtime DLLs beside the EXE");
    load(initFn, "slInit");
    load(shutdownFn, "slShutdown");
    load(deviceFn, "slSetD3DDevice");
    load(featureFn, "slGetFeatureFunction");
    load(supportedFn, "slIsFeatureSupported");
    load(tokenFn, "slGetNewFrameToken");
    load(constantsFn, "slSetConstants");
    load(tagsFn, "slSetTagForFrame");
    load(createFactory, "CreateDXGIFactory1");
    load(createDevice, "D3D12CreateDevice");

    // V10: no IParameters ABI calls or detours. Resolve sl.common's own ctx.compute
    // storage by following the machine-code publish reference to the literal key.
    auto commonFile = (dir / L"sl.common.dll").wstring();
    commonDll = LoadLibraryExW(commonFile.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!commonDll)
        throw std::runtime_error("V10 could not load sl.common.dll");
    auto discovery = discoverCommonComputeSlot(commonDll);
    commonComputeSlot = discovery.slot;
    {
        std::ostringstream os;
        os << "sl.common direct compute slot resolved: publishRVA=0x" << std::hex << discovery.publishRva
           << ", slotRVA=0x" << discovery.slotRva;
        bootstrapStatus = os.str();
    }

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
              sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eUseDXGIFactoryProxy;
    p.engine = sl::EngineType::eCustom;
    p.engineVersion = "FrameGeneration";
    p.projectId = "e86d5d1d-b886-47ac-a90d-48708688358b";
    p.renderAPI = sl::RenderAPI::eD3D12;
    auto initResult = initFn(p, sl::kSDKVersion);
    ck(initResult, "Streamline initialization");
    initialized = true;
}
void FrameGeneration::bind(ID3D12Device* d) {
    if (!commonComputeSlot)
        throw std::runtime_error("V10 sl.common compute slot was not resolved");
    ck(deviceFn(d), "Set Streamline device");
    compute = *commonComputeSlot;
    bootstrapStatus += ", compute pointer=" + (compute ? std::string("captured") : std::string("missing"));
    if (!compute)
        throw std::runtime_error("V10 sl.common ctx.compute is null after slSetD3DDevice: " + bootstrapStatus);
}

void FrameGeneration::configure(IDXGIAdapter1 *a, UINT w, UINT h, UINT multiplier, bool vsync) {
    width = w;
    height = h;
    enabled = multiplier > 1;
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
    sl::DLSSGState state{};
    ck(stateFn(viewport, state, &opt), "DLSS-G capabilities");
    if (multiplier - 1 > state.numFramesToGenerateMax)
        throw std::runtime_error("Selected FG multiplier exceeds GPU/runtime support");
    if (vsync && state.bIsVsyncSupportAvailable != sl::Boolean::eTrue)
        throw std::runtime_error("This runtime does not support VSync with FG; disable VSync");
    sl::ReflexOptions ro{};
    ro.mode = sl::ReflexMode::eLowLatencyWithBoost;
    ck(reflexFn(ro), "Reflex On + Boost");
    opt.mode = sl::DLSSGMode::eOn;
    activeOptions = opt;

    // Cache every invariant per-frame constant once. The hot path only changes
    // the reset bit before passing the structure to Streamline.
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
    inputExtent.width = width;
    inputExtent.height = height;

    ck(optionsFn(viewport, opt), "Enable DLSS-G");
}
void FrameGeneration::begin() {
    if (!enabled)
        return;
    uint32_t frame = index++;
    activeFrame = frame;
    ck(tokenFn(token, &frame), "Frame token");
    ck(sleepFn(*token), "Reflex pacing");
}
void FrameGeneration::constants(bool reset) {
    if (!enabled)
        return;
    auto c = baseConstants;
    c.reset = reset ? sl::eTrue : sl::eFalse;
    ck(constantsFn(c, *token, viewport), "Flat-plane constants");
}
void FrameGeneration::tag(ID3D12GraphicsCommandList *l, ID3D12Resource *motion, ID3D12Resource *depth,
                          ID3D12Resource *back) {
    if (!enabled)
        return;
    const auto &e = inputExtent;
    sl::Resource m(sl::ResourceType::eTex2d, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        d(sl::ResourceType::eTex2d, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        b(sl::ResourceType::eTex2d, back, D3D12_RESOURCE_STATE_PRESENT);
    m.width = d.width = b.width = width;
    m.height = d.height = b.height = height;
    m.nativeFormat = DXGI_FORMAT_R16G16_FLOAT;
    d.nativeFormat = DXGI_FORMAT_R32_FLOAT;
    b.nativeFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    sl::ResourceTag t[] = {{&m, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &e},
                           {&d, sl::kBufferTypeDepth, sl::eValidUntilPresent, &e},
                           {&b, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent, &e}};
    ck(tagsFn(*token, viewport, t, 3, l), "DLSS-G input tags");
}
sl::DLSSGState FrameGeneration::afterPresent() {
    sl::DLSSGState s{};
    if (enabled)
        ck(stateFn(viewport, s, nullptr), "DLSS-G state");
    return s;
}
void FrameGeneration::shutdown() {
    if (initialized) {
        shutdownFn();
        initialized = false;
    }
}

void FrameGeneration::pause(bool p) {
    if (enabled) {
        auto o = activeOptions;
        if (p)
            o.mode = sl::DLSSGMode::eOff;
        ck(optionsFn(viewport, o), "Pause/resume DLSS-G");
    }
}
