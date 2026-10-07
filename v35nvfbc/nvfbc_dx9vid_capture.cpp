#include "nvfbc_cuda_capture.h"

#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <sstream>

namespace {

using u32 = std::uint32_t;
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUstream = void*;
using CUarray = void*;
using CUevent = void*;
using CUmipmappedArray = void*;
using CUexternalMemory = void*;
using CUexternalSemaphore = void*;
using CUgraphicsResource = void*;

constexpr CUresult CUDA_SUCCESS = 0;
constexpr CUresult CUDA_ERROR_NOT_READY = 600;
constexpr u32 CU_MEMORYTYPE_ARRAY = 3;
constexpr u32 CU_AD_FORMAT_UNSIGNED_INT8 = 0x01;
constexpr u32 CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE = 5;
constexpr u32 CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE = 4;
constexpr u32 CUDA_EXTERNAL_MEMORY_DEDICATED = 0x1;
constexpr u32 CU_STREAM_NON_BLOCKING = 0x1;
constexpr u32 CU_EVENT_DISABLE_TIMING = 0x2;
constexpr u32 CU_GRAPHICS_REGISTER_FLAGS_NONE = 0;

struct CUDA_MEMCPY2D {
    std::size_t srcXInBytes;
    std::size_t srcY;
    u32 srcMemoryType;
    const void* srcHost;
    unsigned long long srcDevice;
    CUarray srcArray;
    std::size_t srcPitch;
    std::size_t dstXInBytes;
    std::size_t dstY;
    u32 dstMemoryType;
    void* dstHost;
    unsigned long long dstDevice;
    CUarray dstArray;
    std::size_t dstPitch;
    std::size_t WidthInBytes;
    std::size_t Height;
};

struct CUDA_EXTERNAL_MEMORY_HANDLE_DESC {
    u32 type;
    union {
        int fd;
        struct { void* handle; const void* name; } win32;
    } handle;
    unsigned long long size;
    u32 flags;
    u32 reserved[16];
};

struct CUDA_ARRAY3D_DESCRIPTOR {
    std::size_t Width;
    std::size_t Height;
    std::size_t Depth;
    u32 Format;
    u32 NumChannels;
    u32 Flags;
};

struct CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC {
    unsigned long long offset;
    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
    u32 numLevels;
    u32 reserved[16];
};

struct CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC {
    u32 type;
    union {
        int fd;
        struct { void* handle; const void* name; } win32;
    } handle;
    u32 flags;
    u32 reserved[16];
};

struct CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS {
    struct {
        struct { unsigned long long value; } fence;
        u32 reserved[16];
    } params;
    u32 flags;
    u32 reserved[16];
};
using CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS = CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS;

struct CudaApi {
    HMODULE dll{};
    CUresult (__stdcall* init)(u32){};
    CUresult (__stdcall* d3d9CtxCreate)(CUcontext*, CUdevice*, u32, IDirect3DDevice9*){};
    CUresult (__stdcall* ctxDestroy)(CUcontext){};
    CUresult (__stdcall* streamCreate)(CUstream*, u32){};
    CUresult (__stdcall* streamDestroy)(CUstream){};
    CUresult (__stdcall* streamSynchronize)(CUstream){};
    CUresult (__stdcall* eventCreate)(CUevent*, u32){};
    CUresult (__stdcall* eventDestroy)(CUevent){};
    CUresult (__stdcall* eventRecord)(CUevent, CUstream){};
    CUresult (__stdcall* eventQuery)(CUevent){};
    CUresult (__stdcall* memcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream){};
    CUresult (__stdcall* graphicsD3D9RegisterResource)(CUgraphicsResource*, IDirect3DResource9*, u32){};
    CUresult (__stdcall* graphicsUnregisterResource)(CUgraphicsResource){};
    CUresult (__stdcall* graphicsMapResources)(u32, CUgraphicsResource*, CUstream){};
    CUresult (__stdcall* graphicsUnmapResources)(u32, CUgraphicsResource*, CUstream){};
    CUresult (__stdcall* graphicsSubResourceGetMappedArray)(CUarray*, CUgraphicsResource, u32, u32){};
    CUresult (__stdcall* importExternalMemory)(CUexternalMemory*, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC*){};
    CUresult (__stdcall* destroyExternalMemory)(CUexternalMemory){};
    CUresult (__stdcall* externalMemoryGetMappedMipmappedArray)(CUmipmappedArray*, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC*){};
    CUresult (__stdcall* mipmappedArrayGetLevel)(CUarray*, CUmipmappedArray, u32){};
    CUresult (__stdcall* mipmappedArrayDestroy)(CUmipmappedArray){};
    CUresult (__stdcall* importExternalSemaphore)(CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC*){};
    CUresult (__stdcall* destroyExternalSemaphore)(CUexternalSemaphore){};
    CUresult (__stdcall* signalExternalSemaphoresAsync)(const CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS*, u32, CUstream){};
    CUresult (__stdcall* waitExternalSemaphoresAsync)(const CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS*, u32, CUstream){};
    CUresult (__stdcall* getErrorName)(CUresult, const char**){};
    CUresult (__stdcall* getErrorString)(CUresult, const char**){};

    template<class T> bool Sym(T& out, const char* name) {
        out = reinterpret_cast<T>(GetProcAddress(dll, name));
        return out != nullptr;
    }

    bool Load(std::wstring& error) {
        dll = LoadLibraryW(L"nvcuda.dll");
        if (!dll) { error = L"nvcuda.dll could not be loaded."; return false; }
        const bool ok =
            Sym(init, "cuInit") &&
            Sym(d3d9CtxCreate, "cuD3D9CtxCreate") &&
            Sym(ctxDestroy, "cuCtxDestroy_v2") &&
            Sym(streamCreate, "cuStreamCreate") &&
            Sym(streamDestroy, "cuStreamDestroy_v2") &&
            Sym(streamSynchronize, "cuStreamSynchronize") &&
            Sym(eventCreate, "cuEventCreate") &&
            Sym(eventDestroy, "cuEventDestroy_v2") &&
            Sym(eventRecord, "cuEventRecord") &&
            Sym(eventQuery, "cuEventQuery") &&
            Sym(memcpy2DAsync, "cuMemcpy2DAsync_v2") &&
            Sym(graphicsD3D9RegisterResource, "cuGraphicsD3D9RegisterResource") &&
            Sym(graphicsUnregisterResource, "cuGraphicsUnregisterResource") &&
            Sym(graphicsMapResources, "cuGraphicsMapResources") &&
            Sym(graphicsUnmapResources, "cuGraphicsUnmapResources") &&
            Sym(graphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray") &&
            Sym(importExternalMemory, "cuImportExternalMemory") &&
            Sym(destroyExternalMemory, "cuDestroyExternalMemory") &&
            Sym(externalMemoryGetMappedMipmappedArray, "cuExternalMemoryGetMappedMipmappedArray") &&
            Sym(mipmappedArrayGetLevel, "cuMipmappedArrayGetLevel") &&
            Sym(mipmappedArrayDestroy, "cuMipmappedArrayDestroy") &&
            Sym(importExternalSemaphore, "cuImportExternalSemaphore") &&
            Sym(destroyExternalSemaphore, "cuDestroyExternalSemaphore") &&
            Sym(signalExternalSemaphoresAsync, "cuSignalExternalSemaphoresAsync") &&
            Sym(waitExternalSemaphoresAsync, "cuWaitExternalSemaphoresAsync");
        Sym(getErrorName, "cuGetErrorName");
        Sym(getErrorString, "cuGetErrorString");
        if (!ok) { error = L"The installed CUDA driver is missing a required D3D9/DX12 interoperability entry point."; return false; }
        const CUresult r = init(0);
        if (r != CUDA_SUCCESS) { error = L"cuInit failed: " + Describe(r); return false; }
        return true;
    }

    std::wstring Describe(CUresult r) const {
        const char* name = nullptr;
        const char* text = nullptr;
        if (getErrorName) getErrorName(r, &name);
        if (getErrorString) getErrorString(r, &text);
        std::ostringstream s;
        s << (name ? name : "CUDA_ERROR") << " (" << r << ")";
        if (text) s << ": " << text;
        const std::string a = s.str();
        return std::wstring(a.begin(), a.end());
    }

    void Unload() {
        if (dll) FreeLibrary(dll);
        *this = {};
    }
};

constexpr u32 NVFBC_DLL_VERSION = 0x70;
constexpr u32 NVFBC_TO_DX9_VID = 0x2003;
constexpr u32 NVFBC_GLOBAL_FLAGS_NO_INITIAL_REFRESH = 0x00000002;
constexpr int NVFBC_STATE_ENABLE = 1;
constexpr int NVFBC_SUCCESS = 0;
constexpr int NVFBC_ERROR_INVALIDATED_SESSION = -3;
constexpr int NVFBC_ERROR_PROTECTED_CONTENT = -4;
constexpr int NVFBC_ERROR_DYNAMIC_DISABLE = -20;
constexpr u32 NVFBC_TODX9VID_ARGB = 0;
constexpr int NVFBC_TODX9VID_SOURCEMODE_CROP = 2;
constexpr u32 NVFBC_TODX9VID_WAIT_WITH_TIMEOUT = 0x10;

constexpr u32 NvFbcStructVersion(std::size_t size, u32 version) {
    return static_cast<u32>(size) | (version << 16) | (NVFBC_DLL_VERSION << 24);
}

struct NvFBCStatusEx {
    u32 version;
    u32 flags;
    u32 nvfbcVersion;
    u32 adapterIdx;
    const void* privateData;
    u32 privateDataSize;
    u32 reserved[59];
    const void* reservedPtrs[31];
};
static_assert(sizeof(NvFBCStatusEx) == 512);

struct NvFBCCreateParams {
    u32 version;
    u32 interfaceType;
    u32 maxDisplayWidth;
    u32 maxDisplayHeight;
    const void* device;
    const void* privateData;
    u32 privateDataSize;
    u32 interfaceVersion;
    void* nvfbc;
    u32 adapterIdx;
    u32 nvfbcVersion;
    const void* cudaCtx;
    const void* privateData2;
    u32 privateData2Size;
    u32 reserved[55];
    const void* reservedPtrs[27];
};
static_assert(sizeof(NvFBCCreateParams) == 512);

struct NvFBCFrameGrabInfo {
    u32 width;
    u32 height;
    u32 bufferWidth;
    u32 reserved;
    int overlayActive;
    int mustRecreate;
    int firstBuffer;
    int hwMouseVisible;
    int protectedContent;
    u32 driverInternalError;
    int stereoOn;
    int igpuCapture;
    u32 sourcePid;
    u32 reserved3;
    u32 flags;
    u32 waitModeUsed;
    u32 reserved2[11];
};
static_assert(sizeof(NvFBCFrameGrabInfo) == 108);

struct NvFBCDx9OutBuffer {
    IDirect3DSurface9* primary;
    IDirect3DSurface9* secondary;
};
static_assert(sizeof(NvFBCDx9OutBuffer) == 16);

struct NvFBCDx9SetupParams {
    u32 version;
    u32 flags;
    int mode;
    u32 numBuffers;
    int diffMapBlockSize;
    int stereoFormat;
    u32 diffMapBufferSize;
    u32 classificationMapBufferSize;
    u32 classificationMapStampWidth;
    u32 classificationMapStampHeight;
    void** diffMap;
    void** classificationMap;
    NvFBCDx9OutBuffer* buffers;
    void* cursorCaptureEvent;
    u32 reserved[22];
    void* reservedPtrs[12];
};
static_assert(sizeof(NvFBCDx9SetupParams) == 256);
static_assert(offsetof(NvFBCDx9SetupParams, mode) == 8);
static_assert(offsetof(NvFBCDx9SetupParams, numBuffers) == 12);
static_assert(offsetof(NvFBCDx9SetupParams, buffers) == 56);

struct NvFBCDx9GrabParams {
    u32 version;
    u32 flags;
    u32 targetWidth;
    u32 targetHeight;
    u32 startX;
    u32 startY;
    int grabMode;
    u32 bufferIndex;
    NvFBCFrameGrabInfo* grabInfo;
    u32 waitTime;
    u32 reserved[23];
    void* reservedPtrs[15];
};
static_assert(sizeof(NvFBCDx9GrabParams) == 256);
static_assert(offsetof(NvFBCDx9GrabParams, grabInfo) == 32);
static_assert(offsetof(NvFBCDx9GrabParams, waitTime) == 40);

struct NvFbcApi {
    HMODULE dll{};
    int (__stdcall* getSdkVersion)(u32*){};
    int (__stdcall* getStatusEx)(NvFBCStatusEx*){};
    int (__stdcall* enable)(int){};
    int (__stdcall* createEx)(void*){};
    void (__stdcall* setGlobalFlags)(u32){};

    template<class T> bool Sym(T& out, const char* name) {
        out = reinterpret_cast<T>(GetProcAddress(dll, name));
        return out != nullptr;
    }

    bool Load(std::wstring& error) {
        dll = LoadLibraryW(L"NvFBC64.dll");
        if (!dll) { error = L"NvFBC64.dll was not found in the NVIDIA driver installation."; return false; }
        const bool ok = Sym(getSdkVersion, "NvFBC_GetSDKVersion") &&
                        Sym(getStatusEx, "NvFBC_GetStatusEx") &&
                        Sym(enable, "NvFBC_Enable") &&
                        Sym(createEx, "NvFBC_CreateEx") &&
                        Sym(setGlobalFlags, "NvFBC_SetGlobalFlags");
        if (!ok) { error = L"NvFBC64.dll is present but does not expose the Windows NvFBC capture ABI."; return false; }
        return true;
    }

    void Unload() {
        if (dll) FreeLibrary(dll);
        *this = {};
    }
};

using PfnSetup = int (__stdcall*)(void*, NvFBCDx9SetupParams*);
using PfnGrabFrame = int (__stdcall*)(void*, NvFBCDx9GrabParams*);
using PfnRelease = int (__stdcall*)(void*);

template<class T> T VTableSlot(void* object, std::size_t slot) {
    auto** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<T>(vtable[slot]);
}

std::wstring NvFbcResult(int r) {
    switch (r) {
    case 0: return L"NVFBC_SUCCESS";
    case -1: return L"NVFBC_ERROR_GENERIC";
    case -2: return L"NVFBC_ERROR_INVALID_PARAM";
    case -3: return L"NVFBC_ERROR_INVALIDATED_SESSION";
    case -4: return L"NVFBC_ERROR_PROTECTED_CONTENT";
    case -5: return L"NVFBC_ERROR_DRIVER_FAILURE";
    case -6: return L"NVFBC_ERROR_CUDA_FAILURE";
    case -7: return L"NVFBC_ERROR_UNSUPPORTED";
    case -9: return L"NVFBC_ERROR_INCOMPATIBLE_DRIVER";
    case -10: return L"NVFBC_ERROR_UNSUPPORTED_PLATFORM";
    case -13: return L"NVFBC_ERROR_INCOMPATIBLE_VERSION";
    case -15: return L"NVFBC_ERROR_INSUFFICIENT_PRIVILEGES";
    case -18: return L"NVFBC_ERROR_INVALID_TARGET";
    case -20: return L"NVFBC_ERROR_DYNAMIC_DISABLE";
    default: return L"NvFBC error " + std::to_wstring(r);
    }
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

struct NvFbcCudaCapture::Impl {
    CudaApi cuda;
    NvFbcApi nvfbc;
    IDirect3D9Ex* d3d9{};
    IDirect3DDevice9Ex* d3dDevice{};
    std::array<IDirect3DSurface9*, kSlots> dx9Surfaces{};
    std::array<NvFBCDx9OutBuffer, kSlots> fbcBuffers{};
    std::array<CUgraphicsResource, kSlots> graphics{};
    CUcontext cudaContext{};
    CUdevice cudaDevice{};
    CUstream stream{};
    std::array<CUevent, kSlots> stagingDone{};
    std::array<bool, kSlots> stagingBusy{};
    unsigned nextStaging{};
    std::array<CUexternalMemory, kSlots> externalMemory{};
    std::array<CUmipmappedArray, kSlots> mipmapped{};
    std::array<CUarray, kSlots> arrays{};
    CUexternalSemaphore captureSemaphore{};
    CUexternalSemaphore releaseSemaphore{};
    PfnSetup setup{};
    PfnGrabFrame grab{};
    PfnRelease release{};
    unsigned d3dAdapter{};
};

NvFbcCudaCapture::~NvFbcCudaCapture() { Shutdown(); }

bool NvFbcCudaCapture::Initialize(
    HWND target, const RECT& monitorDesktop, unsigned dxgiAdapterIndex,
    unsigned targetWidth, unsigned targetHeight, ID3D12Device* d3d12Device,
    ID3D12Resource* const slotResources[kSlots], const HANDLE slotHandles[kSlots],
    const HANDLE captureFenceHandle, const HANDLE releaseFenceHandle, std::wstring& error) {

    Shutdown();
    if (!target || !d3d12Device || !targetWidth || !targetHeight) {
        error = L"NvFBC initialization received an invalid target or DX12 resource.";
        return false;
    }

    impl_ = new Impl{};
    target_ = target;
    monitorDesktop_ = monitorDesktop;
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;

    if (!impl_->cuda.Load(error) || !impl_->nvfbc.Load(error)) { Shutdown(); return false; }

    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &impl_->d3d9);
    if (FAILED(hr) || !impl_->d3d9) {
        error = L"Direct3DCreate9Ex failed for the NvFBCToDx9Vid GPU surface path.";
        Shutdown(); return false;
    }

    UINT chosenAdapter = D3DADAPTER_DEFAULT;
    const UINT adapterCount = impl_->d3d9->GetAdapterCount();
    for (UINT i = 0; i < adapterCount; ++i) {
        HMONITOR hm = impl_->d3d9->GetAdapterMonitor(i);
        if (!hm) continue;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hm, &mi) && SameRect(mi.rcMonitor, monitorDesktop)) {
            chosenAdapter = i;
            break;
        }
    }
    impl_->d3dAdapter = chosenAdapter;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.BackBufferWidth = std::max(1u, targetWidth);
    pp.BackBufferHeight = std::max(1u, targetHeight);
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetDesktopWindow();
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = impl_->d3d9->CreateDeviceEx(chosenAdapter, D3DDEVTYPE_HAL, GetDesktopWindow(),
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &pp, nullptr, &impl_->d3dDevice);
    if (FAILED(hr) || !impl_->d3dDevice) {
        error = L"Could not create the D3D9Ex device required by NvFBCToDx9Vid.";
        Shutdown(); return false;
    }

    CUresult cu = impl_->cuda.d3d9CtxCreate(&impl_->cudaContext, &impl_->cudaDevice, 0, impl_->d3dDevice);
    if (cu != CUDA_SUCCESS || !impl_->cudaContext) {
        error = L"cuD3D9CtxCreate failed for NvFBCToDx9Vid interop: " + impl_->cuda.Describe(cu);
        Shutdown(); return false;
    }

    cu = impl_->cuda.streamCreate(&impl_->stream, CU_STREAM_NON_BLOCKING);
    if (cu != CUDA_SUCCESS) {
        error = L"CUDA bridge stream creation failed: " + impl_->cuda.Describe(cu);
        Shutdown(); return false;
    }

    for (unsigned i = 0; i < kSlots; ++i) {
        hr = impl_->d3dDevice->CreateOffscreenPlainSurface(targetWidth, targetHeight,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &impl_->dx9Surfaces[i], nullptr);
        if (FAILED(hr) || !impl_->dx9Surfaces[i]) {
            error = L"Could not allocate an NvFBCToDx9Vid GPU output surface.";
            Shutdown(); return false;
        }
        impl_->fbcBuffers[i].primary = impl_->dx9Surfaces[i];
        impl_->fbcBuffers[i].secondary = nullptr;

        cu = impl_->cuda.graphicsD3D9RegisterResource(&impl_->graphics[i], impl_->dx9Surfaces[i], CU_GRAPHICS_REGISTER_FLAGS_NONE);
        if (cu != CUDA_SUCCESS) {
            error = L"CUDA could not register an NvFBC D3D9 surface: " + impl_->cuda.Describe(cu);
            Shutdown(); return false;
        }
        cu = impl_->cuda.eventCreate(&impl_->stagingDone[i], CU_EVENT_DISABLE_TIMING);
        if (cu != CUDA_SUCCESS) {
            error = L"CUDA DX9 staging event creation failed: " + impl_->cuda.Describe(cu);
            Shutdown(); return false;
        }
    }

    u32 sdkVersion = 0;
    const int sdkResult = impl_->nvfbc.getSdkVersion(&sdkVersion);
    if (sdkResult != NVFBC_SUCCESS) {
        error = L"NvFBC_GetSDKVersion failed: " + NvFbcResult(sdkResult);
        Shutdown(); return false;
    }

    auto queryStatus = [&](NvFBCStatusEx& out) -> int {
        out = {};
        out.version = NvFbcStructVersion(sizeof(out), 2);
        out.adapterIdx = dxgiAdapterIndex;
        int r = impl_->nvfbc.getStatusEx(&out);
        if (r != NVFBC_SUCCESS) {
            out = {};
            out.version = NvFbcStructVersion(sizeof(out), 1);
            out.adapterIdx = dxgiAdapterIndex;
            r = impl_->nvfbc.getStatusEx(&out);
        }
        return r;
    };

    NvFBCStatusEx statusBefore{};
    const int statusBeforeResult = queryStatus(statusBefore);
    const bool captureBefore = statusBeforeResult == NVFBC_SUCCESS && ((statusBefore.flags & 1u) != 0u);
    const int enableResult = impl_->nvfbc.enable(NVFBC_STATE_ENABLE);
    NvFBCStatusEx statusAfter{};
    const int statusAfterResult = queryStatus(statusAfter);
    const bool captureAfter = statusAfterResult == NVFBC_SUCCESS && ((statusAfter.flags & 1u) != 0u);

    impl_->nvfbc.setGlobalFlags(NVFBC_GLOBAL_FLAGS_NO_INITIAL_REFRESH);

    NvFBCCreateParams create{};
    create.version = NvFbcStructVersion(sizeof(create), 2);
    create.interfaceType = NVFBC_TO_DX9_VID;
    create.device = impl_->d3dDevice;
    create.adapterIdx = dxgiAdapterIndex;
    int cr = impl_->nvfbc.createEx(&create);
    if (cr != NVFBC_SUCCESS) {
        create = {};
        create.version = NvFbcStructVersion(sizeof(create), 1);
        create.interfaceType = NVFBC_TO_DX9_VID;
        create.device = impl_->d3dDevice;
        create.adapterIdx = dxgiAdapterIndex;
        cr = impl_->nvfbc.createEx(&create);
    }
    if (cr != NVFBC_SUCCESS || !create.nvfbc) {
        std::wostringstream diag;
        diag << L"NvFBC_CreateEx(NvFBCToDx9Vid) failed: " << NvFbcResult(cr)
             << L" | sdk=0x" << std::hex << sdkVersion
             << L" | statusBefore=" << NvFbcResult(statusBeforeResult)
             << L" flagsBefore=0x" << statusBefore.flags
             << L" captureBitBefore=" << (captureBefore ? 1 : 0)
             << L" | enable=" << NvFbcResult(enableResult)
             << L" | statusAfter=" << NvFbcResult(statusAfterResult)
             << L" flagsAfter=0x" << statusAfter.flags
             << L" captureBitAfter=" << (captureAfter ? 1 : 0)
             << L" | dxgiAdapter=" << std::dec << dxgiAdapterIndex
             << L" d3d9Adapter=" << impl_->d3dAdapter;
        error = diag.str();
        Shutdown(); return false;
    }

    nvfbcObject_ = create.nvfbc;
    desktopWidth_ = create.maxDisplayWidth;
    desktopHeight_ = create.maxDisplayHeight;

    impl_->setup = VTableSlot<PfnSetup>(nvfbcObject_, 0);
    impl_->grab = VTableSlot<PfnGrabFrame>(nvfbcObject_, 1);
    impl_->release = VTableSlot<PfnRelease>(nvfbcObject_, 3);
    if (!impl_->setup || !impl_->grab || !impl_->release) {
        error = L"NvFBCToDx9Vid returned an incomplete interface.";
        Shutdown(); return false;
    }

    NvFBCDx9SetupParams setup{};
    setup.version = NvFbcStructVersion(sizeof(setup), 3);
    setup.flags = 0;
    setup.mode = static_cast<int>(NVFBC_TODX9VID_ARGB);
    setup.numBuffers = kSlots;
    setup.buffers = impl_->fbcBuffers.data();
    const int setupResult = impl_->setup(nvfbcObject_, &setup);
    if (setupResult != NVFBC_SUCCESS) {
        error = L"NvFBCToDx9VidSetUp(v3, ARGB, 3 GPU surfaces) failed: " + NvFbcResult(setupResult);
        Shutdown(); return false;
    }

    for (unsigned i = 0; i < kSlots; ++i) {
        if (!slotResources[i] || !slotHandles[i]) {
            error = L"DX12 capture slot handle is missing.";
            Shutdown(); return false;
        }
        const D3D12_RESOURCE_DESC rd = slotResources[i]->GetDesc();
        const D3D12_RESOURCE_ALLOCATION_INFO ai = d3d12Device->GetResourceAllocationInfo(0, 1, &rd);
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC hd{};
        hd.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
        hd.handle.win32.handle = slotHandles[i];
        hd.size = ai.SizeInBytes;
        hd.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
        cu = impl_->cuda.importExternalMemory(&impl_->externalMemory[i], &hd);
        if (cu != CUDA_SUCCESS) {
            error = L"CUDA could not import DX12 capture texture: " + impl_->cuda.Describe(cu);
            Shutdown(); return false;
        }

        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC md{};
        md.arrayDesc.Width = rd.Width;
        md.arrayDesc.Height = rd.Height;
        md.arrayDesc.Depth = 0;
        md.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
        md.arrayDesc.NumChannels = 4;
        md.arrayDesc.Flags = 0;
        md.numLevels = 1;
        cu = impl_->cuda.externalMemoryGetMappedMipmappedArray(&impl_->mipmapped[i], impl_->externalMemory[i], &md);
        if (cu != CUDA_SUCCESS) {
            error = L"CUDA could not map the DX12 capture texture as an array: " + impl_->cuda.Describe(cu);
            Shutdown(); return false;
        }
        cu = impl_->cuda.mipmappedArrayGetLevel(&impl_->arrays[i], impl_->mipmapped[i], 0);
        if (cu != CUDA_SUCCESS) {
            error = L"CUDA could not get the DX12 texture array level: " + impl_->cuda.Describe(cu);
            Shutdown(); return false;
        }
    }

    auto importFence = [&](HANDLE h, CUexternalSemaphore& out, const wchar_t* which) -> bool {
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC sd{};
        sd.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE;
        sd.handle.win32.handle = h;
        const CUresult rr = impl_->cuda.importExternalSemaphore(&out, &sd);
        if (rr != CUDA_SUCCESS) {
            error = std::wstring(L"CUDA could not import the ") + which + L" fence: " + impl_->cuda.Describe(rr);
            return false;
        }
        return true;
    };
    if (!importFence(captureFenceHandle, impl_->captureSemaphore, L"capture-ready") ||
        !importFence(releaseFenceHandle, impl_->releaseSemaphore, L"release")) {
        Shutdown(); return false;
    }

    return true;
}

NvFbcCudaCapture::GrabResult NvFbcCudaCapture::GrabToSlot(
    unsigned slot, std::uint64_t releaseFenceValue, std::uint64_t captureFenceValue,
    LARGE_INTEGER& sourceTimestamp, std::wstring& error) {

    if (!impl_ || !nvfbcObject_ || slot >= kSlots) {
        error = L"NvFBC capture is not initialized.";
        return GrabResult::Fatal;
    }

    unsigned staging = kSlots;
    for (unsigned n = 0; n < kSlots; ++n) {
        const unsigned candidate = (impl_->nextStaging + n) % kSlots;
        if (!impl_->stagingBusy[candidate]) {
            staging = candidate;
            break;
        }
        const CUresult q = impl_->cuda.eventQuery(impl_->stagingDone[candidate]);
        if (q == CUDA_SUCCESS) {
            impl_->stagingBusy[candidate] = false;
            staging = candidate;
            break;
        }
        if (q != CUDA_ERROR_NOT_READY) {
            error = L"CUDA DX9 staging completion query failed: " + impl_->cuda.Describe(q);
            return GrabResult::Fatal;
        }
    }
    if (staging == kSlots) return GrabResult::Timeout;
    impl_->nextStaging = (staging + 1) % kSlots;

    RECT client{};
    if (!GetClientRect(target_, &client)) {
        error = L"The source window closed during NvFBC capture.";
        return GrabResult::Fatal;
    }
    POINT tl{client.left, client.top};
    POINT br{client.right, client.bottom};
    if (!ClientToScreen(target_, &tl) || !ClientToScreen(target_, &br)) return GrabResult::Timeout;
    RECT screenClient{tl.x, tl.y, br.x, br.y};
    const long currentW = screenClient.right - screenClient.left;
    const long currentH = screenClient.bottom - screenClient.top;
    if (currentW != static_cast<long>(targetWidth_) || currentH != static_cast<long>(targetHeight_)) {
        error = L"The source client size changed. Press Start again to rebuild the NvFBC/DLSS resources.";
        return GrabResult::Reinitialize;
    }

    const long cropX = screenClient.left - monitorDesktop_.left;
    const long cropY = screenClient.top - monitorDesktop_.top;
    const long monitorW = monitorDesktop_.right - monitorDesktop_.left;
    const long monitorH = monitorDesktop_.bottom - monitorDesktop_.top;
    if (cropX < 0 || cropY < 0 ||
        cropX + static_cast<long>(targetWidth_) > monitorW ||
        cropY + static_cast<long>(targetHeight_) > monitorH) {
        error = L"The source window moved outside the NvFBC capture surface. Press Start again on the target monitor.";
        return GrabResult::Reinitialize;
    }

    NvFBCFrameGrabInfo info{};
    NvFBCDx9GrabParams grab{};
    grab.version = NvFbcStructVersion(sizeof(grab), 1);
    grab.flags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
    grab.targetWidth = targetWidth_;
    grab.targetHeight = targetHeight_;
    grab.startX = static_cast<u32>(cropX);
    grab.startY = static_cast<u32>(cropY);
    grab.grabMode = NVFBC_TODX9VID_SOURCEMODE_CROP;
    grab.bufferIndex = staging;
    grab.grabInfo = &info;
    grab.waitTime = 16;

    const int r = impl_->grab(nvfbcObject_, &grab);
    if (r != NVFBC_SUCCESS) {
        if (r == NVFBC_ERROR_INVALIDATED_SESSION || info.mustRecreate) return GrabResult::Reinitialize;
        if (r == NVFBC_ERROR_PROTECTED_CONTENT) {
            error = L"NvFBC blocked capture because protected content is active.";
            return GrabResult::Timeout;
        }
        if (r == NVFBC_ERROR_DYNAMIC_DISABLE) {
            error = L"NvFBC was dynamically disabled by the NVIDIA driver.";
            return GrabResult::Fatal;
        }
        if (!info.width && !info.height && !info.driverInternalError) return GrabResult::Timeout;
        error = L"NvFBCToDx9VidGrabFrame failed: " + NvFbcResult(r);
        return GrabResult::Fatal;
    }
    if (info.mustRecreate) return GrabResult::Reinitialize;

    if (releaseFenceValue) {
        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wp{};
        wp.params.fence.value = releaseFenceValue;
        const CUexternalSemaphore sem = impl_->releaseSemaphore;
        CUresult cu = impl_->cuda.waitExternalSemaphoresAsync(&sem, &wp, 1, impl_->stream);
        if (cu != CUDA_SUCCESS) {
            error = L"CUDA release-fence wait failed: " + impl_->cuda.Describe(cu);
            return GrabResult::Fatal;
        }
    }

    CUgraphicsResource resource = impl_->graphics[staging];
    CUresult cu = impl_->cuda.graphicsMapResources(1, &resource, impl_->stream);
    if (cu != CUDA_SUCCESS) {
        error = L"CUDA could not map the NvFBC D3D9 surface: " + impl_->cuda.Describe(cu);
        return GrabResult::Fatal;
    }

    CUarray srcArray{};
    cu = impl_->cuda.graphicsSubResourceGetMappedArray(&srcArray, resource, 0, 0);
    if (cu != CUDA_SUCCESS || !srcArray) {
        impl_->cuda.graphicsUnmapResources(1, &resource, impl_->stream);
        error = L"CUDA could not expose the NvFBC D3D9 surface as an array: " + impl_->cuda.Describe(cu);
        return GrabResult::Fatal;
    }

    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = srcArray;
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = impl_->arrays[slot];
    copy.WidthInBytes = static_cast<std::size_t>(targetWidth_) * 4u;
    copy.Height = targetHeight_;
    cu = impl_->cuda.memcpy2DAsync(&copy, impl_->stream);
    if (cu != CUDA_SUCCESS) {
        impl_->cuda.graphicsUnmapResources(1, &resource, impl_->stream);
        error = L"CUDA NvFBC DX9Vid-to-DX12 copy failed: " + impl_->cuda.Describe(cu);
        return GrabResult::Fatal;
    }

    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sp{};
    sp.params.fence.value = captureFenceValue;
    const CUexternalSemaphore sem = impl_->captureSemaphore;
    cu = impl_->cuda.signalExternalSemaphoresAsync(&sem, &sp, 1, impl_->stream);
    if (cu != CUDA_SUCCESS) {
        impl_->cuda.graphicsUnmapResources(1, &resource, impl_->stream);
        error = L"CUDA capture-fence signal failed: " + impl_->cuda.Describe(cu);
        return GrabResult::Fatal;
    }

    cu = impl_->cuda.graphicsUnmapResources(1, &resource, impl_->stream);
    if (cu != CUDA_SUCCESS) {
        error = L"CUDA could not unmap the NvFBC D3D9 surface: " + impl_->cuda.Describe(cu);
        return GrabResult::Fatal;
    }
    cu = impl_->cuda.eventRecord(impl_->stagingDone[staging], impl_->stream);
    if (cu != CUDA_SUCCESS) {
        error = L"CUDA DX9 staging completion record failed: " + impl_->cuda.Describe(cu);
        return GrabResult::Fatal;
    }
    impl_->stagingBusy[staging] = true;

    QueryPerformanceCounter(&sourceTimestamp);
    if (info.width) desktopWidth_ = info.width;
    if (info.height) desktopHeight_ = info.height;
    return GrabResult::Frame;
}

void NvFbcCudaCapture::Shutdown() {
    if (!impl_) { nvfbcObject_ = nullptr; return; }

    if (impl_->stream && impl_->cuda.streamSynchronize) impl_->cuda.streamSynchronize(impl_->stream);

    if (nvfbcObject_ && impl_->release) impl_->release(nvfbcObject_);
    nvfbcObject_ = nullptr;

    if (impl_->captureSemaphore && impl_->cuda.destroyExternalSemaphore)
        impl_->cuda.destroyExternalSemaphore(impl_->captureSemaphore);
    if (impl_->releaseSemaphore && impl_->cuda.destroyExternalSemaphore)
        impl_->cuda.destroyExternalSemaphore(impl_->releaseSemaphore);
    impl_->captureSemaphore = nullptr;
    impl_->releaseSemaphore = nullptr;

    for (unsigned i = 0; i < kSlots; ++i) {
        if (impl_->mipmapped[i] && impl_->cuda.mipmappedArrayDestroy)
            impl_->cuda.mipmappedArrayDestroy(impl_->mipmapped[i]);
        impl_->mipmapped[i] = nullptr;
        impl_->arrays[i] = nullptr;
        if (impl_->externalMemory[i] && impl_->cuda.destroyExternalMemory)
            impl_->cuda.destroyExternalMemory(impl_->externalMemory[i]);
        impl_->externalMemory[i] = nullptr;

        if (impl_->graphics[i] && impl_->cuda.graphicsUnregisterResource)
            impl_->cuda.graphicsUnregisterResource(impl_->graphics[i]);
        impl_->graphics[i] = nullptr;

        if (impl_->stagingDone[i] && impl_->cuda.eventDestroy)
            impl_->cuda.eventDestroy(impl_->stagingDone[i]);
        impl_->stagingDone[i] = nullptr;
        impl_->stagingBusy[i] = false;
    }

    if (impl_->stream && impl_->cuda.streamDestroy) impl_->cuda.streamDestroy(impl_->stream);
    impl_->stream = nullptr;

    if (impl_->cudaContext && impl_->cuda.ctxDestroy) impl_->cuda.ctxDestroy(impl_->cudaContext);
    impl_->cudaContext = nullptr;

    for (auto& surface : impl_->dx9Surfaces) {
        if (surface) surface->Release();
        surface = nullptr;
    }
    if (impl_->d3dDevice) impl_->d3dDevice->Release();
    impl_->d3dDevice = nullptr;
    if (impl_->d3d9) impl_->d3d9->Release();
    impl_->d3d9 = nullptr;

    impl_->nvfbc.Unload();
    impl_->cuda.Unload();
    delete impl_;
    impl_ = nullptr;
    target_ = nullptr;
    targetWidth_ = targetHeight_ = desktopWidth_ = desktopHeight_ = 0;
}
