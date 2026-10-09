#pragma once
#include "common.h"
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
class FrameGeneration {
    HMODULE dll{}, commonDll{};
    void** commonComputeSlot{};
    void* compute{};
    std::string bootstrapStatus{};
    bool initialized{};
    UINT index{}, activeFrame{}, width{}, height{};
    bool enabled{};
    sl::DLSSGOptions activeOptions{};
    sl::Constants baseConstants{};
    sl::Extent inputExtent{};
    sl::ViewportHandle viewport{0};
    sl::FrameToken *token{};
    PFun_slInit *initFn{};
    PFun_slShutdown *shutdownFn{};
    PFun_slSetD3DDevice *deviceFn{};
    PFun_slGetFeatureFunction *featureFn{};
    PFun_slIsFeatureSupported *supportedFn{};
    PFun_slGetNewFrameToken *tokenFn{};
    PFun_slSetConstants *constantsFn{};
    PFun_slSetTagForFrame *tagsFn{};
    PFun_slDLSSGSetOptions *optionsFn{};
    PFun_slDLSSGGetState *stateFn{};
    PFun_slReflexSetOptions *reflexFn{};
    PFun_slReflexSleep *sleepFn{};
    PFun_slPCLSetMarker *markerFn{};
    template <class T> void load(T &f, const char *n) {
        f = (T)GetProcAddress(dll, n);
        if (!f)
            throw std::runtime_error(std::string("Missing runtime export: ") + n);
    }
    template <class T> void feature(T &f, sl::Feature id, const char *n) {
        void *p{};
        ck(featureFn(id, n, p), n);
        f = (T)p;
        if (!f)
            throw std::runtime_error(n);
    }
    static void ck(sl::Result r, const char *n) {
        if (r != sl::Result::eOk)
            throw std::runtime_error(std::string(n) + ": Streamline " + std::to_string(int(r)));
    }

  public:
    using Factory = HRESULT(WINAPI *)(REFIID, void **);
    using Device = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    Factory createFactory{};
    Device createDevice{};
    void load(bool diagnostics);
    void bind(ID3D12Device *d);
    void* streamlineCompute() const {
        if (!compute)
            throw std::runtime_error("V10 CHI compute pointer was not captured: " + bootstrapStatus);
        return compute;
    }
    const std::string& streamlineBootstrapStatus() const { return bootstrapStatus; }
    UINT currentFrame() const { return activeFrame; }
    void configure(IDXGIAdapter1 *, UINT, UINT, UINT, bool);
    void pause(bool);
    void begin();
    void constants(bool reset);
    void tag(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *, ID3D12Resource *);
    void marker(sl::PCLMarker m) {
        if (enabled)
            ck(markerFn(m, *token), "Reflex marker");
    }
    sl::DLSSGState afterPresent();
    void shutdown();
    ~FrameGeneration() {
        shutdown();
        // Keep proxy vtables resident until all engine COM objects are released.
        if (dll)
            FreeLibrary(dll);
        if (commonDll)
            FreeLibrary(commonDll);
    }
};
