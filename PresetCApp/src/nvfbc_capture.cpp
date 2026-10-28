#include "nvfbc_capture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {
using NvU32 = unsigned long;
using NVFBCRESULT = int;

std::mutex gNvFbcDiagMutex;
DWORD gNvFbcDiagLastSourcePid = 0xffffffffu;
bool gNvFbcDiagFrameLogged = false;

void NvFbcDiagLog(const std::wstring &message) {
    std::lock_guard<std::mutex> guard(gNvFbcDiagMutex);
    wchar_t exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path =
        (n && n < MAX_PATH) ? std::wstring(exePath, n) : std::wstring(L"FrameGeneration.exe");
    const size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"FrameGeneration-Capture.log";
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    const std::wstring line = std::wstring(prefix) + message + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0)
        return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
}
constexpr NVFBCRESULT NVFBC_SUCCESS = 0;
constexpr NVFBCRESULT NVFBC_ERROR_INVALIDATED_SESSION = -3;
constexpr NVFBCRESULT NVFBC_ERROR_PROTECTED_CONTENT = -4;
constexpr NvU32 NVFBC_TO_DX9_VID_V2 = 0x2002;
constexpr NvU32 NVFBC_TO_DX9_VID_V3 = 0x2003;
constexpr DWORD kGrabTimeoutMs = 100;
constexpr NvU32 kCompatPrivateData[4] = {0xaef57ac5u, 0x401d1a39u, 0x1b856bbeu, 0x9ed0cebau};

// Current community Windows NvFBC code uses the frozen 0x70 ABI generation for
// the struct version high byte even when NvFBC_GetSDKVersion reports a newer DLL.
constexpr NvU32 StructVersion(size_t size, NvU32 revision) {
    return static_cast<NvU32>(size) | (revision << 16) | (0x70u << 24);
}

struct NvFbcStatusEx {
    NvU32 dwVersion;
    NvU32 bIsCapturePossible : 1;
    NvU32 bCurrentlyCapturing : 1;
    NvU32 bCanCreateNow : 1;
    NvU32 bSupportMultiHead : 1;
    NvU32 bSupportConfigurableDiffMap : 1;
    NvU32 bSupportImageClassification : 1;
    NvU32 bReservedBits : 26;
    NvU32 dwNvFBCVersion;
    NvU32 dwAdapterIdx;
    void *pPrivateData;
    NvU32 dwPrivateDataSize;
    NvU32 dwReserved[59];
    void *pReserved[31];
};

struct NvFbcCreateParams {
    NvU32 dwVersion;
    NvU32 dwInterfaceType;
    NvU32 dwMaxDisplayWidth;
    NvU32 dwMaxDisplayHeight;
    void *pDevice;
    void *pPrivateData;
    NvU32 dwPrivateDataSize;
    NvU32 dwInterfaceVersion;
    void *pNvFBC;
    NvU32 dwAdapterIdx;
    NvU32 dwNvFBCVersion;
    void *cudaCtx;
    void *pPrivateData2;
    NvU32 dwPrivateData2Size;
    NvU32 dwReserved[55];
    void *pReserved[27];
};

struct NvFbcFrameGrabInfo {
    DWORD dwWidth;
    DWORD dwHeight;
    DWORD dwBufferWidth;
    DWORD dwReserved;
    BOOL bOverlayActive;
    BOOL bMustRecreate;
    BOOL bFirstBuffer;
    BOOL bHWMouseVisible;
    BOOL bProtectedContent;
    DWORD dwDriverInternalError;
    BOOL bStereoOn;
    BOOL bIGPUCapture;
    DWORD dwSourcePID;
    DWORD dwReserved3;
    DWORD bIsHDR : 1;
    DWORD bReservedBit1 : 1;
    DWORD bReservedBits : 30;
    DWORD dwWaitModeUsed;
    NvU32 dwReserved2[11];
};

// Legacy Windows NvFBC CUDA ABI from the same 2018 SDK generation that uses
// NVFBC_TO_DX9_VID 0x2003.  The CUDA driver API is runtime-loaded: the
// final application does not require a CUDA Toolkit install, only nvcuda.dll
// supplied by the NVIDIA display driver.
constexpr NvU32 NVFBC_SHARED_CUDA_V3 = 0x1007;
constexpr NvU32 NVFBC_SHARED_CUDA_V2 = 0x1006;
constexpr NvU32 NVFBC_TOCUDA_ARGB = 0;
constexpr NvU32 NVFBC_TOCUDA_WAIT_WITH_TIMEOUT = 0x10;

using CUresult = int;
using CUdevice = int;
using CUcontext = void *;
using CUdeviceptr = unsigned long long;
constexpr CUresult CUDA_SUCCESS = 0;


struct ExternalMemoryDesc {
    int type; struct { void *handle; const void *name; } win32;
    unsigned long long size; unsigned flags; unsigned reserved[16];
};
struct ExternalBufferDesc { unsigned long long offset, size; unsigned flags, reserved[16]; };
struct ExternalSemaphoreDesc {
    int type; struct { void *handle; const void *name; } win32;
    unsigned flags, reserved[16];
};
struct ExternalSignal {
    struct { unsigned long long value, nvSciSync, keyedMutex; unsigned reserved[12]; } params;
    unsigned flags, reserved[16];
};
static_assert(sizeof(ExternalMemoryDesc)==104 && offsetof(ExternalMemoryDesc,size)==24);
static_assert(sizeof(ExternalBufferDesc)==88);
static_assert(sizeof(ExternalSemaphoreDesc)==96);
static_assert(sizeof(ExternalSignal)==144 && offsetof(ExternalSignal,flags)==72);

struct CudaDriverApi {
    using InitFn = CUresult(WINAPI *)(unsigned int);
    using D3D9CtxCreateFn = CUresult(WINAPI *)(CUcontext *, CUdevice *, unsigned int, IDirect3DDevice9 *);
    using CtxDestroyFn = CUresult(WINAPI *)(CUcontext);
    using CtxGetCurrentFn = CUresult(WINAPI *)(CUcontext *);
    using CtxSetCurrentFn = CUresult(WINAPI *)(CUcontext);
    using MemAllocFn = CUresult(WINAPI *)(CUdeviceptr *, size_t);
    using MemFreeFn = CUresult(WINAPI *)(CUdeviceptr);

    using ImportMemoryFn = CUresult(WINAPI *)(void **, const ExternalMemoryDesc *);
    using MapBufferFn = CUresult(WINAPI *)(CUdeviceptr *, void *, const ExternalBufferDesc *);
    using DestroyExternalFn = CUresult(WINAPI *)(void *);
    using ImportSemaphoreFn = CUresult(WINAPI *)(void **, const ExternalSemaphoreDesc *);
    using SignalFn = CUresult(WINAPI *)(const void *const *, const ExternalSignal *, unsigned, void *);
    using DeviceLuidFn = CUresult(WINAPI *)(char *, unsigned *, CUdevice);
    ImportMemoryFn importMemory{}; MapBufferFn mapBuffer{};
    DestroyExternalFn destroyMemory{}, destroySemaphore{};
    ImportSemaphoreFn importSemaphore{}; SignalFn signal{}; DeviceLuidFn deviceLuid{};
    HMODULE module{};
    InitFn init{};
    D3D9CtxCreateFn d3d9CtxCreate{};
    CtxDestroyFn ctxDestroy{};
    CtxGetCurrentFn ctxGetCurrent{};
    CtxSetCurrentFn ctxSetCurrent{};
    MemAllocFn memAlloc{};
    MemFreeFn memFree{};

    template <typename T>
    bool Resolve(T &dst, const char *primary) {
        dst = reinterpret_cast<T>(GetProcAddress(module, primary));
        return dst != nullptr;
    }

    bool Load(std::wstring &error) {
        module = LoadLibraryW(L"nvcuda.dll");
        if (!module) {
            error = L"nvcuda.dll could not be loaded (Win32=" + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (!Resolve(init, "cuInit") ||
            !Resolve(d3d9CtxCreate, "cuD3D9CtxCreate_v2") ||
            !Resolve(ctxDestroy, "cuCtxDestroy_v2") ||
            !Resolve(ctxGetCurrent, "cuCtxGetCurrent") ||
            !Resolve(ctxSetCurrent, "cuCtxSetCurrent") ||
            !Resolve(memAlloc, "cuMemAlloc_v2") ||
            !Resolve(memFree, "cuMemFree_v2") ||
            !Resolve(importMemory,"cuImportExternalMemory") ||
            !Resolve(mapBuffer,"cuExternalMemoryGetMappedBuffer") ||
            !Resolve(destroyMemory,"cuDestroyExternalMemory") ||
            !Resolve(importSemaphore,"cuImportExternalSemaphore") ||
            !Resolve(destroySemaphore,"cuDestroyExternalSemaphore") ||
            !Resolve(signal,"cuSignalExternalSemaphoresAsync") ||
            !Resolve(deviceLuid,"cuDeviceGetLuid")) {
            error = L"nvcuda.dll is missing one or more required Driver API entry points";
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }

    ~CudaDriverApi() {
        if (module)
            FreeLibrary(module);
    }
};

struct NvFbcCudaSetupV3 {
    NvU32 dwVersion;
    NvU32 bEnableSeparateCursorCapture : 1;
    NvU32 bHDRRequest : 1;
    NvU32 bReserved : 30;
    void *hCursorCaptureEvent;
    NvU32 eFormat;
    NvU32 dwReserved[61];
    void *pReserved[31];
};

struct NvFbcCudaSetupV2 {
    NvU32 dwVersion;
    NvU32 bEnableSeparateCursorCapture : 1;
    NvU32 bReserved : 31;
    void *hCursorCaptureEvent;
    NvU32 dwReserved[62];
    void *pReserved[31];
};

struct NvFbcCudaGrabV1 {
    NvU32 dwVersion;
    NvU32 dwFlags;
    void *pCUDADeviceBuffer;
    NvFbcFrameGrabInfo *pNvFBCFrameGrabInfo;
    NvU32 dwWaitTime;
    NvU32 dwReserved[61];
    void *pReserved[30];
};

static_assert(sizeof(void *) == 8, "NvFBC backend is x64-only");
static_assert(sizeof(NvFbcStatusEx) == 512, "NvFBC status ABI size mismatch");
static_assert(sizeof(NvFbcCreateParams) == 512, "NvFBC create ABI size mismatch");
static_assert(sizeof(NvFbcFrameGrabInfo) == 108, "NvFBC frame info ABI size mismatch");
static_assert(sizeof(NvFbcCudaSetupV3) == 512, "NvFBC CUDA V3 setup ABI size mismatch");
static_assert(sizeof(NvFbcCudaSetupV2) == 512, "NvFBC CUDA V2 setup ABI size mismatch");
static_assert(sizeof(NvFbcCudaGrabV1) == 512, "NvFBC CUDA grab ABI size mismatch");

// Recovered from NVIDIA ShadowPlay / _nvspcaps64.dll 3.14.0.139.
// NvFBC_CreateEx pPrivateData2 points to this 0xC8-byte block when PID capture is requested.
struct ShadowPlayPidCreatePrivate {
    NvU32 dwVersion;
    NvU32 enable;
    NvU32 pid;
    NvU32 reserved[47];
};
static_assert(sizeof(ShadowPlayPidCreatePrivate) == 0xC8, "ShadowPlay PID create-private size mismatch");

// NvFBC_GetStatusEx pPrivateData block used by ShadowPlay to query PID-session support.
struct ShadowPlayPidStatusPrivate {
    NvU32 dwVersion;
    NvU32 reserved0[2];
    NvU32 flags; // bit0 PID supported, bit1 PID-session creatable, bit2 concurrency
    NvU32 reserved[44];
};
static_assert(sizeof(ShadowPlayPidStatusPrivate) == 0xC0, "ShadowPlay PID status-private size mismatch");

std::wstring HresultText(HRESULT hr) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
    return buf;
}
} // namespace

struct NvFbcCudaSession {
    virtual NVFBCRESULT __stdcall NvFBCCudaGetMaxBufferSize(NvU32 *) = 0;
    virtual NVFBCRESULT __stdcall NvFBCCudaSetup(void *) = 0;
    virtual NVFBCRESULT __stdcall NvFBCCudaGrabFrame(NvFbcCudaGrabV1 *) = 0;
    virtual NVFBCRESULT __stdcall NvFBCCudaGPUBasedCPUSleep(long long) = 0;
    virtual NVFBCRESULT __stdcall NvFBCCudaCursorCapture(void *) = 0;
    virtual NVFBCRESULT __stdcall NvFBCCudaRelease() = 0;
};

struct NvFbcCaptureBackend::CudaState {
    CudaDriverApi api;
    CUcontext context{}; CUdevice device{};
    NvFbcCudaSession *session{};
    NvU32 interfaceId{}, maxBytes{};
    void *semaphore{};
    std::array<void *, 4> memory{};
    std::array<CUdeviceptr, 4> ptr{};
    UINT pitch{};
    ~CudaState() {
        if (!context) return;
        api.ctxSetCurrent(context);
        if (session) session->NvFBCCudaRelease();
        if (semaphore) api.destroySemaphore(semaphore);
        for (unsigned i=0;i<4;++i) {
            if (ptr[i]) api.memFree(ptr[i]);
            if (memory[i]) api.destroyMemory(memory[i]);
        }
        api.ctxDestroy(context);
    }
};
NvFbcCaptureBackend::NvFbcCaptureBackend() = default;
static bool cuOk(CUresult r, const wchar_t *op, std::wstring &error) {
    if (!r) return true;
    error=std::wstring(op)+L" failed: CUDA "+std::to_wstring(r); return false;
}
NvFbcCaptureBackend::~NvFbcCaptureBackend() {
    Shutdown();
}

std::wstring NvFbcCaptureBackend::ResultText(int r) const {
    const wchar_t *name = L"NVFBC_ERROR_UNKNOWN";
    switch (r) {
    case 0:
        name = L"NVFBC_SUCCESS";
        break;
    case -1:
        name = L"NVFBC_ERROR_GENERIC";
        break;
    case -2:
        name = L"NVFBC_ERROR_INVALID_PARAM";
        break;
    case -3:
        name = L"NVFBC_ERROR_INVALIDATED_SESSION";
        break;
    case -4:
        name = L"NVFBC_ERROR_PROTECTED_CONTENT";
        break;
    case -5:
        name = L"NVFBC_ERROR_DRIVER_FAILURE";
        break;
    case -7:
        name = L"NVFBC_ERROR_UNSUPPORTED";
        break;
    case -9:
        name = L"NVFBC_ERROR_INCOMPATIBLE_DRIVER";
        break;
    case -10:
        name = L"NVFBC_ERROR_UNSUPPORTED_PLATFORM";
        break;
    case -13:
        name = L"NVFBC_ERROR_INCOMPATIBLE_VERSION";
        break;
    case -15:
        name = L"NVFBC_ERROR_INSUFFICIENT_PRIVILEGES";
        break;
    case -16:
        name = L"NVFBC_ERROR_INVALID_CALL";
        break;
    case -17:
        name = L"NVFBC_ERROR_SYSTEM_ERROR";
        break;
    case -20:
        name = L"NVFBC_ERROR_DYNAMIC_DISABLE";
        break;
    default:
        break;
    }
    return std::wstring(name) + L" (" + std::to_wstring(r) + L")";
}

bool NvFbcCaptureBackend::LoadApi(std::wstring &error) {
    wchar_t systemDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDir, MAX_PATH)) {
        error = L"GetSystemDirectoryW failed.";
        return false;
    }
    const std::wstring path = std::wstring(systemDir) + L"\\NvFBC64.dll";
    module_ = LoadLibraryW(path.c_str());
    if (!module_) {
        error = L"NvFBC64.dll is not present in System32 (Win32 " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    getSdkVersion_ = reinterpret_cast<GetSdkVersionFn>(GetProcAddress(module_, "NvFBC_GetSDKVersion"));
    getStatus_ = reinterpret_cast<GetStatusFn>(GetProcAddress(module_, "NvFBC_GetStatusEx"));
    create_ = reinterpret_cast<CreateFn>(GetProcAddress(module_, "NvFBC_CreateEx"));
    enable_ = reinterpret_cast<EnableFn>(GetProcAddress(module_, "NvFBC_Enable"));
    if (!getStatus_ || !create_) {
        error = L"NvFBC64.dll is missing NvFBC_GetStatusEx or NvFBC_CreateEx.";
        return false;
    }
    NvU32 sdk = 0;
    if (getSdkVersion_ && getSdkVersion_(&sdk) == NVFBC_SUCCESS)
        sdkVersion_ = sdk;
    return true;
}

bool NvFbcCaptureBackend::ReadStatus(bool &possible, bool &canCreateNow, std::wstring &error) {
    NvFbcStatusEx status{};
    ShadowPlayPidStatusPrivate pidStatus{};
    status.dwVersion = StructVersion(sizeof(status), 2);
    status.dwAdapterIdx = adapterOrdinal_;
    pidStatus.dwVersion = StructVersion(sizeof(pidStatus), 1); // 0x700100C0 current-ABI form.
    status.pPrivateData = &pidStatus;
    status.dwPrivateDataSize = sizeof(pidStatus);
    const NVFBCRESULT r = getStatus_(&status);
    if (r != NVFBC_SUCCESS) {
        error = L"NvFBC_GetStatusEx failed: " + ResultText(r);
        return false;
    }
    const bool pidSupported = (pidStatus.flags & 0x1u) != 0;
    const bool canCreatePid = (pidStatus.flags & 0x2u) != 0;
    const bool concurrency = (pidStatus.flags & 0x4u) != 0;
    wchar_t raw[32]{};
    swprintf_s(raw, L"0x%08X", pidStatus.flags);
    NvFbcDiagLog(L"ShadowPlay private status: flags=" + std::wstring(raw) + L", bPIDCaptureSupported=" +
                 std::to_wstring(pidSupported) + L", bCanCreatePIDSession=" + std::to_wstring(canCreatePid) +
                 L", concurrency=" + std::to_wstring(concurrency) + L".");
    possible = status.bIsCapturePossible != 0;
    canCreateNow = status.bCanCreateNow != 0;
    return true;
}

bool NvFbcCaptureBackend::EnsureEnabled(std::wstring &error) {
    bool possible = false, canCreateNow = false;
    if (!ReadStatus(possible, canCreateNow, error))
        return false;
    if (possible && canCreateNow)
        return true;
    if (!possible && enable_) {
        const NVFBCRESULT r = enable_(1);
        if (r == NVFBC_SUCCESS) {
            Sleep(700);
            if (!ReadStatus(possible, canCreateNow, error))
                return false;
        } else {
            error = L"NvFBC_Enable(ENABLE) failed: " + ResultText(r);
            return false;
        }
    }
    if (!possible || !canCreateNow) {
        error = L"NvFBC status does not permit a capture session (possible=" + std::to_wstring(possible) +
                L", canCreateNow=" + std::to_wstring(canCreateNow) + L").";
        return false;
    }
    return true;
}

bool NvFbcCaptureBackend::Init(HWND deviceWindow, DWORD targetPid, HMONITOR sourceMonitor,
                               const RECT &outputDesktop, const RECT &sourceClientRect,
                               UINT width, UINT height, std::wstring &error) {
    Shutdown();
    targetPid_ = targetPid;
    lastSourcePid_ = 0;
    pidCandidate_ = 0;
    gNvFbcDiagLastSourcePid = 0xffffffffu;
    gNvFbcDiagFrameLogged = false;
    NvFbcDiagLog(L"=== NvFBC PID diagnostic session start ===");
    NvFbcDiagLog(L"Capture process PID=" + std::to_wstring(GetCurrentProcessId()) +
                 L", selected target PID=" + std::to_wstring(targetPid_) +
                 L", adapter will be resolved from the selected game monitor.");
    width_ = width;
    height_ = height;
    acceptNextDimensions_ = true;
    sourceWidth_ = static_cast<UINT>(outputDesktop.right - outputDesktop.left);
    sourceHeight_ = static_cast<UINT>(outputDesktop.bottom - outputDesktop.top);
    if (!sourceWidth_ || !sourceHeight_) {
        error = L"NvFBC source monitor has invalid dimensions.";
        return false;
    }
    if (sourceClientRect.left < outputDesktop.left || sourceClientRect.top < outputDesktop.top) {
        error = L"NvFBC crop starts outside the selected monitor.";
        return false;
    }
    cropX_ = static_cast<UINT>(sourceClientRect.left - outputDesktop.left);
    cropY_ = static_cast<UINT>(sourceClientRect.top - outputDesktop.top);
    if (cropX_ + width_ > sourceWidth_ || cropY_ + height_ > sourceHeight_) {
        error = L"NvFBC crop rectangle extends outside the selected monitor.";
        return false;
    }

    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9_);
    if (FAILED(hr) || !d3d9_) {
        error = L"Direct3DCreate9Ex failed: " + HresultText(hr);
        return false;
    }
    bool found = false;
    for (UINT i = 0; i < d3d9_->GetAdapterCount(); ++i) {
        if (d3d9_->GetAdapterMonitor(i) == sourceMonitor) {
            adapterOrdinal_ = i;
            found = true;
            break;
        }
    }
    if (!found) {
        error = L"Could not map source monitor to a D3D9Ex adapter.";
        return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = deviceWindow ? deviceWindow : GetDesktopWindow();
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const DWORD behavior =
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
    hr = d3d9_->CreateDeviceEx(adapterOrdinal_, D3DDEVTYPE_HAL, pp.hDeviceWindow, behavior, &pp, nullptr,
                               &device9_);
    if (FAILED(hr) || !device9_) {
        error = L"D3D9Ex device creation failed: " + HresultText(hr);
        return false;
    }

    if (!LoadApi(error) || !EnsureEnabled(error)) return false;
    cuda_ = std::make_unique<CudaState>();
    auto &c=*cuda_;
    if (!c.api.Load(error) || !cuOk(c.api.init(0),L"cuInit",error) ||
        !cuOk(c.api.d3d9CtxCreate(&c.context,&c.device,0,device9_.Get()),L"cuD3D9CtxCreate",error)) return false;
    if(!CreateSession(error))return false;
    // Preserve the supplied build's first-grab-before-Streamline initialization order.
    if(!cuOk(c.api.memAlloc(&c.ptr[0],c.maxBytes),L"CUDA startup allocation",error))return false;
    LARGE_INTEGER stamp{};
    auto preflight=Grab(0,stamp,error);
    c.api.memFree(c.ptr[0]);c.ptr[0]=0;
    if(preflight!=NvFbcGrabResult::Frame)return false;
    Diagnostic(L"CUDA PID preflight validated before loading Streamline.");
    return true;
}

bool NvFbcCaptureBackend::CreateSession(std::wstring &error) {
    auto &c=*cuda_;
    if (!cuOk(c.api.ctxSetCurrent(c.context),L"CUDA context",error)) return false;
    if (c.session) { c.session->NvFBCCudaRelease(); c.session=nullptr; }
    // ABI negotiation is initialization only; every candidate is CUDA PID capture.
    // Once selected, session recovery reuses that exact ABI.
    const NvU32 ids[]={NVFBC_SHARED_CUDA_V3,NVFBC_SHARED_CUDA_V2};
    for (auto id:ids) {
        if(c.interfaceId && id!=c.interfaceId) continue;
        for(int candidate=0;candidate<2;++candidate) {
            if(c.interfaceId && candidate!=pidCandidate_) continue;
            NvFbcCreateParams cp{};
            cp.dwVersion=StructVersion(sizeof(cp),2); cp.dwInterfaceType=id;
            cp.pDevice=device9_.Get(); cp.cudaCtx=c.context; cp.dwAdapterIdx=adapterOrdinal_;
            NvU32 token[4]; memcpy(token,kCompatPrivateData,sizeof(token));
            cp.pPrivateData=token; cp.dwPrivateDataSize=sizeof(token);
            ShadowPlayPidCreatePrivate pid{};
            pid.dwVersion=candidate?0x500100C8u:StructVersion(sizeof(pid),1);
            pid.enable=1; pid.pid=targetPid_;
            cp.pPrivateData2=&pid; cp.dwPrivateData2Size=sizeof(pid);
            auto result=create_(&cp);
            if(result || !cp.pNvFBC) { error=L"CUDA PID create: "+ResultText(result); continue; }
            auto session=static_cast<NvFbcCudaSession *>(cp.pNvFBC);
            NvU32 bytes=0;
            result=session->NvFBCCudaGetMaxBufferSize(&bytes);
            if(!result && bytes) {
                if(id==NVFBC_SHARED_CUDA_V3) {
                    NvFbcCudaSetupV3 setup{}; setup.dwVersion=StructVersion(sizeof(setup),1);
                    setup.eFormat=NVFBC_TOCUDA_ARGB;
                    result=session->NvFBCCudaSetup(&setup);
                } else {
                    NvFbcCudaSetupV2 setup{}; setup.dwVersion=StructVersion(sizeof(setup),1);
                    result=session->NvFBCCudaSetup(&setup);
                }
            }
            if(result || !bytes || (c.maxBytes && bytes>c.maxBytes)) {
                session->NvFBCCudaRelease(); error=L"CUDA setup or capture allocation size changed."; continue;
            }
            c.session=session; c.maxBytes=bytes; c.interfaceId=id; pidCandidate_=candidate;
            Diagnostic(L"CUDA-only PID session active. Interface="+std::to_wstring(id)+
                L", PID layout="+std::to_wstring(candidate)+L", allocation="+std::to_wstring(bytes));
            return true;
        }
    }
    return false;
}

bool NvFbcCaptureBackend::Bind(ID3D12Device *device, ID3D12Fence *ready,
    ID3D12Resource **buffers, std::wstring &error) {
    auto &c=*cuda_;
    if(!cuOk(c.api.ctxSetCurrent(c.context),L"CUDA bind context",error)) return false;
    LUID luid{}; unsigned node=0;
    if(!cuOk(c.api.deviceLuid(reinterpret_cast<char *>(&luid),&node,c.device),L"CUDA adapter LUID",error)) return false;
    auto expected=device->GetAdapterLuid();
    if(memcmp(&luid,&expected,sizeof(luid)) || node!=1) {error=L"CUDA/D3D12 adapter mismatch or linked GPU unsupported.";return false;}
    for(unsigned i=0;i<4;++i) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=(uint64_t(c.maxBytes)+255)&~uint64_t(255); rd.Height=1;
        rd.DepthOrArraySize=rd.MipLevels=1; rd.SampleDesc.Count=1;
        rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_SHARED,&rd,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&buffers[i]));
        if(FAILED(hr)) {error=L"Shared CUDA capture buffer: "+HresultText(hr);return false;}
        HANDLE handle{};
        hr=device->CreateSharedHandle(buffers[i],nullptr,GENERIC_ALL,nullptr,&handle);
        if(FAILED(hr)) {error=L"Shared buffer handle: "+HresultText(hr);return false;}
        ExternalMemoryDesc desc{}; desc.type=5; desc.win32.handle=handle;
        desc.size=device->GetResourceAllocationInfo(0,1,&rd).SizeInBytes; desc.flags=1;
        auto result=c.api.importMemory(&c.memory[i],&desc); CloseHandle(handle);
        if(!cuOk(result,L"CUDA import D3D12 capture allocation",error)) return false;
        ExternalBufferDesc mapping{}; mapping.size=rd.Width;
        if(!cuOk(c.api.mapBuffer(&c.ptr[i],c.memory[i],&mapping),L"CUDA map capture pointer",error)) return false;
    }
    HANDLE handle{};
    auto hr=device->CreateSharedHandle(ready,nullptr,GENERIC_ALL,nullptr,&handle);
    if(FAILED(hr)) {error=L"CUDA ready fence handle: "+HresultText(hr);return false;}
    ExternalSemaphoreDesc sem{}; sem.type=4; sem.win32.handle=handle;
    auto result=c.api.importSemaphore(&c.semaphore,&sem); CloseHandle(handle);
    if(!cuOk(result,L"CUDA import ready fence",error)) return false;
    Diagnostic(L"CUDA DIRECT SHARED BUFFER active: NvFBC writes D3D12 allocation; no bridge copy.");
    return true;
}

NvFbcGrabResult NvFbcCaptureBackend::Grab(UINT slot, LARGE_INTEGER &timestamp, std::wstring &error) {
    auto &c=*cuda_;
    if(slot>=4 || !c.ptr[slot] || !c.session) {error=L"CUDA capture is not initialized.";return NvFbcGrabResult::Fatal;}
    if(!cuOk(c.api.ctxSetCurrent(c.context),L"CUDA capture context",error)) return NvFbcGrabResult::Fatal;
    NvFbcFrameGrabInfo info{}; NvFbcCudaGrabV1 grab{};
    grab.dwVersion=StructVersion(sizeof(grab),1); grab.dwFlags=NVFBC_TOCUDA_WAIT_WITH_TIMEOUT;
    grab.pCUDADeviceBuffer=reinterpret_cast<void *>(uintptr_t(c.ptr[slot]));
    grab.pNvFBCFrameGrabInfo=&info; grab.dwWaitTime=kGrabTimeoutMs;
    auto result=c.session->NvFBCCudaGrabFrame(&grab);
    QueryPerformanceCounter(&timestamp);
    if(result==NVFBC_ERROR_INVALIDATED_SESSION || info.bMustRecreate) {
        error=L"CUDA capture session invalidated."; return NvFbcGrabResult::Recreate;
    }
    if(result==NVFBC_ERROR_PROTECTED_CONTENT || info.bProtectedContent) {
        error=L"Capture reported protected content."; return NvFbcGrabResult::Protected;
    }
    if(result) {error=L"CUDA GrabFrame: "+ResultText(result);return NvFbcGrabResult::Fatal;}
    lastSourcePid_=info.dwSourcePID;
    if(lastSourcePid_!=targetPid_) {
        error=L"CUDA capture source PID does not match selected game."; return NvFbcGrabResult::Fatal;
    }
    if (!info.dwWidth || !info.dwHeight || info.dwBufferWidth < info.dwWidth ||
        uint64_t(info.dwBufferWidth) * 4 * info.dwHeight > c.maxBytes || info.bIsHDR) {
        error=L"CUDA requires a valid SDR capture frame; invalid dimensions/pitch or HDR frame.";
        return NvFbcGrabResult::Fatal;
    }
    const UINT framePitch = info.dwBufferWidth * 4;
    if (acceptNextDimensions_) {
        // The NvFBC PID frame is authoritative.  A game can switch render/display
        // resolution without the monitor DesktopCoordinates changing, so locking
        // the pipeline to the monitor size creates an endless restart loop.
        width_ = info.dwWidth;
        height_ = info.dwHeight;
        c.pitch = framePitch;
        acceptNextDimensions_ = false;
        Diagnostic(L"CUDA capture dimensions active: " + std::to_wstring(width_) + L"x" +
                   std::to_wstring(height_) + L", pitch=" + std::to_wstring(c.pitch) + L".");
        return NvFbcGrabResult::Frame;
    }
    if (info.dwWidth != width_ || info.dwHeight != height_ ||
        (c.pitch && c.pitch != framePitch)) {
        error=L"Capture resolution changed.";
        return NvFbcGrabResult::Resize;
    }
    if(!c.pitch)c.pitch=framePitch;
    return NvFbcGrabResult::Frame;
}
bool NvFbcCaptureBackend::Signal(uint64_t value, std::wstring &error) {
    ExternalSignal signal{}; signal.params.value=value;
    // NvFBCCudaGrabFrame uses its synchronous ABI (no NOWAIT flag).
    // Signal on the legacy CUDA stream after the grab; no CPU completion poll.
    const void *sem=cuda_->semaphore;
    return cuOk(cuda_->api.signal(&sem,&signal,1,nullptr),L"CUDA ready signal",error);
}
UINT NvFbcCaptureBackend::Pitch() const {return cuda_->pitch;}
UINT NvFbcCaptureBackend::Width() const {return width_;}
UINT NvFbcCaptureBackend::Height() const {return height_;}
bool NvFbcCaptureBackend::RecreateSession(std::wstring &error) {return CreateSession(error);}
void NvFbcCaptureBackend::Diagnostic(const std::wstring &line) const {NvFbcDiagLog(line);}
void NvFbcCaptureBackend::Shutdown() {
    cuda_.reset(); device9_.Reset(); d3d9_.Reset();
    if(module_) {FreeLibrary(module_);module_=nullptr;}
    getSdkVersion_=nullptr;getStatus_=nullptr;create_=nullptr;enable_=nullptr;
}
